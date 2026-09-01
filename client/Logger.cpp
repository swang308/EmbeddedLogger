// Logger.cpp - Implementation of the embedded debug logger.
//
//              Log entries are formatted with a timestamp, severity, source
//              location and message, then sent to a central log server over a
//              non-blocking UDP socket. A background receive thread listens
//              for "Set Log Level=<n>" commands from that server so the filter
//              severity can be changed at runtime without restarting.
//
//              Shared state (the filter level) is protected by a mutex because
//              it is written by the application thread via SetLogLevel(),
//              written by the receive thread on a server command, and read by
//              every Log() call.
//
// Shan-Yun Wang, Aug 2026
//
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "Logger.h"

// Address of the log server. Change SERVER_IP to the server machine's real IP
// address when running the two sides on separate machines.
#define SERVER_IP   "127.0.0.1"
#define SERVER_PORT 6000
#define BUF_LEN     1024

static int             logFd = -1;        // UDP socket to the server
static struct sockaddr_in serverAddr;     // where log entries are sent
static LOG_LEVEL       filterLevel = DEBUG;  // shared: guarded by logMutex
static pthread_mutex_t logMutex;
static bool            is_running = false;   // drives the receive thread loop
static pthread_t       recvThreadId;

static void *recvFunc(void *arg);

// Creates the UDP socket, makes it non-blocking, records the server address,
// creates the mutex, and starts the receive thread.
// Returns 0 on success, -1 if the socket or thread could not be created.
int InitializeLog()
{
    logFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (logFd < 0) {
        perror("Logger: socket");
        return -1;
    }

    // Non-blocking so the receive thread's recvfrom() returns immediately when
    // no command is waiting, rather than parking the thread indefinitely.
    int flags = fcntl(logFd, F_GETFL, 0);
    if (fcntl(logFd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("Logger: fcntl");
        close(logFd);
        logFd = -1;
        return -1;
    }

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port   = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &serverAddr.sin_addr) != 1) {
        perror("Logger: inet_pton");
        close(logFd);
        logFd = -1;
        return -1;
    }

    pthread_mutex_init(&logMutex, NULL);

    is_running = true;
    if (pthread_create(&recvThreadId, NULL, recvFunc, &logFd) != 0) {
        perror("Logger: pthread_create");
        is_running = false;
        pthread_mutex_destroy(&logMutex);
        close(logFd);
        logFd = -1;
        return -1;
    }

    return 0;
}

// Sets the filter severity. Guarded because Log() reads this value from the
// application thread while the receive thread may be updating it.
void SetLogLevel(LOG_LEVEL level)
{
    pthread_mutex_lock(&logMutex);
    filterLevel = level;
    pthread_mutex_unlock(&logMutex);
}

// Discards the entry if it falls below the filter severity; otherwise builds
// "<timestamp> <LEVEL> <file>:<func>:<line> <message>" and sends it to the
// server. The mutex is held only long enough to copy the filter level out, so
// formatting and sendto() never block the rest of the application.
void Log(LOG_LEVEL level, const char *prog, const char *func, int line, const char *message)
{
    if (logFd < 0) return;

    pthread_mutex_lock(&logMutex);
    LOG_LEVEL current = filterLevel;
    pthread_mutex_unlock(&logMutex);

    if (level < current) return;   // below the filter - throw it away

    time_t now = time(0);
    char *dt = ctime(&now);
    // ctime() terminates its string with a newline; strip it so the timestamp
    // does not break the log entry across two lines.
    size_t dtLen = strlen(dt);
    if (dtLen > 0 && dt[dtLen - 1] == '\n') dt[dtLen - 1] = '\0';

    const char levelStr[][16] = {"DEBUG", "WARNING", "ERROR", "CRITICAL"};

    char buf[BUF_LEN];
    memset(buf, 0, BUF_LEN);
    int len = snprintf(buf, BUF_LEN, "%s %s %s:%s:%d %s\n",
                       dt, levelStr[level], prog, func, line, message);
    if (len < 0) return;
    if (len > BUF_LEN - 1) len = BUF_LEN - 1;   // message was truncated

    if (sendto(logFd, buf, len, 0,
               (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        // UDP is fire-and-forget; a failed send must not stall the application.
        perror("Logger: sendto");
    }
}

// Stops the receive thread, waits for it to finish, then releases the socket
// and mutex. Joining before close() prevents the thread from using a file
// descriptor that has already been closed.
void ExitLog()
{
    if (logFd < 0) return;

    is_running = false;
    pthread_join(recvThreadId, NULL);

    close(logFd);
    logFd = -1;
    pthread_mutex_destroy(&logMutex);
}

// Receive thread: watches for commands from the server. The only command
// currently defined is "Set Log Level=<n>", which overwrites the filter
// severity. Sleeps one second whenever nothing is waiting so the loop does not
// spin on the non-blocking socket.
static void *recvFunc(void *arg)
{
    int fd = *(int *)arg;
    char buf[BUF_LEN];

    while (is_running) {
        memset(buf, 0, BUF_LEN);
        ssize_t len = recvfrom(fd, buf, BUF_LEN - 1, 0, NULL, NULL);

        if (len > 0) {
            buf[len] = '\0';
            int newLevel;
            if (sscanf(buf, "Set Log Level=%d", &newLevel) == 1) {
                if (newLevel >= DEBUG && newLevel <= CRITICAL) {
                    SetLogLevel((LOG_LEVEL)newLevel);
                    printf("Logger: filter level set to %d by server\n", newLevel);
                    fflush(stdout);
                }
            }
        } else {
            sleep(1);   // nothing waiting - back off before polling again
        }
    }

    return NULL;
}
