# host_libc Makefile
# Compiles kernel libc sources with host gcc for unit testing on Linux x86_64

CC = gcc

# Flags for kernel source files (need kernel include paths)
KERNEL_CFLAGS = -DTEST_MODE_HOST \
                -isystem ../../src/include \
                -isystem ../../src \
                -Wall -Werror -Wmisleading-indentation \
                -Wno-error=implicit-function-declaration \
                -Wno-format-truncation \
                -Wno-unused-but-set-variable \
                -Wno-unused-variable \
                -Wno-unused-function \
                -g -O0

# Flags for test files (use system headers, NOT kernel headers)
TEST_CFLAGS = -DTEST_MODE_HOST \
              -Wall -Werror -Wmisleading-indentation \
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

# Data structure tests (klist, llist, interval_tree, prio_tree, cpumask)
KSRCDIR = ../../src/kernel
DS_OBJS = klist.o llist.o interval_tree.o prio_tree.o cpu_bitmask.o test_datastructures.o

# Search algorithm tests (qsort_ext, bsearch_ext, lfind, lsearch)
SEARCH_OBJS = search.o test_search.o

# Extended stdlib tests (strtoll, strtoull, atoll, ultoa, realloc, calloc)
STDLIB_EXT_OBJS = stdlib_ext.o test_stdlib_ext.o

# Hamming weight tests (popcount 8/16/32/64)
HWEIGHT_OBJS = hweight.o test_hweight.o

# Range management tests (sort, add, remove, contains, overlaps)
RANGE_OBJS = range.o test_range.o

# Sort extension tests (sort, swap_bytes, cmp_int/long/str)
SORT_EXT_OBJS = sort_ext.o test_sort_ext.o

# CRC-16 tests
CRC16_OBJS = crc16.o test_crc16.o

# find_bit tests (test_bit, set_bit, find_first_bit, etc.)
FIND_BIT_OBJS = find_bit.o test_find_bit.o

# 64-bit division tests (__udivdi3, __umoddi3, etc.)
DIV64_OBJS = div64.o test_div64.o

# IDR allocator tests (idr_init, idr_alloc, idr_remove, idr_find)
IDR_OBJS = idr.o test_idr.o

# Notifier chain tests
NOTIFIER_OBJS = notifier.o test_notifier.o

# Notifier chain extension tests (priority-ordered)
NOTIFIER_EXT_OBJS = notifier_ext.o test_notifier_ext.o

# W^X page protection enforcement tests
WX_ENFORCE_OBJS = wx_enforce.o test_wx_enforce.o

# STACKLEAK kernel stack eraser tests
STACKLEAK_OBJS = stackleak.o test_stackleak.o

# Rate limiting tests
RATELIMIT_OBJS = ratelimit.o test_ratelimit.o

# Time function tests (gmtime, mktime, asctime, strftime)
TFLAGS_time = -Wno-nonnull-compare -fno-stack-protector
TIME_OBJS = time.o test_time.o

# UUID tests
UUID_OBJS = uuid.o test_uuid.o

# Hash table tests
HASHTABLE_OBJS = hashtable.o test_hashtable.o

# Refcount tests
REFCOUNT_OBJS = refcount.o test_refcount.o

# Bitfield macro tests (self-contained — no kernel sources needed)
BITFIELD_OBJS = test_bitfield.o

# Errno string tests
ERRNO_EXT_OBJS = errno_ext.o test_errno_ext.o

# Errno integration test (kernel errno.h vs system errno.h)
ERRNO_INTEGRATION_OBJS = errno_integration_data.o test_errno_integration.o

# UID16 conversion tests (self-contained — inline functions only)
UID16_OBJS = test_uid16.o

# Memory pool tests
MEMPOOL_OBJS = mempool.o test_mempool.o

# PRNG core tests (self-contained — xorshift64 reimplemented)
RNG_CORE_OBJS = test_rng_core.o

# ── Kernel command-line parsing tests ─────────────────────
CMDLINE_OBJS = cmdline.o test_cmdline.o

