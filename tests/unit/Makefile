# tests/unit/Makefile
# Compiles host-side unit tests for kernel memory allocator algorithms
#
# Usage:  make          → builds and runs tests
#         make test     → same
#         make clean    → remove binaries

CC      = gcc
CFLAGS  = -Wall -Werror -g -O0

.PHONY: all clean test

all: test

test: test_memory
	@echo ""
	@echo "Running memory allocator unit tests..."
	@./test_memory

test_memory: test_memory.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f test_memory
