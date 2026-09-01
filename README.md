# EmbeddedLogger

A lightweight, multi-threaded logging library and companion log server for real-time C++ applications. Originally built as coursework for a UNIX/Linux systems programming course (Seneca Polytechnic, UNX511).

## Overview

Application code calls a small logging API instead of touching sockets directly:

```cpp
InitializeLog();
SetLogLevel(DEBUG);
Log(DEBUG, __FILE__, __func__, __LINE__, "message");
ExitLog();
```

`Log()` runs synchronously on the caller's own thread, so it's built to add minimal latency: it briefly locks a mutex to read the current filter severity, releases it immediately, then formats and sends the entry over a UDP socket to a separate log server — it never blocks the caller on the network. A background thread in the same process listens for a remote "set log level" command from the server, so an operator can change what severity gets logged (`DEBUG` / `WARNING` / `ERROR` / `CRITICAL`) without restarting the application.

The server is a second process — designed to run on a separate machine — with its own receive thread that appends incoming log entries to a file, plus a small console menu for changing the client's log level remotely or dumping the log to screen.

## Architecture

```
   APPLICATION PROCESS                          SERVER PROCESS
 ┌──────────────────────────┐            ┌──────────────────────────┐
 │  main thread             │            │  main thread (menu)      │
 │    calls Log(...)        │            │    1. set log level      │
 │      │                   │            │    2. dump log file      │
 │      v                   │            │                          │
 │  Logger: check filter    │   UDP      │                          │
 │  format, sendto() ───────┼───────────>│  receive thread          │
 │                          │            │    writes to log file    │
 │  receive thread <────────┼────────────┤    saves sender address  │
 │    updates filter level  │   UDP      │                          │
 └──────────────────────────┘            └──────────────────────────┘
        mutex: filter level                      mutex: log file +
        (main writes/reads,                      last client address
         recv thread writes)
```

## Highlights

- Two-process, four-thread system communicating over UDP sockets
- POSIX mutexes protect every piece of state shared between a main thread and its background receive thread (filter level; last-known client address; the log file itself) — without ever locking around the blocking socket calls themselves
- Clean shutdown on `SIGINT`: threads are joined before their sockets are closed, so nothing is left dangling
- Runtime-adjustable log severity, driven remotely from the server's console menu, with no restart required

## Project layout

```
EmbeddedLogger/
├── README.md
├── .gitignore
├── client/             # the application process
│   ├── Makefile
│   ├── Logger.h        # the logging library
│   ├── Logger.cpp
│   ├── Automobile.h    # demo class exercised by the simulator
│   ├── Automobile.cpp
│   └── TravelSimulator.cpp   # demo app: calls Log() while "driving" cars
└── server/             # the log server process
    ├── Makefile
    └── LogServer.cpp
```

`Logger.h`/`Logger.cpp` are the reusable part of the project — a small API any application can call. `Automobile`/`TravelSimulator` are a demo consumer that exercises it under a repeating workload.

## Build & Run

```bash
# Application side
cd client
make all
./travel

# Server side (separate terminal, or separate machine)
cd server
make all
./server
```

Set `SERVER_IP` / `SERVER_PORT` in `client/Logger.cpp` to match the server's address before running across two machines.

## Tech

C++ · POSIX Threads (pthreads) · BSD sockets (UDP) · Makefile · Linux