# ── LZSS compression tests ────────────────────────────────
COMPRESS_OBJS = compress.o test_compress.o

# ── Log buffer tests ──────────────────────────────────────
LOGBUF_OBJS = logbuf.o test_logbuf.o

# ── Module alias tests ────────────────────────────────────
MODULE_ALIAS_OBJS = module_alias.o test_module_alias.o

# ── Sched attribute validation tests ──────────────────────
SCHED_ATTR_OBJS = sched_attr.o test_sched_attr.o

# ── Signal validation tests ───────────────────────────────
SIGNAL_VALIDATE_OBJS = signal_validate.o test_signal_validate.o

# ── Kernel pointer restrict tests ─────────────────────────
KPTR_RESTRICT_OBJS = kptr_restrict.o test_kptr_restrict.o

.PHONY: all clean

all: test_libc test_bitops_crc_sha test_crypto_ext test_lib_structs test_cipher test_datastructures \
     test_search test_stdlib_ext test_hweight test_range test_sort_ext test_crc16 \
     test_find_bit test_div64 test_idr test_notifier test_ratelimit test_time \
     test_uuid test_hashtable test_refcount \
     test_bitfield test_errno_ext test_errno_integration \
     test_uid16 test_mempool test_rng_core \
     test_cmdline test_compress test_logbuf test_module_alias test_sched_attr \
     test_signal_validate test_kptr_restrict \
     test_notifier_ext test_wx_enforce test_stackleak \
     test_negative_path
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
	@echo ""
	@echo "Running host-side data structure tests..."
	./test_datastructures
	@echo ""
	@echo "Running search algorithm tests..."
	./test_search
	@echo ""
	@echo "Running extended stdlib tests..."
	./test_stdlib_ext
	@echo ""
	@echo "Running hamming weight tests..."
	./test_hweight
	@echo ""
	@echo "Running range management tests..."
	./test_range
	@echo ""
	@echo "Running sort extension tests..."
	./test_sort_ext
	@echo ""
	@echo "Running CRC-16 tests..."
	./test_crc16
	@echo ""
	@echo "Running find_bit tests..."
	./test_find_bit
	@echo ""
	@echo "Running 64-bit division tests..."
	./test_div64
	@echo ""
	@echo "Running IDR allocator tests..."
	./test_idr
	@echo ""
	@echo "Running notifier chain tests..."
	./test_notifier
	@echo ""
	@echo "Running rate limiting tests..."
	./test_ratelimit
	@echo ""
	@echo "Running time function tests..."
	./test_time
	@echo ""
	@echo "Running UUID tests..."
	./test_uuid
	@echo ""
	@echo "Running hash table tests..."
	./test_hashtable
	@echo ""
	@echo "Running refcount tests..."
	./test_refcount
	@echo ""
	@echo "Running bitfield macro tests..."
	./test_bitfield
	@echo ""
	@echo "Running errno string tests..."
	./test_errno_ext
	@echo ""
	@echo "Running errno integration tests..."
	./test_errno_integration
	@echo ""
	@echo "Running UID16 conversion tests..."
	./test_uid16
	@echo ""
	@echo "Running memory pool tests..."
	./test_mempool
	@echo ""
	@echo "Running PRNG core tests..."
	./test_rng_core
	@echo ""
	@echo "Running kernel cmdline parsing tests..."
	./test_cmdline
	@echo ""
	@echo "Running LZSS compression tests..."
	./test_compress
	@echo ""
	@echo "Running log buffer tests..."
	./test_logbuf
	@echo ""
	@echo "Running module alias tests..."
	./test_module_alias
	@echo ""
	@echo "Running sched_attr validation tests..."
	./test_sched_attr
	@echo ""
	@echo "Running signal validation tests..."
	./test_signal_validate
	@echo ""
	@echo "Running kernel pointer restrict tests..."
	./test_kptr_restrict
	@echo ""
	@echo "Running notifier chain extension tests..."
	./test_notifier_ext
	@echo ""
	@echo "Running W^X enforcement tests..."
	./test_wx_enforce
	@echo ""
	@echo "Running STACKLEAK stack eraser tests..."
	./test_stackleak
	@echo ""
	@echo "Running negative-path / edge-case tests..."
	./test_negative_path
