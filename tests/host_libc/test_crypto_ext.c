/*
 * test_crypto_ext.c — Host-side tests for extended crypto functions.
 *
 * Tests SHA-512, MD5, HMAC-MD5, HMAC-SHA256, Adler-32, Base64.
 * AES tests are excluded from host-side because AES key schedule
 * and block ops use kernel-specific struct layout and are tested
 * in-kernel via dm-crypt / LUKS e2e tests.
 *
 * Compile with -DTEST_MODE_HOST. Manual prototypes only — no kernel headers.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===================================================================
 *  Kernel function declarations (mirroring kernel headers)
 * =================================================================== */
#define SHA512_DIGEST_SIZE 64
struct sha512_ctx { uint64_t count; uint64_t state[8]; uint8_t buffer[128]; };
extern void sha512_init(struct sha512_ctx *ctx);
extern void sha512_update(struct sha512_ctx *ctx, const void *data, size_t len);
extern void sha512_final(uint8_t digest[64], struct sha512_ctx *ctx);
extern void sha512_hash(uint8_t digest[64], const void *data, size_t len);
extern void sha512_init_crypto(void);

#define MD5_DIGEST_SIZE 16
struct md5_ctx { uint64_t count; uint32_t state[4]; uint8_t buffer[64]; };
extern void md5_init(struct md5_ctx *ctx);
extern void md5_update(struct md5_ctx *ctx, const void *data, size_t len);
extern void md5_final(uint8_t digest[16], struct md5_ctx *ctx);
extern void md5_hash(uint8_t digest[16], const void *data, size_t len);
extern void md5_init_crypto(void);

#define HMAC_MD5_DIGEST_SIZE 16
#define HMAC_SHA256_DIGEST_SIZE 32
extern void hmac_md5(const uint8_t *key, size_t klen, const uint8_t *data, size_t dlen, uint8_t mac[16]);
extern void hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *data, size_t dlen, uint8_t mac[32]);
extern void hmac_init(void);

extern uint32_t adler32(uint32_t adler, const void *buf, size_t len);
extern void adler32_init(void);

extern size_t base64_encode(char *out, const uint8_t *in, size_t in_len);
extern size_t base64_decode(uint8_t *out, const char *in, size_t in_len);
extern void base64_init(void);

/* Stubs for printf.o */
void vga_putchar(char c)    { (void)c; }
void serial_putchar(char c) { (void)c; }

/* ===================================================================
 *  Test harness
 * =================================================================== */
static int tests_passed = 0;
static int tests_failed = 0;
#define TEST(name, cond) do { if (!(cond)) { printf("  FAIL: %s\n", name); tests_failed++; } \
    else { printf("  PASS: %s\n", name); tests_passed++; } } while(0)

/* ===================================================================
 *  SHA-512 tests
 * =================================================================== */
static void test_sha512(void) {
    uint8_t d1[SHA512_DIGEST_SIZE], d2[SHA512_DIGEST_SIZE];
    printf("\n[SHA-512]\n");

    sha512_hash(d1, "", 0);
    sha512_hash(d2, "abc", 3);
    TEST("SHA512('') != SHA512('abc')", memcmp(d1, d2, SHA512_DIGEST_SIZE) != 0);

    sha512_hash(d2, "", 0);
    TEST("SHA512('') deterministic", memcmp(d1, d2, SHA512_DIGEST_SIZE) == 0);

    /* Stream vs all-in-one */
    struct sha512_ctx ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, "Hello ", 6);
    sha512_update(&ctx, "World", 5);
    sha512_final(d1, &ctx);
    sha512_hash(d2, "Hello World", 11);
    TEST("SHA512 stream matches all-in-one", memcmp(d1, d2, SHA512_DIGEST_SIZE) == 0);
}

/* ===================================================================
 *  MD5 tests
 * =================================================================== */
