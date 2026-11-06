# Linux C testing and code-quality targets

The project targets Linux/POSIX. Ticket data uses fixed-size arrays and a bounded FIFO queue—**no linked lists are used**. Production programs and unit/integration tests are written in C; the existing optional GoogleTest integration test remains C++.

## Build and demos

`make` builds the ticket program and IPC demos. Install OpenSSL development headers/libraries for `ipc_advanced` (linked with `-lcrypto`).

- `./ipc_advanced rooms ROOM_NAME` — named-FIFO parent/child room using AES-256-GCM authenticated messages.
- `./ipc_advanced server` — three persistent child agents, addressed with `/send ID MESSAGE`.
- `./ipc_advanced signal-chat` — pipe chat with SIGUSR1 response notifications.
- `./ipc_advanced encrypt` — standalone AES-256-GCM round-trip demonstration.

The chat demos append timestamped sent/received message text to `chat_log.txt`, with owner-only permissions. This log contains plaintext; the encrypted FIFO demo logs only after local encryption/decryption and does not record passphrases or ciphertext.

## Make targets

- `make test` — run the C unit tests, C integration tests, and GoogleTest integration tests in order. Python is not used by the build or test targets.
- `make gtest` — build and run the existing C++ GoogleTest executable test; requires GoogleTest development headers and libraries.
- `make cppcheck` — static analysis.
- `make misra MISRA_ADDON=...` — run a supplied cppcheck MISRA addon/configuration; this is not certification.
- `make valgrind` / `make helgrind` — run memory/synchronization tooling on the core ticket process.
- `make coverage` — measure core `main.c` coverage and enforce at least 95% line, branch, and function coverage. This is a configured threshold, not a measured result until the target is run.

GoogleTest, OpenSSL, cppcheck, Valgrind, gcovr, and a MISRA addon are external Linux dependencies. Helgrind primarily checks thread synchronization; these examples communicate with processes.
