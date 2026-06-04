# host_libc Makefile
# Compiles kernel libc sources with host gcc for unit testing on Linux x86_64

CC = gcc

# Flags for kernel source files (need kernel include paths)
KERNEL_CFLAGS = -DTEST_MODE_HOST \
                -isystem ../../src/include \
                -isystem ../../src \
                -Wall -Werror \
                -Wno-error=implicit-function-declaration \
                -Wno-format-truncation \
                -g -O0

# Flags for test files (use system headers, NOT kernel headers)
TEST_CFLAGS = -DTEST_MODE_HOST \
              -Wall -Werror \
              -g -O0

# Source files — only compile the pure-logic libc implementation files.
# libc.c is excluded because it contains only syscall wrappers (kernel-specific).
SRCDIR = ../../src/lib
OBJS = stubs.o string.o stdlib.o printf.o string_ext.o test_libc.o

# Additional source files for crypto/bitmap tests
CRYPTODIR = ../../src/lib
CRYPTO_OBJS = bitmap.o crc32.o crc64.o sha256.o test_bitops_crc_sha.o

.PHONY: all clean

all: test_libc test_bitops_crc_sha
	@echo ""
	@echo "Running host_libc unit tests..."
	./test_libc
	@echo ""
	@echo "Running host-side crypto/bitmap tests..."
	./test_bitops_crc_sha

test_libc: $(OBJS)
	$(CC) -o $@ $^

stubs.o: stubs.c
	$(CC) -DTEST_MODE_HOST -g -O0 -c -o $@ $<

string.o: $(SRCDIR)/string.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

stdlib.o: $(SRCDIR)/stdlib.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

printf.o: $(SRCDIR)/printf.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

string_ext.o: $(SRCDIR)/string_ext.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_libc.o: test_libc.c
	$(CC) $(TEST_CFLAGS) \
	  -Wno-memset-transposed-args \
	  -Wno-format \
	  -Wno-stringop-truncation \
	  -Wno-stringop-overflow \
	  -Wno-stringop-overread \
	  -c -o $@ $<

# ── Host-side bitmap/crypto/SHA test ─────────────────────────────

test_bitops_crc_sha: $(CRYPTO_OBJS) printf.o stubs.o
	$(CC) -o $@ $^

bitmap.o: $(CRYPTODIR)/bitmap.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

crc32.o: $(CRYPTODIR)/crc32.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

crc64.o: $(CRYPTODIR)/crc64.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

sha256.o: $(CRYPTODIR)/sha256.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_bitops_crc_sha.o: test_bitops_crc_sha.c
	$(CC) $(TEST_CFLAGS) -DTEST_CRC64 -c -o $@ $<

clean:
	rm -f $(OBJS) $(CRYPTO_OBJS) test_libc test_bitops_crc_sha