else
	@echo ""
	@echo "Running host_libc unit tests (JUnit output to $(JUNIT_DIR))..."
	python3 ../../tests/junit_reporter.py host_libc ./test_libc
	python3 ../../tests/junit_reporter.py host-bitops-crc-sha ./test_bitops_crc_sha
	python3 ../../tests/junit_reporter.py host-crypto-ext ./test_crypto_ext
	python3 ../../tests/junit_reporter.py host-lib-structs ./test_lib_structs
	python3 ../../tests/junit_reporter.py host-cipher ./test_cipher
	python3 ../../tests/junit_reporter.py host-datastructures ./test_datastructures
	python3 ../../tests/junit_reporter.py host-search ./test_search
	python3 ../../tests/junit_reporter.py host-stdlib-ext ./test_stdlib_ext
	python3 ../../tests/junit_reporter.py host-hweight ./test_hweight
	python3 ../../tests/junit_reporter.py host-range ./test_range
	python3 ../../tests/junit_reporter.py host-sort-ext ./test_sort_ext
	python3 ../../tests/junit_reporter.py host-crc16 ./test_crc16
	python3 ../../tests/junit_reporter.py host-find-bit ./test_find_bit
	python3 ../../tests/junit_reporter.py host-div64 ./test_div64
	python3 ../../tests/junit_reporter.py host-idr ./test_idr
	python3 ../../tests/junit_reporter.py host-notifier ./test_notifier
	python3 ../../tests/junit_reporter.py host-ratelimit ./test_ratelimit
	python3 ../../tests/junit_reporter.py host-time ./test_time
	python3 ../../tests/junit_reporter.py host-uuid ./test_uuid
	python3 ../../tests/junit_reporter.py host-hashtable ./test_hashtable
	python3 ../../tests/junit_reporter.py host-refcount ./test_refcount
	python3 ../../tests/junit_reporter.py host-bitfield ./test_bitfield
	python3 ../../tests/junit_reporter.py host-errno-ext ./test_errno_ext
	python3 ../../tests/junit_reporter.py host-errno-integration ./test_errno_integration
	python3 ../../tests/junit_reporter.py host-uid16 ./test_uid16
	python3 ../../tests/junit_reporter.py host-mempool ./test_mempool
	python3 ../../tests/junit_reporter.py host-rng-core ./test_rng_core
	python3 ../../tests/junit_reporter.py host-cmdline ./test_cmdline
	python3 ../../tests/junit_reporter.py host-compress ./test_compress
	python3 ../../tests/junit_reporter.py host-logbuf ./test_logbuf
	python3 ../../tests/junit_reporter.py host-module-alias ./test_module_alias
	python3 ../../tests/junit_reporter.py host-sched-attr ./test_sched_attr
	python3 ../../tests/junit_reporter.py host-signal-validate ./test_signal_validate
	python3 ../../tests/junit_reporter.py host-kptr-restrict ./test_kptr_restrict
	python3 ../../tests/junit_reporter.py host-notifier-ext ./test_notifier_ext
	python3 ../../tests/junit_reporter.py host-wx-enforce ./test_wx_enforce
	python3 ../../tests/junit_reporter.py host-stackleak ./test_stackleak
	python3 ../../tests/junit_reporter.py host-negative-path ./test_negative_path
	@mv -f host_libc-results.xml host-bitops-crc-sha-results.xml \
	      host-crypto-ext-results.xml host-lib-structs-results.xml \
	      host-cipher-results.xml host-datastructures-results.xml \
	      host-search-results.xml host-stdlib-ext-results.xml \
	      host-hweight-results.xml host-range-results.xml \
	      host-sort-ext-results.xml host-crc16-results.xml \
	      host-find-bit-results.xml host-div64-results.xml \
	      host-idr-results.xml host-notifier-results.xml \
	      host-ratelimit-results.xml host-time-results.xml \
	      host-uuid-results.xml host-hashtable-results.xml \
	      host-refcount-results.xml \
	      host-bitfield-results.xml host-errno-ext-results.xml \
	      host-errno-integration-results.xml \
	      host-uid16-results.xml host-mempool-results.xml \
	      host-rng-core-results.xml \
	      host-cmdline-results.xml host-compress-results.xml \
	      host-logbuf-results.xml host-module-alias-results.xml \
	      host-sched-attr-results.xml host-signal-validate-results.xml \
	      host-kptr-restrict-results.xml \
	      host-notifier-ext-results.xml host-wx-enforce-results.xml \
	      host-stackleak-results.xml \
	      "$(JUNIT_DIR)/" 2>/dev/null || true
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

