# Pipe-Based Customer Support Ticket System

Educational Linux/POSIX C project demonstrating customer-support ticket handling and parent/child process communication. Ticket records use fixed-size arrays and a bounded FIFO queue; **no linked lists are used**. The program includes timestamped audit logging and a small FIFO-style ticket queue for processing.

## Core application

The reception parent and support child exchange commands and replies over two anonymous pipes. Ticket data is stored in fixed-size arrays, requests are processed from a bounded FIFO queue, and ticket activity is written to a timestamped log file.

Build and run on Linux or a POSIX-compatible environment:

```sh
make
./ticket_system
```

Commands: `/new CUSTOMER|ISSUE|PRIORITY`, `/process`, `/queue`, `/list`, `/stats`, `/help`, `/quit`.

## IPC demos and chat logging

`make` builds C programs for the ticket system and Linux IPC demonstrations. The interactive chat demos append timestamped sent and received plaintext messages to `chat_log.txt` in the current working directory. Newly created log files use owner-only permissions, while existing logs keep their current file mode instead of being forced to a different permission set.

```sh
./ipc_advanced rooms [ROOM_NAME] # encrypted parent-child chat over named FIFOs
./ipc_advanced server           # three child agents; use /send ID MESSAGE
./ipc_advanced signal-chat      # two anonymous pipes and SIGUSR1 notifications
./ipc_advanced encrypt          # AES-256-GCM encryption round-trip demonstration
```

The encryption demo and room mode use OpenSSL; install the OpenSSL development package before building. Chat logs contain message contents, so keep the file private and remove it when no longer needed.

## Tests

`make test` runs the C unit tests, C integration tests, and the existing GoogleTest integration test in sequence. The unit and integration checks are implemented in C; GoogleTest remains the separate C++ layer.

The project targets Linux/POSIX, not native Windows. Tests and IPC demos require a working POSIX environment; the advanced executable also requires OpenSSL.
