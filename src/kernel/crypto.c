#define KERNEL_INTERNAL
#include "types.h"
#include "crypto.h"
#include "string.h"
#include "printf.h"

/* ── Cryptographic operations ────────────────────────────────────────── */

/* AES S-box */
static const uint8_t aes_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

/* AES inverse S-box */
static const uint8_t aes_inv_sbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
};

/* Round constants */
static const uint8_t aes_rcon[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

static void sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++)
        state[i] = aes_sbox[state[i]];
}

static void inv_sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++)
        state[i] = aes_inv_sbox[state[i]];
}

static void shift_rows(uint8_t state[16]) {
    uint8_t tmp;
    /* Row 1: shift left 1 */
    tmp = state[1];
    state[1] = state[5];
    state[5] = state[9];
    state[9] = state[13];
    state[13] = tmp;
    /* Row 2: shift left 2 */
    tmp = state[2];
    state[2] = state[10];
    state[10] = tmp;
    tmp = state[6];
    state[6] = state[14];
    state[14] = tmp;
    /* Row 3: shift left 3 (right 1) */
    tmp = state[15];
    state[15] = state[11];
    state[11] = state[7];
    state[7] = state[3];
    state[3] = tmp;
}

static void inv_shift_rows(uint8_t state[16]) {
    uint8_t tmp;
    /* Row 1: shift right 1 */
    tmp = state[13];
    state[13] = state[9];
    state[9] = state[5];
    state[5] = state[1];
    state[1] = tmp;
    /* Row 2: shift right 2 */
    tmp = state[2];
    state[2] = state[10];
    state[10] = tmp;
    tmp = state[6];
    state[6] = state[14];
    state[14] = tmp;
    /* Row 3: shift right 1 */
    tmp = state[3];
    state[3] = state[7];
    state[7] = state[11];
    state[11] = state[15];
    state[15] = tmp;
}

static uint8_t galois_mul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

static void mix_columns(uint8_t state[16]) {
    for (int i = 0; i < 4; i++) {
        int col = i * 4;
        uint8_t a0 = state[col];
        uint8_t a1 = state[col + 1];
        uint8_t a2 = state[col + 2];
        uint8_t a3 = state[col + 3];
        state[col]     = galois_mul(2, a0) ^ galois_mul(3, a1) ^ a2 ^ a3;
        state[col + 1] = a0 ^ galois_mul(2, a1) ^ galois_mul(3, a2) ^ a3;
        state[col + 2] = a0 ^ a1 ^ galois_mul(2, a2) ^ galois_mul(3, a3);
        state[col + 3] = galois_mul(3, a0) ^ a1 ^ a2 ^ galois_mul(2, a3);
    }
}

static void inv_mix_columns(uint8_t state[16]) {
    for (int i = 0; i < 4; i++) {
        int col = i * 4;
        uint8_t a0 = state[col];
        uint8_t a1 = state[col + 1];
        uint8_t a2 = state[col + 2];
        uint8_t a3 = state[col + 3];
        state[col]     = galois_mul(14, a0) ^ galois_mul(11, a1) ^ galois_mul(13, a2) ^ galois_mul(9, a3);
        state[col + 1] = galois_mul(9, a0) ^ galois_mul(14, a1) ^ galois_mul(11, a2) ^ galois_mul(13, a3);
        state[col + 2] = galois_mul(13, a0) ^ galois_mul(9, a1) ^ galois_mul(14, a2) ^ galois_mul(11, a3);
        state[col + 3] = galois_mul(11, a0) ^ galois_mul(13, a1) ^ galois_mul(9, a2) ^ galois_mul(14, a3);
    }
}

static void add_round_key(uint8_t state[16], const uint8_t round_key[16]) {
    for (int i = 0; i < 16; i++)
        state[i] ^= round_key[i];
}

static void key_expansion(const uint8_t key[16], uint8_t round_keys[11][16]) {
    /* Copy original key */
    for (int i = 0; i < 16; i++)
        round_keys[0][i] = key[i];

    for (int round = 1; round <= 10; round++) {
        uint8_t temp[4];
        /* RotWord */
        temp[0] = round_keys[round - 1][13];
        temp[1] = round_keys[round - 1][14];
        temp[2] = round_keys[round - 1][15];
        temp[3] = round_keys[round - 1][12];
        /* SubWord */
        for (int i = 0; i < 4; i++)
            temp[i] = aes_sbox[temp[i]];
        /* XOR with Rcon */
        temp[0] ^= aes_rcon[round];

        /* Compute round key */
        for (int i = 0; i < 4; i++)
            round_keys[round][i] = round_keys[round - 1][i] ^ temp[i];
        for (int i = 4; i < 16; i++)
            round_keys[round][i] = round_keys[round - 1][i] ^ round_keys[round][i - 4];
    }
}

static void aes_ecb_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    uint8_t state[16];
    uint8_t round_keys[11][16];

    memcpy(state, in, 16);
    key_expansion(key, round_keys);

    add_round_key(state, round_keys[0]);

    for (int round = 1; round <= 9; round++) {
        sub_bytes(state);
        shift_rows(state);
        mix_columns(state);
        add_round_key(state, round_keys[round]);
    }

    sub_bytes(state);
    shift_rows(state);
    add_round_key(state, round_keys[10]);

    memcpy(out, state, 16);
}

