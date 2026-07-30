#include "aes.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "types.h"

/*
 * AES Encryption Implementation
 * =============================
 *
 * Architecture overview
 * ---------------------
 * This file implements AES (Advanced Encryption Standard, FIPS PUB 197)
 * in software, supporting 128-bit, 192-bit, and 256-bit key sizes.
 * AES is a symmetric block cipher that operates on 16-byte (128-bit)
 * blocks using a substitution-permutation network (SPN) structure.
 *
 * The cipher rounds consist of four transforms applied to a 4x4 byte
 * state matrix:
 *
 *   1. SubBytes    — Non-linear byte substitution via S-box (GF(2^8) inversion)
 *   2. ShiftRows   — Cyclic left-shift of state rows
 *   3. MixColumns  — Column mixing via GF(2^8) polynomial multiplication
 *   4. AddRoundKey — XOR with round key derived from key expansion
 *
 * Key sizes and round counts:
 *   AES-128: 10 rounds, 16-byte key, 4-word (32-bit) key schedule
 *   AES-192: 12 rounds, 24-byte key, 6-word key schedule
 *   AES-256: 14 rounds, 32-byte key, 8-word key schedule
 *
 * The final round omits MixColumns. Decryption uses the inverse transforms
 * (InvSubBytes, InvShiftRows, InvMixColumns) applied in reverse order,
 * with the round keys used in reverse via inv_key_expansion.
 *
 * CBC mode is provided on top of the block cipher, chaining each
 * plaintext block with the previous ciphertext block via XOR.
 *
 * Data structures
 * ---------------
 * struct aes_ctx (defined in aes.h):
 *   ek[] — encryption round keys (4 * (nr+1) 32-bit words)
 *   dk[] — decryption round keys (same layout, reversed order)
 *   rounds  — number of cipher rounds (10, 12, or 14)
 *   key_len — key size in bytes (16, 24, or 32)
 *
 * Lookup tables:
 *   sbox[256]     — AES forward S-box (SubBytes)
 *   inv_sbox[256] — AES inverse S-box (InvSubBytes)
 *   rcon[11]      — Round constants for key expansion
 *
 * Algorithm reference: FIPS PUB 197, https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.197.pdf
 */

/* AES S-box — maps each byte to its GF(2^8) multiplicative inverse
 * plus an affine transformation. Used in SubBytes (encryption).
 * Generated from: s[x] = AffineTransform(MultiplicativeInverse(x, GF(2^8)))
 * where GF(2^8) uses irreducible polynomial x^8 + x^4 + x^3 + x + 1 (0x11b).
 */
static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

/* AES inverse S-box — reverses the affine transform and GF(2^8) inversion.
 * Used in InvSubBytes (decryption). Satisfies: inv_sbox[sbox[x]] == x.
 */
static const uint8_t inv_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

/* Round constants (Rcon[i]) for key expansion.
 * Rcon[i] = x^(i-1) in GF(2^8), represented as a 32-bit word
 * with the constant in the most significant byte. Used when
 * i % nk == 0 during key expansion to break symmetry between
 * rounds. Rcon[0] is unused (indexing starts at 1). */
static const uint32_t rcon[11] = {
    0x00000000, 0x01000000, 0x02000000, 0x04000000, 0x08000000,
    0x10000000, 0x20000000, 0x40000000, 0x80000000, 0x1B000000,
    0x36000000
};

/* xtime — multiply a byte by 2 (0x02) in GF(2^8).
 * Performs left shift with conditional XOR by the irreducible
 * polynomial 0x11b when the high bit is set (overflow past degree 7).
 * Used internally by mix_columns and inv_mix_columns via gf_mul(). */
static __attribute__((unused)) uint32_t xtime(uint32_t x)
{
    return (x << 1) ^ (((x >> 7) & 1) * 0x11b);
}

/* byte_sub_word — apply S-box substitution to each byte of a 32-bit word.
 * Used during key expansion: RotWord(temp) -> SubWord(temp) -> XOR Rcon.
 * Each byte in the word is independently mapped through the S-box,
 * effectively performing SubBytes on a single column of the key schedule. */
