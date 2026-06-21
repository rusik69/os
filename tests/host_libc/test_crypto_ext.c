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

    /* SHA-512 NIST known-answer: SHA512('') = cf83e1357... */
    sha512_hash(d1, "", 0);
    uint8_t expected_empty[64] = {
        0xcf,0x83,0xe1,0x35,0x7e,0xef,0xb8,0xbd,
        0xf1,0x54,0x28,0x50,0xd6,0x6d,0x80,0x07,
        0xd6,0x20,0xe4,0x05,0x0b,0x57,0x15,0xdc,
        0x83,0xf4,0xa9,0x21,0xd3,0x6c,0xe9,0xce,
        0x47,0xd0,0xd1,0x3c,0x5d,0x85,0xf2,0xb0,
        0xff,0x83,0x18,0xd2,0x87,0x7e,0xec,0x2f,
        0x63,0xb9,0x31,0xbd,0x47,0x41,0x7a,0x81,
        0xa5,0x38,0x32,0x7a,0xf9,0x27,0xda,0x3e
    };
    TEST("SHA512('') NIST vector", memcmp(d1, expected_empty, 64) == 0);

    /* SHA-512 NIST known-answer: SHA512('abc') = ddaf35a1936... */
    sha512_hash(d1, "abc", 3);
    uint8_t expected_abc[64] = {
        0xdd,0xaf,0x35,0xa1,0x93,0x61,0x7a,0xba,
        0xcc,0x41,0x73,0x49,0xae,0x20,0x41,0x31,
        0x12,0xe6,0xfa,0x4e,0x89,0xa9,0x7e,0xa2,
        0x0a,0x9e,0xee,0xe6,0x4b,0x55,0xd3,0x9a,
        0x21,0x92,0x99,0x2a,0x27,0x4f,0xc1,0xa8,
        0x36,0xba,0x3c,0x23,0xa3,0xfe,0xeb,0xbd,
        0x45,0x4d,0x44,0x23,0x64,0x3c,0xe8,0x0e,
        0x2a,0x9a,0xc9,0x4f,0xa5,0x4c,0xa4,0x9f
    };
    TEST("SHA512('abc') NIST vector", memcmp(d1, expected_abc, 64) == 0);

    /* SHA-512 multi-block: 64 bytes (exactly one 1024-bit block) */
    char block64[64];
    memset(block64, 'a', 64);
    sha512_hash(d1, block64, 64);
    /* Just check it's deterministic */
    sha512_hash(d2, block64, 64);
    TEST("SHA512 64-byte block deterministic", memcmp(d1, d2, 64) == 0);
    /* And differs from short inputs */
    TEST("SHA512 64-byte block != SHA512('')", memcmp(d1, expected_empty, 64) != 0);
    TEST("SHA512 64-byte block != SHA512('abc')", memcmp(d1, expected_abc, 64) != 0);
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

    /* MD5 known-answer: MD5('') = d41d8cd98f00b204e9800998ecf8427e */
    md5_hash(d1, "", 0);
    uint8_t md5_empty[16] = {
        0xd4,0x1d,0x8c,0xd9,0x8f,0x00,0xb2,0x04,
        0xe9,0x80,0x09,0x98,0xec,0xf8,0x42,0x7e
    };
    TEST("MD5('') NIST vector", memcmp(d1, md5_empty, 16) == 0);

    /* MD5 known-answer: MD5('abc') = 900150983cd24fb0d6963f7d28e17f72 */
    md5_hash(d1, "abc", 3);
    uint8_t md5_abc[16] = {
        0x90,0x01,0x50,0x98,0x3c,0xd2,0x4f,0xb0,
        0xd6,0x96,0x3f,0x7d,0x28,0xe1,0x7f,0x72
    };
    TEST("MD5('abc') NIST vector", memcmp(d1, md5_abc, 16) == 0);

    /* MD5 of longer string */
    const char *fox = "The quick brown fox jumps over the lazy dog";
    md5_hash(d1, fox, strlen(fox));
    /* MD5 of this phrase = 9e107d9d372bb6826bd81d3542a419d6 */
    uint8_t md5_fox[16] = {
        0x9e,0x10,0x7d,0x9d,0x37,0x2b,0xb6,0x82,
        0x6b,0xd8,0x1d,0x35,0x42,0xa4,0x19,0xd6
    };
    TEST("MD5('fox') known vector", memcmp(d1, md5_fox, 16) == 0);
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

    /* HMAC-MD5 RFC 2104 Test Case 2: key='Jefe', data='what do ya want for nothing?' */
    const uint8_t *rfc_key = (const uint8_t*)"Jefe";
    const uint8_t *rfc_data = (const uint8_t*)"what do ya want for nothing?";
    hmac_md5(rfc_key, 4, rfc_data, 28, mac1);
    /* Check that result is deterministic and non-zero */
    uint8_t md5_copy[16];
    memcpy(md5_copy, mac1, 16);
    hmac_md5(rfc_key, 4, rfc_data, 28, mac2);
    TEST("HMAC-MD5 RFC 2104 TC2 deterministic", memcmp(md5_copy, mac2, 16) == 0);
    TEST("HMAC-MD5 RFC 2104 TC2 non-zero", mac1[0] != 0 || mac1[15] != 0);

    /* HMAC-SHA256 RFC 4231 Test Case 2: key='Jefe', data='what do ya want for nothing?' */
    hmac_sha256(rfc_key, 4, rfc_data, 28, mac1);
    uint8_t sha256_copy[32];
    memcpy(sha256_copy, mac1, 32);
    hmac_sha256(rfc_key, 4, rfc_data, 28, mac2);
    TEST("HMAC-SHA256 RFC 4231 TC2 deterministic", memcmp(sha256_copy, mac2, 32) == 0);
    TEST("HMAC-SHA256 RFC 4231 TC2 non-zero", mac1[0] != 0 || mac1[31] != 0);

    /* HMAC empty key */
    hmac_md5((const uint8_t*)"", 0, (const uint8_t*)"test", 4, mac1);
    TEST("HMAC-MD5 empty key works", mac1[0] != 0 || mac1[15] != 0);

    /* HMAC empty data */
    hmac_md5((const uint8_t*)"key", 3, (const uint8_t*)"", 0, mac1);
    TEST("HMAC-MD5 empty data works", mac1[0] != 0 || mac1[15] != 0);

    /* HMAC-SHA256 empty key + empty data */
    hmac_sha256((const uint8_t*)"", 0, (const uint8_t*)"", 0, mac1);
    TEST("HMAC-SHA256 empty key+data", mac1[0] != 0 || mac1[31] != 0);
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

    /* Known vector: Adler32('Wikipedia') = 0x11E60398 */
    uint32_t wiki = adler32(1, "Wikipedia", 9);
    TEST("Adler32('Wikipedia') = 0x11E60398", wiki == 0x11E60398);

    /* Long buffer */
    char longbuf[1024];
    memset(longbuf, 'x', 1024);
    uint32_t long_a = adler32(1, longbuf, 1024);
    TEST("Adler32 1024-byte buffer non-trivial", long_a != 1);
    /* Chaining 2 halves should match whole */
    uint32_t half1 = adler32(1, longbuf, 512);
    uint32_t half2 = adler32(half1, longbuf + 512, 512);
    TEST("Adler32 512+512 chain matches 1024 direct", half2 == long_a);
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

    /* Specific padding tests from RFC 4648 */
    /* "f" -> "Zg==" */
    elen = base64_encode(enc, (const uint8_t*)"f", 1);
    enc[elen] = '\0';
    TEST("Base64 'f' -> 'Zg=='", strcmp(enc, "Zg==") == 0);
    dlen = base64_decode(dec, enc, elen);
    TEST("Base64 decode 'Zg==' -> 'f'", dlen == 1 && dec[0] == 'f');

    /* "fo" -> "Zm8=" */
    elen = base64_encode(enc, (const uint8_t*)"fo", 2);
    enc[elen] = '\0';
    TEST("Base64 'fo' -> 'Zm8='", strcmp(enc, "Zm8=") == 0);
    dlen = base64_decode(dec, enc, elen);
    TEST("Base64 decode 'Zm8=' -> 'fo'", dlen == 2 && memcmp(dec, "fo", 2) == 0);

    /* Large buffer encode/decode */
    uint8_t large_buf[512];
    for (i = 0; i < 512; i++) large_buf[i] = (uint8_t)(i * 13 + 7);
    char large_enc[1024];
    uint8_t large_dec[512];
    elen = base64_encode(large_enc, large_buf, 512);
    large_enc[elen] = '\0';
    dlen = base64_decode(large_dec, large_enc, elen);
    TEST("Base64 512-byte roundtrip", dlen == 512 && memcmp(large_dec, large_buf, 512) == 0);
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
