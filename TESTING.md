# Testing and quality checks

The project remains implemented in C and uses fixed-size ticket storage plus a bounded FIFO queue. **It does not use linked lists.** The primary test suite is native C because GoogleTest is a C++ framework and is not a natural unit-test dependency for this C program.

## Targets

- `make test` — builds and runs native unit tests and process-level integration tests.
- `make unit` / `make integration` — run either suite separately.
- `make gtest` — optional GoogleTest-based integration tests; install GoogleTest first (for example, `libgtest-dev` on Debian/Ubuntu).
- `make cppcheck` — static analysis using cppcheck.
- `make misra MISRA_ADDON=/path/to/addon.py` — invokes a supplied cppcheck MISRA addon/configuration. This is a rule check, **not** a claim of certified MISRA compliance; applicable rules and a compatible addon must be supplied.
- `make valgrind` — runs the application under Valgrind with EOF input.
- `make helgrind` — runs under Helgrind. Helgrind primarily detects thread synchronization issues; this application communicates between processes, so the result does not replace process/IPC-specific testing.
- `make coverage` — builds instrumented core and test binaries, runs the suites, and asks gcovr to enforce at least 95% line, branch, and function coverage for `main.c`.

## Dependencies and limitations

The core project requires GCC and a Linux/POSIX environment. The quality targets are optional and need their tools installed: `cppcheck`, Valgrind, `gcovr`, and (for `make gtest`) GoogleTest plus a C++ compiler. The MISRA target also needs a compatible addon and rule configuration. These tools are not downloaded automatically.

Coverage percentages are measured results, not guaranteed by adding a Make target. The threshold is deliberately enforced by `make coverage`; if it fails, add tests for the uncovered lines/branches/functions and rerun it. I have not run these commands in this GitHub-only environment, so their results must be verified on a Linux machine with the dependencies installed. No claim of >95% coverage or MISRA compliance should be made until the relevant target passes with recorded output.
