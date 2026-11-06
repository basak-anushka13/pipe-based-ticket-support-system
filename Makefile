CC = gcc
CXX = g++
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -O2
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2
LDLIBS_IPC ?= -pthread -lrt
LDLIBS_CRYPTO ?= -lcrypto
BUILD_DIR := build
COVERAGE_DIR := $(BUILD_DIR)/coverage

.PHONY: all clean run extensions test unit integration advanced-integration gtest cppcheck misra \
        coverage function_coverage branch_coverage valgrind helgrind

all: ticket_system ipc_extensions ipc_advanced

ticket_system: main.c
	$(CC) $(CFLAGS) -o $@ $<

ipc_extensions: ipc_extensions.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS_IPC)

ipc_advanced: ipc_advanced.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS_CRYPTO)

extensions: ipc_extensions ipc_advanced

run: ticket_system
	./ticket_system

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/test_ticket: tests/test_ticket.c main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ tests/test_ticket.c

$(BUILD_DIR)/integration_test: tests/integration_test.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ tests/integration_test.c

unit: $(BUILD_DIR)/test_ticket
	(cd $(BUILD_DIR) && ./test_ticket)

integration: ticket_system ipc_advanced $(BUILD_DIR)/integration_test
	./$(BUILD_DIR)/integration_test $(CURDIR)/ticket_system $(CURDIR)/ipc_advanced

advanced-integration: integration

# Requires GoogleTest development headers and libraries to be installed.
gtest: ticket_system | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) tests/test_gtest.cpp -o $(BUILD_DIR)/test_gtest -lgtest -lgtest_main -pthread
	TICKET_SYSTEM_BIN=$(CURDIR)/ticket_system ./$(BUILD_DIR)/test_gtest

# Run the C unit and integration suites, then the existing optional C++/GoogleTest suite.
test:
	$(MAKE) unit
	$(MAKE) integration
	$(MAKE) gtest

cppcheck:
	cppcheck --error-exitcode=1 --enable=warning,style,performance,portability \
		--std=c11 main.c ipc_extensions.c ipc_advanced.c tests/test_ticket.c tests/integration_test.c 2> cppcheck_report.txt

# Provide a cppcheck MISRA addon/configuration, e.g. MISRA_ADDON=misra.json.
# This is a rule check, not certification of MISRA compliance.
MISRA_ADDON ?=
misra:
	@test -n "$(MISRA_ADDON)" || (echo "Set MISRA_ADDON to a cppcheck MISRA addon/configuration" >&2; exit 2)
	cppcheck --error-exitcode=1 --enable=all --std=c11 --addon=$(MISRA_ADDON) \
		main.c ipc_extensions.c ipc_advanced.c tests/test_ticket.c tests/integration_test.c 2> misra_report.txt

# Coverage is measured for main.c; integration and unit tests are implemented in C.
coverage: ticket_system $(BUILD_DIR)/test_ticket $(BUILD_DIR)/integration_test | $(COVERAGE_DIR)
	rm -f $(COVERAGE_DIR)/*.gcda $(COVERAGE_DIR)/*.gcno
	$(CC) $(CFLAGS) -O0 -g --coverage -o $(COVERAGE_DIR)/ticket_system main.c
	$(CC) $(CFLAGS) -O0 -g --coverage -o $(COVERAGE_DIR)/test_ticket tests/test_ticket.c
	(cd $(COVERAGE_DIR) && ./test_ticket)
	./$(BUILD_DIR)/integration_test $(CURDIR)/$(COVERAGE_DIR)/ticket_system $(CURDIR)/ipc_advanced
	gcovr --root $(CURDIR) --object-directory $(CURDIR)/$(COVERAGE_DIR) \
		--filter '.*main[.]c$$' --txt --output $(COVERAGE_DIR)/coverage_report.txt \
		--fail-under-line 95 --fail-under-branch 95 --fail-under-function 95

function_coverage branch_coverage: coverage

$(COVERAGE_DIR): | $(BUILD_DIR)
	mkdir -p $@

valgrind: ticket_system
	@tmp=$$(mktemp -d); \
	(cd "$$tmp" && printf '%s\n' '/new Valgrind|memory check|1' '/process' '/quit' | \
		valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 $(CURDIR)/ticket_system); \
	status=$$?; rm -rf "$$tmp"; exit $$status

# Helgrind checks thread synchronization; this application primarily uses processes.
helgrind: ticket_system
	@tmp=$$(mktemp -d); \
	(cd "$$tmp" && printf '%s\n' '/new Helgrind|IPC check|1' '/process' '/quit' | \
		valgrind --tool=helgrind --error-exitcode=1 $(CURDIR)/ticket_system); \
	status=$$?; rm -rf "$$tmp"; exit $$status

clean:
	rm -f ticket_system ipc_extensions ipc_advanced cppcheck_report.txt misra_report.txt
	rm -f *.gcda *.gcno *.gcov ticket_log.txt chat_log.txt
	rm -rf $(BUILD_DIR)