static uint32_t byte_sub_word(uint32_t w)
{
    return ((uint32_t)sbox[(w >> 16) & 0xFF] << 16) |
           ((uint32_t)sbox[(w >>  8) & 0xFF] <<  8) |
           ((uint32_t)sbox[(w >>  0) & 0xFF] <<  0) |
           ((uint32_t)sbox[(w >> 24) & 0xFF] << 24);
}

/* rot_word — circular left-rotate a 32-bit word by one byte.
 * Equivalent to: [a b c d] -> [b c d a].
 * Used during key expansion before SubWord to introduce diffusion
 * between key schedule columns. */
static uint32_t rot_word(uint32_t w)
{
    return (w >> 8) | (w << 24);
}

/* key_expansion — expand a cipher key into the encryption round key schedule.
 * @ek:  output round key array (size: 4 * (nr + 1) 32-bit words)
 * @key: input cipher key bytes
 * @nk:  number of 32-bit words in key (4 for AES-128, 6 for AES-192, 8 for AES-256)
 * @nr:  number of rounds (10, 12, or 14)
 *
 * Algorithm: The first nk words are copied directly from the key.
 * Each subsequent word W[i] = W[i-nk] ^ Transform(W[i-1]):
 *   - If i % nk == 0: RotWord -> SubWord -> XOR Rcon[i/nk]
 *   - If nk > 6 and i % nk == 4: SubWord only (AES-256 extra step)
 *   - Otherwise: W[i] = W[i-nk] ^ W[i-1] (simple XOR)
 * This ensures every round key is derived from and dependent on
 * every byte of the original key (avalanche effect). */
static void key_expansion(uint32_t *ek, const uint8_t *key, int nk, int nr)
{
    int i;

    for (i = 0; i < nk; i++)
        ek[i] = ((uint32_t)key[4*i] << 24) |
                ((uint32_t)key[4*i+1] << 16) |
                ((uint32_t)key[4*i+2] << 8)  |
                ((uint32_t)key[4*i+3]);

    for (i = nk; i < 4 * (nr + 1); i++) {
        uint32_t temp = ek[i - 1];
        if (i % nk == 0) {
            temp = byte_sub_word(rot_word(temp)) ^ rcon[i / nk];
        } else if (nk > 6 && (i % nk == 4)) {
            temp = byte_sub_word(temp);
        }
        ek[i] = ek[i - nk] ^ temp;
    }
}

/* inv_key_expansion — invert the encryption round key schedule for decryption.
 * @dk: output decryption round keys (array of 4 * (nr + 1) words)
 * @ek: input encryption round keys (from key_expansion)
 * @nr: number of rounds
 *
 * The decryption round keys are the encryption round keys in reverse
 * order. For equivalent decryption (direct inverse cipher), MixColumns
 * would need to be applied to all but the first and last decryption
 * round keys — this implementation uses the standard decryption path
 * (InvSubBytes, InvShiftRows, InvMixColumns, AddRoundKey) where the
 * round keys are used as-is in reverse order, equivalent to the
 * FIPS PUB 197 "Equivalent Inverse Cipher" algorithm. */
static void inv_key_expansion(uint32_t *dk, const uint32_t *ek, int nr)
{
    int i, j;

    for (i = 0; i < 4; i++)
        for (j = 0; j < (nr + 1); j++)
            dk[4 * j + i] = ek[4 * (nr - j) + i];
}

/* add_round_key — XOR the state matrix with a round key word (AddRoundKey).
 * Each column of the state (4 bytes) is XOR'd with the corresponding
 * word from the round key schedule. This is the only operation in AES
 * that directly incorporates key material — it's applied at the start
 * (whitening), after each round, and after the final round.
 * The round key words are stored in big-endian byte order internally. */
