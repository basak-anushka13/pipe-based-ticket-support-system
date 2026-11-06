CC ?= gcc
CXX ?= g++
PYTHON ?= python3
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2
LDLIBS_IPC ?= -pthread -lrt
BUILD_DIR ?= build
COVERAGE_DIR := $(BUILD_DIR)/coverage

.PHONY: all clean run extensions test unit integration gtest cppcheck misra valgrind helgrind coverage
all: ticket_system ipc_extensions

ticket_system: main.c
	$(CC) $(CFLAGS) -o $@ $<

ipc_extensions: ipc_extensions.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS_IPC)

extensions: ipc_extensions

run: ticket_system
	./ticket_system

unit: $(BUILD_DIR)/test_ticket
	./$(BUILD_DIR)/test_ticket

$(BUILD_DIR)/test_ticket: tests/test_ticket.c main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ tests/test_ticket.c

$(BUILD_DIR):
	mkdir -p $@

test: ticket_system unit integration

integration: ticket_system
	$(PYTHON) tests/integration_test.py ./ticket_system

# Optional C++ integration tests. Install GoogleTest (libgtest-dev on Debian/Ubuntu).
gtest: ticket_system
	$(CXX) $(CXXFLAGS) tests/test_gtest.cpp -o $(BUILD_DIR)/test_gtest -lgtest -lgtest_main -pthread
	TICKET_SYSTEM_BIN=./ticket_system ./$(BUILD_DIR)/test_gtest

cppcheck:
	cppcheck --error-exitcode=1 --enable=warning,style,performance,portability --std=c11 main.c ipc_extensions.c

# cppcheck does not provide complete MISRA certification. Supply a compatible
# MISRA addon (and the applicable rules configuration) with MISRA_ADDON=path.
MISRA_ADDON ?=
misra:
	@test -n "$(MISRA_ADDON)" || (echo "Set MISRA_ADDON to your cppcheck MISRA addon/configuration" >&2; exit 2)
	cppcheck --error-exitcode=1 --addon=$(MISRA_ADDON) --std=c11 main.c ipc_extensions.c

valgrind: ticket_system
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./ticket_system </dev/null

# Helgrind is aimed at thread synchronization; the core application uses processes.
helgrind: ticket_system
	valgrind --tool=helgrind --error-exitcode=1 ./ticket_system </dev/null

coverage: | $(COVERAGE_DIR)
	$(CC) $(CFLAGS) --coverage -O0 -o $(COVERAGE_DIR)/ticket_system main.c
	$(CC) $(CFLAGS) --coverage -O0 -o $(COVERAGE_DIR)/test_ticket tests/test_ticket.c
	./$(COVERAGE_DIR)/test_ticket
	$(PYTHON) tests/integration_test.py ./$(COVERAGE_DIR)/ticket_system
	gcovr --root . --object-directory $(COVERAGE_DIR) --filter 'main\\.c$$' \
		--fail-under-line 95 --fail-under-branch 95 --fail-under-function 95

$(COVERAGE_DIR):
	mkdir -p $@

clean:
	rm -f ticket_system ipc_extensions
	rm -rf $(BUILD_DIR) *.gcda *.gcno *.gcov ticket_log.txt
