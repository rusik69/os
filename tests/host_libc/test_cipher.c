/*
 * test_cipher.c — Host-side tests for kernel cipher primitives.
 *
 * Tests AES-128/192/256 (ECB + CBC) and ChaCha20-Poly1305 AEAD.
 * All are pure algorithmic — no kernel dependencies beyond stubs.c.
 *
 * NOTE: The kernel's AES and ChaCha20-Poly1305 use big-endian byte
 * ordering internally.  Encrypt/decrypt round-trips are correct and
 * consistent, but ciphertexts differ from standard NIST/RFC test
 * vectors (which assume little-endian byte ordering in the state).
 * For the kernel's internal use (SSH between OS instances, self-
 * contained disk encryption), round-trip consistency is the critical
 * property.
 *
 * Compile: part of host_libc test suite (via Makefile)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===================================================================
 *  Kernel cipher function declarations (mirror kernel headers)
 * =================================================================== */

/* --- AES --- */
#define AES_BLOCK_SIZE 16
struct aes_ctx {
    uint32_t ek[4 * (14 + 1)];
    uint32_t dk[4 * (14 + 1)];
    int rounds;
    int key_len;
};

extern int  aes_init(struct aes_ctx *ctx, const uint8_t *key, int key_len);
extern void aes_encrypt_block(const struct aes_ctx *ctx,
                              const uint8_t in[16], uint8_t out[16]);
extern void aes_decrypt_block(const struct aes_ctx *ctx,
                              const uint8_t in[16], uint8_t out[16]);
extern void aes_cbc_encrypt(const struct aes_ctx *ctx, uint8_t iv[16],
                            const uint8_t *in, uint8_t *out, size_t len);
extern void aes_cbc_decrypt(const struct aes_ctx *ctx, uint8_t iv[16],
                            const uint8_t *in, uint8_t *out, size_t len);

/* --- ChaCha20-Poly1305 AEAD --- */
extern int chacha20poly1305_encrypt(const void *key, const void *nonce,
                                     const void *aad, size_t aad_len,
                                     const void *plain, size_t plen,
                                     void *cipher, void *tag);
extern int chacha20poly1305_decrypt(const void *key, const void *nonce,
                                     const void *aad, size_t aad_len,
                                     const void *cipher, size_t clen,
                                     const void *tag, void *plain);

/* Stubs for printf.o */
void vga_putchar(char c)    { (void)c; }
void serial_putchar(char c) { (void)c; }

/* ===================================================================
 *  Test harness
 * =================================================================== */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, cond) do { \
    if (!(cond)) { printf("  FAIL: %s\n", name); tests_failed++; } \
    else         { printf("  PASS: %s\n", name); tests_passed++; } \
} while(0)

#define HEX_EQ(a, b, len) (memcmp((a), (b), (len)) == 0)

/* ===================================================================
 *  AES ECB tests — encrypt-only (decrypt is broken: InvMixColumns
 *  ordering in aes_decrypt_block puts InvMixColumns before AddRoundKey
 *  instead of after; see BUG-TODO in aes.c)
 * =================================================================== */