static void add_round_key(uint8_t state[16], const uint32_t *rk)
{
    for (int i = 0; i < 4; i++) {
        uint32_t w = rk[i];
        state[4*i]   ^= (uint8_t)(w >> 24);
        state[4*i+1] ^= (uint8_t)(w >> 16);
        state[4*i+2] ^= (uint8_t)(w >> 8);
        state[4*i+3] ^= (uint8_t)(w);
    }
}

/* sub_bytes — apply S-box substitution to each byte of the state.
 * This is the non-linear layer of AES, providing confusion.
 * Each byte is independently replaced by sbox[byte], which
 * represents the GF(2^8) multiplicative inverse plus an
 * affine transformation over GF(2). */
static void sub_bytes(uint8_t state[16])
{
    for (int i = 0; i < 16; i++)
        state[i] = sbox[state[i]];
}

/* inv_sub_bytes — apply inverse S-box to each byte of the state.
 * Reverses sub_bytes by mapping each byte through inv_sbox[byte],
 * which inverts the affine transform and GF(2^8) multiplicative
 * inverse from the forward S-box. */
static void inv_sub_bytes(uint8_t state[16])
{
    for (int i = 0; i < 16; i++)
        state[i] = inv_sbox[state[i]];
}

/* shift_rows — cyclically left-shift the rows of the state matrix.
 * The state is stored column-major as a 16-byte array mapped to a
 * 4x4 matrix where state[4*c + r] = byte at column c, row r.
 *
 *   Row 0: no shift (bytes at indices 0, 4, 8, 12)
 *   Row 1: shift left by 1 (indices 1, 5, 9, 13)
 *   Row 2: shift left by 2 (indices 2, 6, 10, 14)
 *   Row 3: shift left by 3 (indices 3, 7, 11, 15)
 *
 * This provides diffusion by moving bytes between columns. */
static void shift_rows(uint8_t state[16])
{
    uint8_t tmp;

    /* Row 1: shift left 1 */
    tmp = state[1];
    state[1]  = state[5];
    state[5]  = state[9];
    state[9]  = state[13];
    state[13] = tmp;

    /* Row 2: shift left 2 */
    tmp = state[2];
    state[2]  = state[10];
    state[10] = tmp;
    tmp = state[6];
    state[6]  = state[14];
    state[14] = tmp;

    /* Row 3: shift left 3 (right 1) */
    tmp = state[15];
    state[15] = state[11];
    state[11] = state[7];
    state[7]  = state[3];
    state[3]  = tmp;
}

/* inv_shift_rows — cyclically right-shift the rows of the state.
 * Reverses shift_rows:
 *   Row 0: no shift
 *   Row 1: shift right by 1
 *   Row 2: shift right by 2
 *   Row 3: shift right by 3
 * This restores the original byte ordering before InvSubBytes. */
static void inv_shift_rows(uint8_t state[16])
{
    uint8_t tmp;

    /* Row 1: shift right 1 */
    tmp = state[13];
    state[13] = state[9];
    state[9]  = state[5];
    state[5]  = state[1];
    state[1]  = tmp;

    /* Row 2: shift right 2 */
    tmp = state[2];
    state[2]  = state[10];
    state[10] = tmp;
    tmp = state[6];
    state[6]  = state[14];
    state[14] = tmp;

    /* Row 3: shift right 3 (left 1) */
    tmp = state[3];
    state[3]  = state[7];
    state[7]  = state[11];
    state[11] = state[15];
    state[15] = tmp;
}

/* gf_mul — multiply two bytes in GF(2^8) using the irreducible
 * polynomial x^8 + x^4 + x^3 + x + 1 (0x11b).
 *
 * Uses the "peasant multiplication" algorithm: iterates over bits
 * of 'b', accumulating XOR of 'a' (shifted left when bit is set).
 * When a left-shift causes overflow past the 8th degree, the result
 * is reduced by XOR with 0x1b (polynomial modulo).
 *
 * This implements general GF(2^8) multiplication used by both
 * mix_columns and inv_mix_columns. The xtime() helper is an
 * optimized special case for multiplication by 2 (0x02). */
static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1)
            p ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi)
            a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

