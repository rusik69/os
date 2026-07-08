# tests/unit/Makefile
# Compiles host-side unit tests for kernel algorithms
#
# Usage:  make          → builds and runs all tests
#         make test     → same
#         make clean    → remove binaries

CC      = gcc
CFLAGS  = -Wall -Werror -Wundef -Wmisleading-indentation -Wduplicated-branches -g -O0

.PHONY: all clean test

all: test

test: test_memory test_fs test_socket test_list test_string test_bitmap
ifeq ($(JUNIT_DIR),)
	@echo ""
	@echo "Running memory allocator unit tests..."
	@./test_memory
	@echo ""
	@echo "Running filesystem operation unit tests..."
	@./test_fs
	@echo ""
	@echo "Running socket operation unit tests..."
	@./test_socket
	@echo ""
	@echo "Running linked list and hash table unit tests..."
	@./test_list
	@echo ""
	@echo "Running string operation unit tests..."
	@./test_string
	@echo ""
	@echo "Running bit manipulation unit tests..."
	@./test_bitmap
else
	@echo ""
	@echo "Running unit tests (JUnit output to $(JUNIT_DIR))..."
	python3 ../../tests/junit_reporter.py unit-memory ./test_memory
	python3 ../../tests/junit_reporter.py unit-fs ./test_fs
	python3 ../../tests/junit_reporter.py unit-socket ./test_socket
	python3 ../../tests/junit_reporter.py unit-list ./test_list
	python3 ../../tests/junit_reporter.py unit-string ./test_string
	python3 ../../tests/junit_reporter.py unit-bitmap ./test_bitmap
	@mv -f unit-memory-results.xml unit-fs-results.xml \
	      unit-socket-results.xml unit-list-results.xml \
	      unit-string-results.xml unit-bitmap-results.xml \
	      "$(JUNIT_DIR)/" 2>/dev/null || true
	@echo "JUnit XML: $(JUNIT_DIR)/"
endif

test_memory: test_memory.c
	$(CC) $(CFLAGS) -o $@ $<

test_fs: test_fs.c
	$(CC) $(CFLAGS) -o $@ $<

test_socket: test_socket.c
	$(CC) $(CFLAGS) -o $@ $<

test_list: test_list.c
	$(CC) $(CFLAGS) -o $@ $<

test_string: test_string.c
	$(CC) $(CFLAGS) -o $@ $<

test_bitmap: test_bitmap.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f test_memory test_fs test_socket test_list test_string test_bitmap