# ── Host-side data structure tests ─────────────────────────────

test_datastructures: $(DS_OBJS) printf.o stubs.o
	$(CC) -o $@ $^

klist.o: $(KSRCDIR)/klist.c
	$(CC) -I. $(subst -isystem,-I,$(KERNEL_CFLAGS)) -include spinlock.h -c -o $@ $<

llist.o: $(KSRCDIR)/llist.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

interval_tree.o: $(KSRCDIR)/interval_tree.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

prio_tree.o: $(KSRCDIR)/prio_tree.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

cpu_bitmask.o: $(KSRCDIR)/cpu_bitmask.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_datastructures.o: test_datastructures.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Search algorithm tests ────────────────────────────────────

test_search: $(SEARCH_OBJS) stubs.o
	$(CC) -o $@ $^

search.o: $(SRCDIR)/search.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_search.o: test_search.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Extended stdlib tests ─────────────────────────────────────

test_stdlib_ext: $(STDLIB_EXT_OBJS) stubs.o
	$(CC) -o $@ $^

stdlib_ext.o: $(SRCDIR)/stdlib_ext.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_stdlib_ext.o: test_stdlib_ext.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Hamming weight tests ──────────────────────────────────────

test_hweight: $(HWEIGHT_OBJS) stubs.o
	$(CC) -o $@ $^

hweight.o: $(KSRCDIR)/hweight.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_hweight.o: test_hweight.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Range management tests ────────────────────────────────────

test_range: $(RANGE_OBJS) stubs.o
	$(CC) -o $@ $^

range.o: $(KSRCDIR)/range.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_range.o: test_range.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Sort extension tests ──────────────────────────────────────

test_sort_ext: $(SORT_EXT_OBJS) stubs.o
	$(CC) -o $@ $^

sort_ext.o: $(KSRCDIR)/sort_ext.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_sort_ext.o: test_sort_ext.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── CRC-16 tests ──────────────────────────────────────────────

test_crc16: $(CRC16_OBJS) stubs.o
	$(CC) -o $@ $^

crc16.o: $(CRYPTODIR)/crc16.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_crc16.o: test_crc16.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── find_bit tests ─────────────────────────────────────────

test_find_bit: $(FIND_BIT_OBJS) stubs.o
	$(CC) -o $@ $^

find_bit.o: $(SRCDIR)/find_bit.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_find_bit.o: test_find_bit.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── 64-bit division tests ─────────────────────────────────

test_div64: $(DIV64_OBJS) stubs.o
	$(CC) -o $@ $^

div64.o: $(KSRCDIR)/div64.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_div64.o: test_div64.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── IDR allocator tests ───────────────────────────────────

test_idr: $(IDR_OBJS) stubs.o
	$(CC) -o $@ $^

idr.o: $(KSRCDIR)/idr.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_idr.o: test_idr.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Notifier chain tests ──────────────────────────────────

test_notifier: $(NOTIFIER_OBJS) stubs.o
	$(CC) -o $@ $^

notifier.o: $(KSRCDIR)/notifier.c
	$(CC) $(KERNEL_CFLAGS) -DEINVAL=22 -c -o $@ $<

test_notifier.o: test_notifier.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Notifier chain extension tests (priority-ordered) ────────────

test_notifier_ext: $(NOTIFIER_EXT_OBJS) stubs.o
	$(CC) -o $@ $^