static void aes_ecb_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    uint8_t state[16];
    uint8_t round_keys[11][16];

    memcpy(state, in, 16);
    key_expansion(key, round_keys);

    add_round_key(state, round_keys[10]);
    inv_shift_rows(state);
    inv_sub_bytes(state);

    for (int round = 9; round >= 1; round--) {
        add_round_key(state, round_keys[round]);
        inv_mix_columns(state);
        inv_shift_rows(state);
        inv_sub_bytes(state);
    }

    add_round_key(state, round_keys[0]);
    memcpy(out, state, 16);
}

static uint32_t crypto_xor_checksum(const void *data, size_t len) {
    if (!data || len == 0) return 0;
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t checksum = 0;
    for (size_t i = 0; i < len; i++) {
        checksum ^= ((uint32_t)bytes[i] << ((i % 4) * 8));
    }
    return checksum;
}

/**
 * sha256 - Compute SHA-256 hash of a data buffer
 * @data: Pointer to the input data buffer
 * @len: Length of the input data in bytes
 * @out: Output buffer of 32 bytes to receive the hash digest
 *
 * Stub implementation: currently zeroes the output buffer.
 * In a full implementation this would compute the SHA-256 cryptographic hash.
 *
 * Context: Any context (no locking required for this stub).
 * Return: void (result written to @out).
 */
void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    (void)data;
    (void)len;
    /* Stub: zero out (would implement SHA-256 in full version) */
    memset(out, 0, 32);
}

/* ── Crypto API wrapper (stateful AES context) ─────────────────────── */

static uint8_t crypto_aes_current_key[16];
static int     crypto_aes_key_set = 0;

void crypto_aes_set_key(const uint8_t *key) {
    if (!key) {
        crypto_aes_key_set = 0;
        return;
    }
    memcpy(crypto_aes_current_key, key, 16);
    crypto_aes_key_set = 1;
}

void crypto_aes_encrypt(const uint8_t in[16], uint8_t out[16]) {
    if (!crypto_aes_key_set) {
        /* Fallback: no key set — just copy input to output */
        if (in != out)
            memcpy(out, in, 16);
        return;
    }
    aes_ecb_encrypt(crypto_aes_current_key, in, out);
}

void crypto_aes_decrypt(const uint8_t in[16], uint8_t out[16]) {
    if (!crypto_aes_key_set) {
        if (in != out)
            memcpy(out, in, 16);
        return;
    }
    aes_ecb_decrypt(crypto_aes_current_key, in, out);
}

void crypto_init(void) {
    memset(crypto_aes_current_key, 0, 16);
    crypto_aes_key_set = 0;
    kprintf("[OK] crypto initialized (AES-128 available)\n");
}

/* ── Stub: crypto_skcipher_encrypt ─────────────────────────────────── */
static int crypto_skcipher_encrypt(void *tfm, const uint8_t *src, uint8_t *dst,
                            size_t len, const uint8_t *iv)
{
    (void)tfm; (void)src; (void)dst; (void)len; (void)iv;
    kprintf("[CRYPTO] crypto_skcipher_encrypt: not yet implemented\n");
    return 0;
}

/* ── Stub: crypto_skcipher_decrypt ─────────────────────────────────── */
static int crypto_skcipher_decrypt(void *tfm, const uint8_t *src, uint8_t *dst,
                            size_t len, const uint8_t *iv)
{
    (void)tfm; (void)src; (void)dst; (void)len; (void)iv;
    kprintf("[CRYPTO] crypto_skcipher_decrypt: not yet implemented\n");
    return 0;
}

/* ── Stub: crypto_aead_encrypt ─────────────────────────────────────── */
static int crypto_aead_encrypt(void *tfm, const uint8_t *src, uint8_t *dst,
                        size_t len, const uint8_t *aad, size_t aad_len,
                        const uint8_t *iv)
{
    (void)tfm; (void)src; (void)dst; (void)len;
    (void)aad; (void)aad_len; (void)iv;
    kprintf("[CRYPTO] crypto_aead_encrypt: not yet implemented\n");
    return 0;
}

/* ── Stub: crypto_aead_decrypt ─────────────────────────────────────── */
static int crypto_aead_decrypt(void *tfm, const uint8_t *src, uint8_t *dst,
                        size_t len, const uint8_t *aad, size_t aad_len,
                        const uint8_t *iv)
{
    (void)tfm; (void)src; (void)dst; (void)len;
    (void)aad; (void)aad_len; (void)iv;
    kprintf("[CRYPTO] crypto_aead_decrypt: not yet implemented\n");
    return 0;
}

/* ── Stub: crypto_alloc_skcipher ───────────────────────────────────── */
static void *crypto_alloc_skcipher(const char *alg_name, uint32_t type, uint32_t mask)
{
    (void)alg_name; (void)type; (void)mask;
    kprintf("[CRYPTO] crypto_alloc_skcipher: not yet implemented\n");
    return NULL;
}

/* ── Stub: crypto_alloc_aead ───────────────────────────────────────── */
static void *crypto_alloc_aead(const char *alg_name, uint32_t type, uint32_t mask)
{
    (void)alg_name; (void)type; (void)mask;
    kprintf("[CRYPTO] crypto_alloc_aead: not yet implemented\n");
    return NULL;
}
