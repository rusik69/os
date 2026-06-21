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
                -Wno-unused-but-set-variable \
                -Wno-unused-variable \
                -Wno-unused-function \
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

# Extended crypto tests (SHA-512, MD5, HMAC, Adler-32, Base64)
CRYPTO_EXT_OBJS = sha512.o md5.o hmac.o adler32.o base64.o test_crypto_ext.o

# Library structure tests (hexdump, radix tree)
KSRCDIR = ../../src/kernel
LIB_STRUCTS_OBJS = hexdump.o radix_tree.o test_lib_structs.o

# Cipher tests (AES, ChaCha20-Poly1305)
CIPHERDIR = ../../src/lib
CIPHER_OBJS = aes.o aes_xts.o chacha20.o poly1305.o chacha20poly1305.o test_cipher.o

.PHONY: all clean

all: test_libc test_bitops_crc_sha test_crypto_ext test_lib_structs test_cipher
ifeq ($(JUNIT_DIR),)
	@echo ""
	@echo "Running host_libc unit tests..."
	./test_libc
	@echo ""
	@echo "Running host-side crypto/bitmap tests..."
	./test_bitops_crc_sha
	@echo ""
	@echo "Running host-side crypto extension tests..."
	./test_crypto_ext
	@echo ""
	@echo "Running host-side library structure tests..."
	./test_lib_structs
	@echo ""
	@echo "Running host-side cipher tests..."
	./test_cipher
else
	@echo ""
	@echo "Running host_libc unit tests (JUnit output to $(JUNIT_DIR))..."
	python3 ../../tests/junit_reporter.py host_libc ./test_libc
	python3 ../../tests/junit_reporter.py host-bitops-crc-sha ./test_bitops_crc_sha
	python3 ../../tests/junit_reporter.py host-crypto-ext ./test_crypto_ext
	python3 ../../tests/junit_reporter.py host-lib-structs ./test_lib_structs
	python3 ../../tests/junit_reporter.py host-cipher ./test_cipher
	@mv -f host_libc-results.xml host-bitops-crc-sha-results.xml \
	      host-crypto-ext-results.xml host-lib-structs-results.xml \
	      host-cipher-results.xml "$(JUNIT_DIR)/" 2>/dev/null || true
	@echo "JUnit XML: $(JUNIT_DIR)/"
endif

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

# ── Host-side crypto extension tests ───────────────────────────

test_crypto_ext: $(CRYPTO_EXT_OBJS) sha256.o printf.o stubs.o
	$(CC) -o $@ $^

sha512.o: $(CRYPTODIR)/sha512.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

md5.o: $(CRYPTODIR)/md5.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

hmac.o: $(CRYPTODIR)/hmac.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

adler32.o: $(CRYPTODIR)/adler32.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

base64.o: $(CRYPTODIR)/base64.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_crypto_ext.o: test_crypto_ext.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Host-side library structure tests ──────────────────────────

test_lib_structs: $(LIB_STRUCTS_OBJS) printf.o stubs.o
	$(CC) -o $@ $^

uuid.o: $(CRYPTODIR)/uuid.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

hexdump.o: $(CRYPTODIR)/hexdump.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

radix_tree.o: $(CRYPTODIR)/radix_tree.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_lib_structs.o: test_lib_structs.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Host-side cipher tests (AES, ChaCha20-Poly1305) ──────────────

test_cipher: $(CIPHER_OBJS) printf.o stubs.o
	$(CC) -o $@ $^

aes.o: $(CIPHERDIR)/aes.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

aes_xts.o: $(CIPHERDIR)/aes_xts.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

chacha20.o: $(CIPHERDIR)/chacha20.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

poly1305.o: $(CIPHERDIR)/poly1305.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

chacha20poly1305.o: $(CIPHERDIR)/chacha20poly1305.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_cipher.o: test_cipher.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(CRYPTO_OBJS) $(CRYPTO_EXT_OBJS) $(LIB_STRUCTS_OBJS) $(CIPHER_OBJS) \
	      test_libc test_bitops_crc_sha test_crypto_ext test_lib_structs test_cipher