notifier_ext.o: $(KSRCDIR)/notifier_ext.c
	$(CC) -I. $(subst -isystem,-I,$(KERNEL_CFLAGS)) -include spinlock.h -DEINVAL=22 -c -o $@ $<

test_notifier_ext.o: test_notifier_ext.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── W^X enforcement tests ────────────────────────────────────────

test_wx_enforce: $(WX_ENFORCE_OBJS) stubs.o
	$(CC) -o $@ $^

wx_enforce.o: $(KSRCDIR)/wx_enforce.c
	$(CC) $(KERNEL_CFLAGS) -DEPERM=1 -DEINVAL=22 -c -o $@ $<

test_wx_enforce.o: test_wx_enforce.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── STACKLEAK stack eraser tests ─────────────────────────────────

test_stackleak: $(STACKLEAK_OBJS) stubs.o
	$(CC) -o $@ $^

stackleak.o: $(KSRCDIR)/stackleak.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_stackleak.o: test_stackleak.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Negative-path / edge-case tests ────────────────────────

test_negative_path: test_negative_path.o
	$(CC) -o $@ $^

test_negative_path.o: test_negative_path.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Rate limiting tests ────────────────────────────────────

test_ratelimit: $(RATELIMIT_OBJS)
	$(CC) -o $@ $^

ratelimit.o: $(KSRCDIR)/ratelimit.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_ratelimit.o: test_ratelimit.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Time function tests ────────────────────────────────────

test_time: $(TIME_OBJS) stubs.o
	$(CC) -o $@ $^

time.o: $(SRCDIR)/time.c
	$(CC) $(KERNEL_CFLAGS) $(TFLAGS_time) -c -o $@ $<

test_time.o: test_time.c
	$(CC) $(TEST_CFLAGS) -Wno-format-zero-length -c -o $@ $<

# ── UUID tests ──────────────────────────────────────────────

test_uuid: $(UUID_OBJS) stubs.o
	$(CC) -o $@ $^

test_uuid.o: test_uuid.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Hash table tests ────────────────────────────────────────

test_hashtable: $(HASHTABLE_OBJS) stubs.o
	$(CC) -o $@ $^

hashtable.o: $(KSRCDIR)/hashtable.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_hashtable.o: test_hashtable.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Refcount tests ──────────────────────────────────────────

test_refcount: $(REFCOUNT_OBJS) stubs.o
	$(CC) -o $@ $^

refcount.o: $(KSRCDIR)/refcount_ext.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_refcount.o: test_refcount.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Bitfield macro tests ────────────────────────────────────

test_bitfield: $(BITFIELD_OBJS)
	$(CC) -o $@ $^

test_bitfield.o: test_bitfield.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Errno string tests ───────────────────────────────────

test_errno_ext: $(ERRNO_EXT_OBJS) stubs.o
	$(CC) -o $@ $^

errno_ext.o: $(SRCDIR)/errno_ext.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_errno_ext.o: test_errno_ext.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Errno integration test (kernel vs system) ────────────────

test_errno_integration: $(ERRNO_INTEGRATION_OBJS) errno_ext.o
	$(CC) -o $@ $^

errno_integration_data.o: errno_integration_data.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_errno_integration.o: test_errno_integration.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── UID16 conversion tests ────────────────────────────────

test_uid16: $(UID16_OBJS)
	$(CC) -o $@ $^

test_uid16.o: test_uid16.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Memory pool tests ─────────────────────────────────────

test_mempool: $(MEMPOOL_OBJS) stubs.o
	$(CC) -o $@ $^

mempool.o: $(SRCDIR)/mempool.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_mempool.o: test_mempool.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── PRNG core tests ───────────────────────────────────────

test_rng_core: $(RNG_CORE_OBJS)
	$(CC) -o $@ $^

test_rng_core.o: test_rng_core.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Kernel command-line parsing tests ─────────────────────

test_cmdline: $(CMDLINE_OBJS) stubs.o
	$(CC) -o $@ $^

cmdline.o: $(KSRCDIR)/cmdline.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_cmdline.o: test_cmdline.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── LZSS compression tests ────────────────────────────────