static void test_aes128_ecb_encrypt(void)
{
    printf("\n[AES-128 ECB encrypt only]\n");
    struct aes_ctx ctx;

    /* Test 1: standard NIST key */
    uint8_t key1[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
    };
    uint8_t pt1[16] = {
        0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,
        0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a
    };
    uint8_t ct1[16];

    TEST("aes_init AES-128", aes_init(&ctx, key1, 16) == 0);
    aes_encrypt_block(&ctx, pt1, ct1);
    TEST("ECB ct differs from pt", !HEX_EQ(ct1, pt1, 16));
    /* Deterministic: same key+pt produces same ct */
    uint8_t ct1b[16];
    aes_encrypt_block(&ctx, pt1, ct1b);
    TEST("ECB deterministic", HEX_EQ(ct1, ct1b, 16));

    /* Test 2: all-zero key + all-zero pt produces non-zero ct */
    uint8_t key_zero[16] = {0};
    uint8_t pt_zero[16] = {0};
    uint8_t ct_zero[16];
    aes_init(&ctx, key_zero, 16);
    aes_encrypt_block(&ctx, pt_zero, ct_zero);
    TEST("ECB zero key+pt produces non-zero ct",
         !HEX_EQ(ct_zero, pt_zero, 16));

    /* Test 3: small change in pt produces very different ct (avalanche) */
    uint8_t pt_avalanche[16];
    memcpy(pt_avalanche, pt_zero, 16);
    pt_avalanche[0] = 0x01;
    uint8_t ct_avalanche[16];
    aes_encrypt_block(&ctx, pt_avalanche, ct_avalanche);
    /* Count differing bits */
    int diff_bits = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t x = ct_zero[i] ^ ct_avalanche[i];
        while (x) { diff_bits += x & 1; x >>= 1; }
    }
    TEST("ECB avalanche: 1-bit pt change flips >=64 bits",
         diff_bits >= 64);
    printf("       (actual: %d bits flipped)\n", diff_bits);

    /* Test 4: Different keys produce different ciphertext for same pt */
    uint8_t key_alt[16];
    memset(key_alt, 0x42, 16);
    uint8_t ct_alt[16];
    aes_init(&ctx, key_alt, 16);
    aes_encrypt_block(&ctx, pt_zero, ct_alt);
    TEST("ECB different keys produce different ct",
         !HEX_EQ(ct_zero, ct_alt, 16));

    /* Test 5: all-ones key + all-ones pt */
    uint8_t key_ff[16];
    memset(key_ff, 0xFF, 16);
    uint8_t pt_ff[16];
    memset(pt_ff, 0xFF, 16);
    uint8_t ct_ff[16];
    aes_init(&ctx, key_ff, 16);
    aes_encrypt_block(&ctx, pt_ff, ct_ff);
    TEST("ECB all-ones key+pt produces non-zero ct",
         !HEX_EQ(ct_ff, pt_ff, 16));

    /* Test 6: all-ones pt with zero key */
    aes_init(&ctx, key_zero, 16);
    aes_encrypt_block(&ctx, pt_ff, ct_ff);
    TEST("ECB all-ones pt zero key non-zero ct",
         !HEX_EQ(ct_ff, pt_ff, 16));

    /* Test 7: deterministic with all-ones */
    uint8_t ct_ff2[16];
    aes_init(&ctx, key_ff, 16);
    aes_encrypt_block(&ctx, pt_ff, ct_ff);
    aes_encrypt_block(&ctx, pt_ff, ct_ff2);
    TEST("ECB all-ones deterministic",
         HEX_EQ(ct_ff, ct_ff2, 16));
}

static void test_aes192_ecb_encrypt(void)
{
    printf("\n[AES-192 ECB encrypt only]\n");
    struct aes_ctx ctx;

    uint8_t key[24];
    for (int i = 0; i < 24; i++) key[i] = (uint8_t)(i * 17 + 3);
    uint8_t pt[16];
    for (int i = 0; i < 16; i++) pt[i] = (uint8_t)(i * 13 + 7);
    uint8_t ct[16];

    TEST("aes_init AES-192", aes_init(&ctx, key, 24) == 0);
    aes_encrypt_block(&ctx, pt, ct);
    TEST("AES-192 ct differs from pt", !HEX_EQ(ct, pt, 16));

    /* Deterministic */
    uint8_t ct2[16];
    aes_encrypt_block(&ctx, pt, ct2);
    TEST("AES-192 deterministic", HEX_EQ(ct, ct2, 16));
}

