// LogServer.cpp - Central log server for the embedded debug logger.
//
//                 Binds a non-blocking UDP socket and starts a receive thread
//                 that appends every incoming log entry to a common server log
//                 file. The main thread presents a user menu allowing the log
//                 file to be dumped to the screen and the loggers' filter
//                 severity to be overwritten at runtime.
//
//                 The log file and the saved client address are shared between
//                 the receive thread and the menu thread, so both are guarded
//                 by a mutex.
//
// Shan-Yun Wang, Aug 2026
//
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace std;

#define SERVER_PORT  6000
#define BUF_LEN      1024
#define LOG_FILENAME "ServerLog.txt"

static bool            is_running = true;   // drives both the menu and receive loops
static pthread_mutex_t fileMutex;

// Address of the most recent logger to send us an entry. Recorded by the
// receive thread and read by the menu when sending a "Set Log Level" command,
// which is why it is guarded by fileMutex.
static struct sockaddr_in lastClientAddr;
static bool               haveClient = false;

static void *recvFunc(void *arg);

// Ctrl-C handler. Only flips the running flag; all teardown happens in main()
// so nothing unsafe is performed inside signal context.
static void shutdownHandler(int sig)
{
    if (sig == SIGINT) {
        is_running = false;
    }
}

// Creates and binds the socket, starts the receive thread, then runs the user
// menu until the user selects "Shut down" or presses Ctrl-C.
int main()
{
    // sa_flags = 0 (no SA_RESTART) so Ctrl-C interrupts the blocking menu read
    // instead of silently restarting it, letting the menu loop notice the flag.
    struct sigaction action;
    action.sa_handler = shutdownHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, NULL);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("LogServer: socket");
        return -1;
    }

    // Non-blocking so the receive thread polls rather than parking forever.
    int flags = fcntl(fd, F_GETFL, 0);
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("LogServer: fcntl");
        close(fd);
        return -1;
    }

    struct sockaddr_in myAddr;
    memset(&myAddr, 0, sizeof(myAddr));
    myAddr.sin_family      = AF_INET;
    myAddr.sin_port        = htons(SERVER_PORT);
    myAddr.sin_addr.s_addr = INADDR_ANY;   // accept on any local interface

    if (bind(fd, (struct sockaddr *)&myAddr, sizeof(myAddr)) < 0) {
        perror("LogServer: bind");
        close(fd);
        return -1;
    }

    pthread_mutex_init(&fileMutex, NULL);

    pthread_t recvThreadId;
    if (pthread_create(&recvThreadId, NULL, recvFunc, &fd) != 0) {
        perror("LogServer: pthread_create");
        pthread_mutex_destroy(&fileMutex);
        close(fd);
        return -1;
    }

    cout << "LogServer: listening on port " << SERVER_PORT << endl;

    while (is_running) {
        cout << "\n1. Set the log level" << endl;
        cout << "2. Dump the log file here" << endl;
        cout << "0. Shut down" << endl;
        cout << "> " << flush;

        int choice;
        if (!(cin >> choice)) {
            // Interrupted by Ctrl-C, or stdin closed/garbage entered.
            if (!is_running) break;
            if (cin.eof()) { is_running = false; break; }
            cin.clear();
            cin.ignore(numeric_limits<streamsize>::max(), '\n');
            cout << "Please enter 0, 1 or 2." << endl;
            continue;
        }

        if (choice == 0) {
            is_running = false;
        }
        else if (choice == 1) {
            int level;
            cout << "Enter filter level (0=DEBUG 1=WARNING 2=ERROR 3=CRITICAL): " << flush;
            if (!(cin >> level)) {
                if (!is_running) break;
                cin.clear();
                cin.ignore(numeric_limits<streamsize>::max(), '\n');
                cout << "Invalid level." << endl;
                continue;
            }
            if (level < 0 || level > 3) {
                cout << "Level must be between 0 and 3." << endl;
                continue;
            }

            // Copy the destination out under the lock, then send outside it.
            pthread_mutex_lock(&fileMutex);
            bool ok = haveClient;
            struct sockaddr_in dest = lastClientAddr;
            pthread_mutex_unlock(&fileMutex);

            if (!ok) {
                cout << "No logger has sent anything yet - no address to reply to." << endl;
            } else {
                char buf[BUF_LEN];
                memset(buf, 0, BUF_LEN);
                int len = sprintf(buf, "Set Log Level=%d", level);
                if (sendto(fd, buf, len, 0,
                           (struct sockaddr *)&dest, sizeof(dest)) < 0) {
                    perror("LogServer: sendto");
                } else {
                    cout << "Sent: " << buf << endl;
                }
            }
        }
        else if (choice == 2) {
            // Hold the lock while reading so the receive thread cannot append
            // mid-dump and interleave a partial entry into the output.
            pthread_mutex_lock(&fileMutex);
            int rfd = open(LOG_FILENAME, O_RDONLY);
            if (rfd < 0) {
                cout << "No log file yet." << endl;
            } else {
                char buf[BUF_LEN];
                ssize_t n;
                cout << "----- " << LOG_FILENAME << " -----" << endl;
                while ((n = read(rfd, buf, BUF_LEN)) > 0) {
                    if (write(STDOUT_FILENO, buf, n) < 0) break;
                }
                close(rfd);
                cout << "----- end of log -----" << endl;
            }
            pthread_mutex_unlock(&fileMutex);

            cout << "Press any key to continue: " << flush;
            cin.ignore(numeric_limits<streamsize>::max(), '\n');
            cin.get();
        }
        else {
            cout << "Please enter 0, 1 or 2." << endl;
        }
    }

    // Stop and join the receive thread before releasing the socket it uses.
    is_running = false;
    pthread_join(recvThreadId, NULL);

    close(fd);
    pthread_mutex_destroy(&fileMutex);

    cout << "\nLogServer: shut down." << endl;
    return 0;
}

// Receive thread: appends every incoming datagram to the server log file and
// remembers who sent it so the menu can reply. Sleeps one second when nothing
// is waiting so the loop does not spin on the non-blocking socket.
static void *recvFunc(void *arg)
{
    int fd = *(int *)arg;

    // rw-rw-rw- as required; O_APPEND so entries accumulate across runs.
    int logFileFd = open(LOG_FILENAME, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (logFileFd < 0) {
        perror("LogServer: open log file");
        return NULL;
    }

    char buf[BUF_LEN];

    while (is_running) {
        struct sockaddr_in fromAddr;
        socklen_t fromLen = sizeof(fromAddr);

        memset(buf, 0, BUF_LEN);
        ssize_t len = recvfrom(fd, buf, BUF_LEN - 1, 0,
                               (struct sockaddr *)&fromAddr, &fromLen);

        if (len > 0) {
            pthread_mutex_lock(&fileMutex);
            if (write(logFileFd, buf, len) < 0) {
                perror("LogServer: write");
            }
            lastClientAddr = fromAddr;   // remember where to send commands
            haveClient     = true;
            pthread_mutex_unlock(&fileMutex);
        } else {
            sleep(1);   // nothing waiting - back off before polling again
        }
    }

    close(logFileFd);
    return NULL;
}