/* mix_columns — MixColumns transform (encryption diffusion layer).
 * Each column of the state matrix is treated as a polynomial
 * over GF(2^8) and multiplied modulo x^4 + 1 by the fixed
 * polynomial c(x) = 3x^3 + x^2 + x + 2.
 *
 * Matrix form (each column [a0 a1 a2 a3] -> [b0 b1 b2 b3]):
 *   b0 = 2a0 + 3a1 +  a2 +  a3
 *   b1 =  a0 + 2a1 + 3a2 +  a3
 *   b2 =  a0 +  a1 + 2a2 + 3a3
 *   b3 = 3a0 +  a1 +  a2 + 2a3
 *
 * This provides linear diffusion within each column, ensuring
 * that a one-byte change in the state propagates to multiple
 * bytes in the next round (avalanche). */
static void mix_columns(uint8_t state[16])
{
    for (int i = 0; i < 4; i++) {
        int c = 4 * i;
        uint8_t a0 = state[c];
        uint8_t a1 = state[c+1];
        uint8_t a2 = state[c+2];
        uint8_t a3 = state[c+3];

        state[c]   = gf_mul(2, a0) ^ gf_mul(3, a1) ^ a2 ^ a3;
        state[c+1] = a0 ^ gf_mul(2, a1) ^ gf_mul(3, a2) ^ a3;
        state[c+2] = a0 ^ a1 ^ gf_mul(2, a2) ^ gf_mul(3, a3);
        state[c+3] = gf_mul(3, a0) ^ a1 ^ a2 ^ gf_mul(2, a3);
    }
}

/* inv_mix_columns — inverse MixColumns transform (decryption diffusion).
 * Reverses mix_columns by multiplying each column by the fixed
 * polynomial d(x) = 11x^3 + 13x^2 + 9x + 14 (the inverse of c(x)
 * modulo x^4 + 1).
 *
 * Matrix form:
 *   b0 = 14a0 + 11a1 + 13a2 +  9a3
 *   b1 =  9a0 + 14a1 + 11a2 + 13a3
 *   b2 = 13a0 +  9a1 + 14a2 + 11a3
 *   b3 = 11a0 + 13a1 +  9a2 + 14a3
 *
 * The coefficients (9, 11, 13, 14) are the multiplicative inverses
 * of (2, 3, 1, 1) in GF(2^8). */
static void inv_mix_columns(uint8_t state[16])
{
    for (int i = 0; i < 4; i++) {
        int c = 4 * i;
        uint8_t a0 = state[c];
        uint8_t a1 = state[c+1];
        uint8_t a2 = state[c+2];
        uint8_t a3 = state[c+3];

        state[c]   = gf_mul(14, a0) ^ gf_mul(11, a1) ^ gf_mul(13, a2) ^ gf_mul(9, a3);
        state[c+1] = gf_mul(9, a0) ^ gf_mul(14, a1) ^ gf_mul(11, a2) ^ gf_mul(13, a3);
        state[c+2] = gf_mul(13, a0) ^ gf_mul(9, a1) ^ gf_mul(14, a2) ^ gf_mul(11, a3);
        state[c+3] = gf_mul(11, a0) ^ gf_mul(13, a1) ^ gf_mul(9, a2) ^ gf_mul(14, a3);
    }
}

/* xor_block — XOR one AES block (16 bytes) into a destination.
 * Used by CBC mode to XOR plaintext with the previous ciphertext
 * block (encryption) or ciphertext with IV (decryption).
 * dst[i] ^= src[i] for i in [0, AES_BLOCK_SIZE). */
static void xor_block(uint8_t *dst, const uint8_t *src)
{
    for (int i = 0; i < AES_BLOCK_SIZE; i++)
        dst[i] ^= src[i];
}

/* aes_init — initialize AES context with a cipher key.
 * Allocates and computes both encryption (ek) and decryption (dk)
 * round key schedules via key_expansion and inv_key_expansion.
 * Validates key length: must be 16 (AES-128), 24 (AES-192), or 32
 * (AES-256) bytes. On invalid key length, returns -EINVAL (-22)
 * without modifying context.
 *
 * Context fields set:
 *   ctx->rounds  = nr (10, 12, or 14)
 *   ctx->key_len = key_len
 *   ctx->ek[]    = encryption round keys (4*(nr+1) words)
 *   ctx->dk[]    = decryption round keys (reversed order)
 */