static void test_md5(void) {
    uint8_t d1[MD5_DIGEST_SIZE], d2[MD5_DIGEST_SIZE];
    printf("\n[MD5]\n");

    md5_hash(d1, "", 0);
    TEST("MD5('') non-zero", d1[0] != 0 || d1[15] != 0);

    md5_hash(d1, "abc", 3);
    md5_hash(d2, "xyz", 3);
    TEST("MD5 different inputs differ", memcmp(d1, d2, MD5_DIGEST_SIZE) != 0);

    /* Stream consistency */
    struct md5_ctx ctx;
    md5_init(&ctx);
    md5_update(&ctx, "abc", 3);
    md5_final(d1, &ctx);
    md5_hash(d2, "abc", 3);
    TEST("MD5 stream matches all-in-one", memcmp(d1, d2, MD5_DIGEST_SIZE) == 0);
}

/* ===================================================================
 *  HMAC tests
 * =================================================================== */
static void test_hmac(void) {
    uint8_t mac1[32], mac2[32];
    printf("\n[HMAC]\n");

    uint8_t key[16];
    memset(key, 0x0b, 16);
    hmac_md5(key, 16, (const uint8_t*)"Hi There", 8, mac1);
    TEST("HMAC-MD5 non-zero", mac1[0] != 0 || mac1[15] != 0);

    /* Different key => different MAC */
    uint8_t key2[16];
    memset(key2, 0x01, 16);
    hmac_md5(key2, 16, (const uint8_t*)"Hi There", 8, mac2);
    TEST("HMAC-MD5 different key => different MAC", memcmp(mac1, mac2, 16) != 0);

    /* HMAC-SHA256 produces deterministic output */
    uint8_t k3[20];
    memset(k3, 0x0b, 20);
    hmac_sha256(k3, 20, (const uint8_t*)"Hi There", 8, mac1);
    hmac_sha256(k3, 20, (const uint8_t*)"Hi There", 8, mac2);
    TEST("HMAC-SHA256 deterministic", memcmp(mac1, mac2, 32) == 0);

    /* HMAC with long key */
    uint8_t k4[80];
    memset(k4, 0xaa, 80);
    hmac_md5(k4, 80, (const uint8_t*)"Test Using Large Key", 20, mac2);
    TEST("HMAC-MD5 long key works", mac2[0] != 0 || mac2[15] != 0);
}

/* ===================================================================
 *  Adler-32 tests
 * =================================================================== */
static void test_adler32(void) {
    printf("\n[Adler-32]\n");
    TEST("Adler32('') = 1", adler32(1, "", 0) == 1);
    uint32_t a = adler32(1, "Hello ", 6);
    uint32_t b = adler32(a, "World", 5);
    uint32_t d = adler32(1, "Hello World", 11);
    TEST("Adler32 chain matches direct", b == d);
    TEST("Adler32 chaining modifies state", a != 1);
}

/* ===================================================================
 *  Base64 tests (RFC 4648 roundtrip)
 * =================================================================== */
static void test_base64(void) {
    char enc[64];
    uint8_t dec[64];
    size_t elen, dlen;
    printf("\n[Base64]\n");

    const char *tests[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar",
                           "hello base64 world!", "binary\0data with nulls"};
    int i;
    for (i = 0; i < 9; i++) {
        size_t len = strlen(tests[i]);
        /* For the binary test, include null terminator */
        if (i == 8) len = 22;
        elen = base64_encode(enc, (const uint8_t*)tests[i], len);
        enc[elen] = '\0';
        dlen = base64_decode(dec, enc, elen);
        char name[64];
        snprintf(name, sizeof(name), "Base64 roundtrip '%s'", tests[i]);
        TEST(name, dlen == len && memcmp(dec, tests[i], len) == 0);
    }
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void) {
    printf("=== Extended Crypto Unit Tests ===\n");
    sha512_init_crypto();
    md5_init_crypto();
    hmac_init();
    adler32_init();
    base64_init();
    test_sha512();
    test_md5();
    test_hmac();
    test_adler32();
    test_base64();
    printf("\n=== Results: %d passed, %d failed (out of %d) ===\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
