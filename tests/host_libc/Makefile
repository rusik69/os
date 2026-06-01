# host_libc Makefile
# Compiles kernel libc sources with host gcc for unit testing on Linux x86_64

CC = gcc
CFLAGS = -DTEST_MODE_HOST \
         -I../../src/include \
         -I../../src \
         -Wall -Werror \
         -Wno-format-truncation \
         -g -O0

# Source files — only compile the pure-logic libc implementation files.
# libc.c is excluded because it contains only syscall wrappers (kernel-specific).
SRCDIR = ../../src/lib
OBJS = stubs.o string.o stdlib.o printf.o test_libc.o

.PHONY: all clean

all: test_libc
	@echo ""
	@echo "Running host_libc unit tests..."
	./test_libc

test_libc: $(OBJS)
	$(CC) -o $@ $^

stubs.o: stubs.c
	$(CC) -DTEST_MODE_HOST -g -O0 -c -o $@ $<

string.o: $(SRCDIR)/string.c
	$(CC) $(CFLAGS) -c -o $@ $<

stdlib.o: $(SRCDIR)/stdlib.c
	$(CC) $(CFLAGS) -c -o $@ $<

printf.o: $(SRCDIR)/printf.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_libc.o: test_libc.c
	$(CC) $(CFLAGS) \
	  -Wno-memset-transposed-args \
	  -Wno-format \
	  -c -o $@ $<

clean:
	rm -f $(OBJS) test_libc