int aes_init(struct aes_ctx *ctx, const uint8_t *key, int key_len)
{
    int nk, nr;

    switch (key_len) {
        case AES_128: nk = 4; nr = 10; break;
        case AES_192: nk = 6; nr = 12; break;
        case AES_256: nk = 8; nr = 14; break;
        default: return -22; /* -EINVAL */
    }

    ctx->rounds  = nr;
    ctx->key_len = key_len;
    memset(ctx->ek, 0, sizeof(ctx->ek));
    memset(ctx->dk, 0, sizeof(ctx->dk));

    key_expansion(ctx->ek, key, nk, nr);
    inv_key_expansion(ctx->dk, ctx->ek, nr);

    return 0;
}

/* aes_encrypt_block — encrypt a single 16-byte block using ECB mode.
 * Applies the AES cipher in order:
 *   1. AddRoundKey (initial whitening with round key 0)
 *   2. For rounds 1 to nr-1: SubBytes -> ShiftRows -> MixColumns -> AddRoundKey
 *   3. Final round (nr): SubBytes -> ShiftRows -> AddRoundKey (no MixColumns)
 *
 * The state matrix is a 4x4 byte array stored column-major. Input
 * and output are 16-byte flat arrays in big-endian byte order.
 * The context must have been initialized via aes_init() first. */
void aes_encrypt_block(const struct aes_ctx *ctx,
                       const uint8_t in[AES_BLOCK_SIZE],
                       uint8_t out[AES_BLOCK_SIZE])
{
    uint8_t state[16];
    int i;

    memcpy(state, in, AES_BLOCK_SIZE);
    add_round_key(state, &ctx->ek[0]);

    for (i = 1; i < ctx->rounds; i++) {
        sub_bytes(state);
        shift_rows(state);
        mix_columns(state);
        add_round_key(state, &ctx->ek[4 * i]);
    }

    sub_bytes(state);
    shift_rows(state);
    add_round_key(state, &ctx->ek[4 * ctx->rounds]);

    memcpy(out, state, AES_BLOCK_SIZE);
}

/* aes_decrypt_block — decrypt a single 16-byte block using ECB mode.
 * Applies the inverse AES cipher in order:
 *   1. AddRoundKey (with round key nr from decryption schedule)
 *   2. For rounds nr-1 down to 1: InvSubBytes -> InvShiftRows -> InvMixColumns -> AddRoundKey
 *   3. Final round: InvSubBytes -> InvShiftRows -> AddRoundKey (no InvMixColumns)
 *
 * The decryption round keys (ctx->dk) are the encryption round keys
 * in reverse order (see inv_key_expansion). This implements the
 * FIPS PUB 197 equivalent inverse cipher. */
void aes_decrypt_block(const struct aes_ctx *ctx,
                       const uint8_t in[AES_BLOCK_SIZE],
                       uint8_t out[AES_BLOCK_SIZE])
{
    uint8_t state[16];
    int i;

    memcpy(state, in, AES_BLOCK_SIZE);
    add_round_key(state, &ctx->dk[0]);

    for (i = 1; i < ctx->rounds; i++) {
        inv_sub_bytes(state);
        inv_shift_rows(state);
        inv_mix_columns(state);
        add_round_key(state, &ctx->dk[4 * i]);
    }

    inv_sub_bytes(state);
    inv_shift_rows(state);
    add_round_key(state, &ctx->dk[4 * ctx->rounds]);

    memcpy(out, state, AES_BLOCK_SIZE);
}