test_compress: $(COMPRESS_OBJS) stubs.o
	$(CC) -o $@ $^

compress.o: $(KSRCDIR)/compress.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_compress.o: test_compress.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Log buffer tests ──────────────────────────────────────

test_logbuf: $(LOGBUF_OBJS) stubs.o
	$(CC) -o $@ $^

logbuf.o: $(KSRCDIR)/logbuf.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_logbuf.o: test_logbuf.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Module alias tests ────────────────────────────────────

test_module_alias: $(MODULE_ALIAS_OBJS) stubs.o
	$(CC) -o $@ $^

module_alias.o: $(KSRCDIR)/module_alias.c
	$(CC) -I. $(subst -isystem,-I,$(KERNEL_CFLAGS)) -include spinlock.h -c -o $@ $<

test_module_alias.o: test_module_alias.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Sched attribute validation tests ──────────────────────

test_sched_attr: $(SCHED_ATTR_OBJS) stubs.o
	$(CC) -o $@ $^

sched_attr.o: $(KSRCDIR)/sched_attr.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_sched_attr.o: test_sched_attr.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Signal validation tests ───────────────────────────────

test_signal_validate: $(SIGNAL_VALIDATE_OBJS) stubs.o
	$(CC) -o $@ $^

signal_validate.o: $(KSRCDIR)/signal_validate.c
	$(CC) $(KERNEL_CFLAGS) -DEINVAL=22 -DEPERM=1 -c -o $@ $<

test_signal_validate.o: test_signal_validate.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ── Kernel pointer restrict tests ─────────────────────────
# NOTE: Does NOT link stubs.o — both define kptr_restrict_check()
# (stubs.c has weak attr, but test file provides its own stubs).

test_kptr_restrict: $(KPTR_RESTRICT_OBJS)
	$(CC) -o $@ $^

kptr_restrict.o: $(KSRCDIR)/kptr_restrict.c
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

test_kptr_restrict.o: test_kptr_restrict.c
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(CRYPTO_OBJS) $(CRYPTO_EXT_OBJS) $(LIB_STRUCTS_OBJS) $(CIPHER_OBJS) $(DS_OBJS) \
	      $(SEARCH_OBJS) $(STDLIB_EXT_OBJS) $(HWEIGHT_OBJS) $(RANGE_OBJS) $(SORT_EXT_OBJS) $(CRC16_OBJS) \
	      $(FIND_BIT_OBJS) $(DIV64_OBJS) $(IDR_OBJS) $(NOTIFIER_OBJS) $(RATELIMIT_OBJS) $(TIME_OBJS) \
	      $(UUID_OBJS) $(HASHTABLE_OBJS) $(REFCOUNT_OBJS) \
	      $(BITFIELD_OBJS) $(ERRNO_EXT_OBJS) $(ERRNO_INTEGRATION_OBJS) $(UID16_OBJS) $(MEMPOOL_OBJS) $(RNG_CORE_OBJS) \
	      $(CMDLINE_OBJS) $(COMPRESS_OBJS) $(LOGBUF_OBJS) $(MODULE_ALIAS_OBJS) \
	      $(SCHED_ATTR_OBJS) $(SIGNAL_VALIDATE_OBJS) $(KPTR_RESTRICT_OBJS) \
	      $(NOTIFIER_EXT_OBJS) $(WX_ENFORCE_OBJS) $(STACKLEAK_OBJS) \
	      test_negative_path.o \
	      test_libc test_bitops_crc_sha test_crypto_ext test_lib_structs test_cipher test_datastructures \
	      test_search test_stdlib_ext test_hweight test_range test_sort_ext test_crc16 \
	      test_find_bit test_div64 test_idr test_notifier test_ratelimit test_time \
	      test_uuid test_hashtable test_refcount \
	      test_bitfield test_errno_ext test_errno_integration \
      test_uid16 test_mempool test_rng_core \
	      test_cmdline test_compress test_logbuf test_module_alias test_sched_attr \
	      test_signal_validate test_kptr_restrict \
	      test_notifier_ext test_wx_enforce test_stackleak test_negative_path