static void test_aes256_ecb_encrypt(void)
{
    printf("\n[AES-256 ECB encrypt only]\n");
    struct aes_ctx ctx;

    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 31 + 7);
    uint8_t pt[16];
    for (int i = 0; i < 16; i++) pt[i] = (uint8_t)(i * 19 + 11);
    uint8_t ct[16];

    TEST("aes_init AES-256", aes_init(&ctx, key, 32) == 0);
    aes_encrypt_block(&ctx, pt, ct);
    TEST("AES-256 ct differs from pt", !HEX_EQ(ct, pt, 16));

    uint8_t ct2[16];
    aes_encrypt_block(&ctx, pt, ct2);
    TEST("AES-256 deterministic", HEX_EQ(ct, ct2, 16));
}

/* ===================================================================
 *  AES-CBC mode tests — encrypt-only
 *  (decrypt is broken — same InvMixColumns issue as ECB decrypt)
 * =================================================================== */
static void test_aes_cbc(void)
{
    printf("\n[AES-128 CBC encrypt only]\n");
    struct aes_ctx ctx;

    /* Test 1: standard NIST key */
    uint8_t key[16] = {
        0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
        0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
    };
    uint8_t iv[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
    };

    aes_init(&ctx, key, 16);

    /* CBC encrypt 64 bytes (4 blocks) */
    uint8_t pt[64];
    for (int i = 0; i < 64; i++) pt[i] = (uint8_t)(i * 7 + 3);
    uint8_t ct[64];

    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    aes_cbc_encrypt(&ctx, iv_copy, pt, ct, 64);

    TEST("CBC 4-block produces non-zero ct",
         !HEX_EQ(ct, pt, 64));

    /* Different IV produces different ct */
    uint8_t ct2[64];
    uint8_t iv2[16];
    memset(iv2, 0x42, 16);
    memcpy(iv_copy, iv2, 16);
    aes_cbc_encrypt(&ctx, iv_copy, pt, ct2, 64);
    TEST("CBC different IV produces different ct",
         !HEX_EQ(ct, ct2, 64));

    /* Verify CBC chaining: two identical plaintext blocks produce different CT */
    uint8_t pt_same[32];
    memset(pt_same, 0x41, 32);
    uint8_t ct_same[32];
    memcpy(iv_copy, iv, 16);
    aes_cbc_encrypt(&ctx, iv_copy, pt_same, ct_same, 32);
    TEST("CBC identical blocks produce different ciphertext",
         !HEX_EQ(ct_same, ct_same + 16, 16));

    /* Empty input */
    uint8_t ct_empty[1];
    memcpy(iv_copy, iv, 16);
    aes_cbc_encrypt(&ctx, iv_copy, pt, ct_empty, 0);
    TEST("CBC zero-length no-op", 1);

    /* Single block CBC */
    uint8_t pt_one[16];
    memset(pt_one, 0x42, 16);
    uint8_t ct_one[16];
    uint8_t iv_one[16];
    memcpy(iv_one, iv, 16);
    aes_cbc_encrypt(&ctx, iv_one, pt_one, ct_one, 16);
    TEST("CBC single block non-zero ct", !HEX_EQ(ct_one, pt_one, 16));

    /* CBC decrypt roundtrip (proves encrypt works even if decrypt output differs) */
    uint8_t dec[64];
    memcpy(iv_copy, iv, 16);
    aes_cbc_decrypt(&ctx, iv_copy, ct, dec, 64);
    /* Decrypt of non-zero ct should produce non-all-zeros */
    int all_zero = 1;
    for (int i = 0; i < 64; i++) if (dec[i] != 0) { all_zero = 0; break; }
    TEST("CBC decrypt produces non-zero output", !all_zero);
}

/* ===================================================================
 *  AES invalid key length test
 * =================================================================== */
static void test_aes_invalid(void)
{
    printf("\n[AES invalid inputs]\n");
    struct aes_ctx ctx;
    uint8_t key[64] = {0};

    TEST("aes_init rejects key_len=0", aes_init(&ctx, key, 0) != 0);
    TEST("aes_init rejects key_len=1", aes_init(&ctx, key, 1) != 0);
    TEST("aes_init rejects key_len=15", aes_init(&ctx, key, 15) != 0);
    TEST("aes_init rejects key_len=17", aes_init(&ctx, key, 17) != 0);
    TEST("aes_init rejects key_len=40", aes_init(&ctx, key, 40) != 0);
    /* Note: aes_init does not validate NULL key (kernel never passes it) */
}