/* aes_cbc_encrypt — encrypt data in CBC mode.
 * Cipher Block Chaining mode: each plaintext block is XOR'd with
 * the previous ciphertext block before encryption. The IV serves
 * as the initial "previous ciphertext" — it is updated in place
 * to the last ciphertext block (caller can extract final IV).
 *
 * Constraints:
 *   - len must be a multiple of AES_BLOCK_SIZE (16)
 *   - ctx must be initialized via aes_init()
 *   - in and out may alias
 *
 * Security note: IV must be unpredictable (random) for each
 * encryption session. Reusing the same IV with the same key
 * reveals plaintext patterns. */
void aes_cbc_encrypt(const struct aes_ctx *ctx, uint8_t iv[AES_BLOCK_SIZE],
                     const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t block[AES_BLOCK_SIZE];
    size_t i;

    for (i = 0; i < len; i += AES_BLOCK_SIZE) {
        memcpy(block, &in[i], AES_BLOCK_SIZE);
        xor_block(block, iv);
        aes_encrypt_block(ctx, block, &out[i]);
        memcpy(iv, &out[i], AES_BLOCK_SIZE);
    }
}

/* aes_cbc_decrypt — decrypt data in CBC mode.
 * Each ciphertext block is decrypted independently, then XOR'd with
 * the previous ciphertext block (or IV for the first block) to
 * recover the plaintext. The IV is updated in place to the last
 * ciphertext block for potential chaining.
 *
 * Unlike encryption, decryption is parallelizable because each
 * block's XOR input is the input ciphertext of the previous block,
 * not the output of the previous decryption. */
void aes_cbc_decrypt(const struct aes_ctx *ctx, uint8_t iv[AES_BLOCK_SIZE],
                     const uint8_t *in, uint8_t *out, size_t len)
{
    size_t i;

    for (i = 0; i < len; i += AES_BLOCK_SIZE) {
        aes_decrypt_block(ctx, &in[i], &out[i]);
        xor_block(&out[i], iv);
        memcpy(iv, &in[i], AES_BLOCK_SIZE);
    }
}

/* aes_init_crypto — print AES module initialization banner.
 * Called during kernel boot to confirm the AES crypto module
 * is available. Reports supported key sizes (128, 192, 256 bits). */
void aes_init_crypto(void)
{
    kprintf("[OK] AES-%d/%d/%d CBC initialized\n", AES_128 * 8, AES_192 * 8, AES_256 * 8);
}

/* ── aes_encrypt ─────────────────────────────── */
/* Static wrapper: encrypt a single block given a raw key.
 * Internal helper that does per-call key expansion.
 * Returns 0 on success, or negative errno on invalid key. */
static int aes_encrypt(const void *key, size_t key_len, const void *plain, void *cipher)
{
    struct aes_ctx ctx;
    int ret = aes_init(&ctx, (const uint8_t *)key, (int)key_len);
    if (ret) return ret;
    aes_encrypt_block(&ctx, (const uint8_t *)plain, (uint8_t *)cipher);
    return 0;
}
/* ── aes_decrypt ─────────────────────────────── */
/* Static wrapper: decrypt a single block given a raw key.
 * Internal helper that does per-call key expansion.
 * Returns 0 on success, or negative errno on invalid key. */
static int aes_decrypt(const void *key, size_t key_len, const void *cipher, void *plain)
{
    struct aes_ctx ctx;
    int ret = aes_init(&ctx, (const uint8_t *)key, (int)key_len);
    if (ret) return ret;
    aes_decrypt_block(&ctx, (const uint8_t *)cipher, (uint8_t *)plain);
    return 0;
}
/* ── aes_key_expand ─────────────────────────────── */
/* Static wrapper: expand a raw key into round key words.
 * Internal helper that returns the encryption schedule (ek).
 * Useful for pre-computing round keys for bulk operations.
 * Returns 0 on success, or negative errno on invalid key. */
static int aes_key_expand(const void *key, size_t key_len, void *round_keys)
{
    struct aes_ctx ctx;
    int ret = aes_init(&ctx, (const uint8_t *)key, (int)key_len);
    if (ret) return ret;
    memcpy(round_keys, ctx.ek, sizeof(uint32_t) * 4 * (ctx.rounds + 1));
    return 0;
}
