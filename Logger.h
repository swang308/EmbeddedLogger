// Logger.h - Header file for the embedded debug logger.
//
//            Exposes the logging interface used by application processes.
//            Each process calls InitializeLog() once at startup, SetLogLevel()
//            to choose a filter severity, Log() wherever it wants to report
//            activity, and ExitLog() before terminating.
//
//            Log messages at or above the current filter severity are sent to
//            a central log server over UDP. Messages below it are discarded.
//
// 06-Aug-26  Shan-Yun Wang (133159228)   Created.
//
#ifndef LOGGER_H
#define LOGGER_H

// Severity levels, ordered least to most severe. The numeric ordering matters:
// Log() keeps a message only when its level is >= the current filter level.
enum LOG_LEVEL {
    DEBUG = 0,
    WARNING,
    ERROR,
    CRITICAL
};

// Sets up the logger for this process: creates a non-blocking UDP socket,
// resolves the server address, creates the mutex, and starts the receive
// thread. Returns 0 on success, -1 on failure.
int InitializeLog();

// Sets the filter severity. Any subsequent Log() call below this level is
// discarded. Safe to call from any thread.
void SetLogLevel(LOG_LEVEL level);

// Formats and sends one log entry to the server, if its severity is at or
// above the current filter level.
//   level   - severity of this entry
//   prog    - source file name    (pass __FILE__)
//   func    - function name       (pass __func__)
//   line    - line number         (pass __LINE__)
//   message - free-form message text
void Log(LOG_LEVEL level, const char *prog, const char *func, int line, const char *message);

// Shuts the logger down: stops the receive thread, joins it, and closes the
// socket. Call once before the process exits.
void ExitLog();

#endif //LOGGER_H