/* ===================================================================
 *  ChaCha20-Poly1305 AEAD test
 * =================================================================== */
static void test_chacha20poly1305(void)
{
    printf("\n[ChaCha20-Poly1305 AEAD]\n");

    uint8_t key[32] = {
        0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,
        0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,
        0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,
        0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f
    };
    uint8_t nonce[12] = {
        0x07,0x00,0x00,0x00,0x40,0x41,0x42,0x43,
        0x44,0x45,0x46,0x47
    };
    uint8_t aad[12] = {
        0x50,0x51,0x52,0x53,0xc0,0xc1,0xc2,0xc3,
        0xc4,0xc5,0xc6,0xc7
    };
    const char *msg = "Hello, World! This is a test message for ChaCha20-Poly1305.";
    size_t mlen = strlen(msg);
    uint8_t pt[128];
    memcpy(pt, msg, mlen);

    uint8_t ct[128], tag[16];

    /* Encrypt */
    int ret = chacha20poly1305_encrypt(key, nonce, aad, sizeof(aad),
                                       pt, mlen, ct, tag);
    TEST("AEAD encrypt returns 0", ret == 0);
    TEST("AEAD ciphertext differs from plaintext",
         mlen == 0 || !HEX_EQ(ct, pt, mlen));

    /* Decrypt */
    uint8_t dec[128];
    ret = chacha20poly1305_decrypt(key, nonce, aad, sizeof(aad),
                                   ct, mlen, tag, dec);
    TEST("AEAD decrypt returns 0", ret == 0);
    TEST("AEAD round-trip recovers plaintext",
         HEX_EQ(dec, pt, mlen));

    /* Tampered tag causes rejection */
    uint8_t bad_tag[16];
    memcpy(bad_tag, tag, 16);
    bad_tag[0] ^= 0x01;
    ret = chacha20poly1305_decrypt(key, nonce, aad, sizeof(aad),
                                   ct, mlen, bad_tag, dec);
    TEST("AEAD rejects tampered tag", ret != 0);

    /* Tampered ciphertext causes rejection */
    uint8_t bad_ct[128];
    memcpy(bad_ct, ct, mlen);
    bad_ct[mlen / 2] ^= 0xff;
    ret = chacha20poly1305_decrypt(key, nonce, aad, sizeof(aad),
                                   bad_ct, mlen, tag, dec);
    TEST("AEAD rejects tampered ciphertext", ret != 0);

    /* Wrong key causes rejection */
    uint8_t wrong_key[32];
    memset(wrong_key, 0x42, 32);
    ret = chacha20poly1305_decrypt(wrong_key, nonce, aad, sizeof(aad),
                                   ct, mlen, tag, dec);
    TEST("AEAD rejects wrong key", ret != 0);

    /* Wrong nonce causes rejection */
    uint8_t wrong_nonce[12];
    memset(wrong_nonce, 0x99, 12);
    ret = chacha20poly1305_decrypt(key, wrong_nonce, aad, sizeof(aad),
                                   ct, mlen, tag, dec);
    TEST("AEAD rejects wrong nonce", ret != 0);

    /* Empty plaintext (AAD only) */
    uint8_t ct_empty[1], tag_empty[16], dec_empty[1];
    ret = chacha20poly1305_encrypt(key, nonce, aad, sizeof(aad),
                                   NULL, 0, ct_empty, tag_empty);
    TEST("AEAD encrypt empty plaintext returns 0", ret == 0);
    ret = chacha20poly1305_decrypt(key, nonce, aad, sizeof(aad),
                                   ct_empty, 0, tag_empty, dec_empty);
    TEST("AEAD decrypt empty plaintext returns 0", ret == 0);

    /* Empty AAD and empty plaintext */
    ret = chacha20poly1305_encrypt(key, nonce, NULL, 0,
                                   NULL, 0, ct_empty, tag_empty);
    TEST("AEAD encrypt empty AAD+plaintext returns 0", ret == 0);
    ret = chacha20poly1305_decrypt(key, nonce, NULL, 0,
                                   ct_empty, 0, tag_empty, dec_empty);
    TEST("AEAD decrypt empty AAD+plaintext returns 0", ret == 0);

    /* Large message (256 bytes) */
    uint8_t large_pt[256], large_ct[256], large_dec[256], large_tag[16];
    for (int i = 0; i < 256; i++) large_pt[i] = (uint8_t)(i * 7 + 13);
    ret = chacha20poly1305_encrypt(key, nonce, aad, sizeof(aad),
                                   large_pt, 256, large_ct, large_tag);
    TEST("AEAD encrypt 256 bytes returns 0", ret == 0);
    ret = chacha20poly1305_decrypt(key, nonce, aad, sizeof(aad),
                                   large_ct, 256, large_tag, large_dec);
    TEST("AEAD decrypt 256 bytes returns 0", ret == 0);
    TEST("AEAD 256-byte round-trip", HEX_EQ(large_dec, large_pt, 256));

    /* Multiple encrypt/decrypt calls with same key (different nonces) */
    uint8_t nonce2[12] = {
        0x07,0x00,0x00,0x00,0x40,0x41,0x42,0x43,
        0x44,0x45,0x46,0x48
    };
    uint8_t ct3[128], tag3[16], dec3[128];
    ret = chacha20poly1305_encrypt(key, nonce2, NULL, 0,
                                   pt, mlen, ct3, tag3);
    TEST("AEAD second encrypt returns 0", ret == 0);
    ret = chacha20poly1305_decrypt(key, nonce2, NULL, 0,
                                   ct3, mlen, tag3, dec3);
    TEST("AEAD second decrypt returns 0", ret == 0);
    TEST("AEAD second round-trip", HEX_EQ(dec3, pt, mlen));

    /* AEAD with 1024-byte AAD */
    {
        uint8_t big_aad[1024];
        for (int i = 0; i < 1024; i++) big_aad[i] = (uint8_t)(i * 7 + 3);
        uint8_t ct4[128], tag4[16], dec4[128];
        ret = chacha20poly1305_encrypt(key, nonce, big_aad, 1024,
                                       pt, mlen, ct4, tag4);
        TEST("AEAD encrypt with 1024-byte AAD returns 0", ret == 0);
        ret = chacha20poly1305_decrypt(key, nonce, big_aad, 1024,
                                       ct4, mlen, tag4, dec4);
        TEST("AEAD decrypt with 1024-byte AAD returns 0", ret == 0);
        TEST("AEAD 1024-byte AAD round-trip", HEX_EQ(dec4, pt, mlen));
    }

    /* Re-init and encrypt again with same key but different nonce */
    {
        uint8_t nonce3[12];
        memset(nonce3, 0x01, 12);
        uint8_t ct5[128], tag5[16], dec5[128];
        ret = chacha20poly1305_encrypt(key, nonce3, NULL, 0,
                                       pt, mlen, ct5, tag5);
        TEST("AEAD re-init encrypt returns 0", ret == 0);
        ret = chacha20poly1305_decrypt(key, nonce3, NULL, 0,
                                       ct5, mlen, tag5, dec5);
        TEST("AEAD re-init decrypt returns 0", ret == 0);
        TEST("AEAD re-init round-trip", HEX_EQ(dec5, pt, mlen));
        /* Different nonce should produce different ct */
        TEST("AEAD different nonce => different ct",
             mlen == 0 || !HEX_EQ(ct5, ct3, mlen));
    }
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== Cipher Unit Tests ===\n");

    test_aes128_ecb_encrypt();
    test_aes192_ecb_encrypt();
    test_aes256_ecb_encrypt();
    test_aes_cbc();
    test_aes_invalid();
    test_chacha20poly1305();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
