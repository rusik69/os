// SPDX-License-Identifier: GPL-2.0-only
/*
 * RSA public-key cryptography (PKCS#1 v1.5)
 *
 * Supports key sizes up to 4096 bits.
 * Implements RSA encryption, decryption, signing, and verification.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "rng.h"

#define RSA_MAX_BITS 4096
#define RSA_MAX_BYTES (RSA_MAX_BITS / 8)
#define RSA_MAX_WORDS (RSA_MAX_BYTES / 4)

struct rsa_key {
    uint32_t n[RSA_MAX_WORDS]; /* modulus */
    uint32_t e;                /* public exponent */
    uint32_t d[RSA_MAX_WORDS]; /* private exponent */
    uint32_t p[RSA_MAX_WORDS]; /* prime p */
    uint32_t q[RSA_MAX_WORDS]; /* prime q */
    uint32_t dp[RSA_MAX_WORDS];/* d mod (p-1) */
    uint32_t dq[RSA_MAX_WORDS];/* d mod (q-1) */
    uint32_t qinv[RSA_MAX_WORDS]; /* q^(-1) mod p */
    int bits;                  /* key size in bits */
    int nwords;                /* words used */
};

/* Big number helpers */
static void bn_zero(uint32_t *r, int nwords)
{
    memset(r, 0, nwords * sizeof(uint32_t));
}

static void bn_copy(uint32_t *r, const uint32_t *a, int nwords)
{
    memcpy(r, a, nwords * sizeof(uint32_t));
}

/* Compare: returns -1, 0, 1 */
static int bn_cmp(const uint32_t *a, const uint32_t *b, int nwords)
{
    for (int i = nwords - 1; i >= 0; i--) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}

/* r = a - b, returns borrow (0 or 1) */
static uint32_t bn_sub(uint32_t *r, const uint32_t *a, const uint32_t *b, int nwords)
{
    uint64_t borrow = 0;
    for (int i = 0; i < nwords; i++) {
        borrow = (uint64_t)a[i] - (uint64_t)b[i] - (borrow >> 63);
        r[i] = (uint32_t)borrow;
        borrow = ((int64_t)borrow >> 63) & 1;
    }
    return (uint32_t)borrow;
}

/* r = a * b (256-bit result in 2*nwords words) */
static void bn_mul_wide(uint32_t *r, const uint32_t *a, const uint32_t *b, int nwords)
{
    uint64_t carry;
    bn_zero(r, nwords * 2);
    
    for (int i = 0; i < nwords; i++) {
        carry = 0;
        for (int j = 0; j < nwords; j++) {
            carry += (uint64_t)r[i + j] + (uint64_t)a[i] * b[j];
            r[i + j] = (uint32_t)carry;
            carry >>= 32;
        }
        r[i + nwords] = (uint32_t)carry;
    }
}

/* r = a mod m (assumes m > 0) */
static void bn_mod(uint32_t *r, const uint32_t *a, const uint32_t *m, int nwords)
{
    uint32_t tmp[RSA_MAX_WORDS * 2];
    int mbits = 0;
    
    bn_copy(tmp, a, nwords * 2);
    
    /* Find bit length of m */
    uint32_t mtop = m[nwords - 1];
    while (mtop) { mbits++; mtop >>= 1; }
    mbits += (nwords - 1) * 32;
    
    /* Compute bit length of a */
    int abits = 0;
    for (int i = nwords * 2 - 1; i >= 0; i--) {
        if (a[i]) {
            uint32_t atop = a[i];
            abits = i * 32;
            while (atop) { abits++; atop >>= 1; }
            break;
        }
    }
    
    /* Long division (simplified) */
    /* For RSA sizes, repeated subtraction is fine */
    while (abits >= mbits) {
        int shift = abits - mbits;
        uint32_t shifted_m[RSA_MAX_WORDS];
        bn_zero(shifted_m, nwords);
        
        int word_shift = shift / 32;
        int bit_shift = shift % 32;
        
        for (int i = 0; i < nwords; i++) {
            uint64_t val = (uint64_t)m[i] << bit_shift;
            if (i + word_shift < nwords)
                shifted_m[i + word_shift] |= (uint32_t)(val & 0xFFFFFFFF);
            if (i + word_shift + 1 < nwords)
                shifted_m[i + word_shift + 1] |= (uint32_t)(val >> 32);
        }
        
        if (bn_cmp(tmp, shifted_m, nwords * 2) >= 0) {
            bn_sub(tmp, tmp, shifted_m, nwords * 2);
        }
        
        /* Recalculate abits */
        abits = 0;
        for (int i = nwords * 2 - 1; i >= 0; i--) {
            if (tmp[i]) {
                uint32_t atop = tmp[i];
                abits = i * 32;
                while (atop) { abits++; atop >>= 1; }
                break;
            }
        }
    }
    
    /* Result guaranteed to be < m now */
    for (int i = 0; i < nwords; i++)
        r[i] = tmp[i];
}

/* r = a * b mod m */
static void bn_mul_mod(uint32_t *r, const uint32_t *a, const uint32_t *b,
                        const uint32_t *m, int nwords)
{
    uint32_t product[RSA_MAX_WORDS * 2];
    bn_mul_wide(product, a, b, nwords);
    bn_mod(r, product, m, nwords);
}

/* Modular exponentiation: r = base^exp mod m (binary method) */
static void bn_mod_exp(uint32_t *r, const uint32_t *base, const uint32_t *exp,
                        const uint32_t *m, int nwords)
{
    uint32_t result[RSA_MAX_WORDS];
    uint32_t b[RSA_MAX_WORDS];
    int ebits = 0;

    bn_zero(result, nwords);
    result[0] = 1;
    bn_copy(b, base, nwords);

    /* Find bit length of exp */
    for (int i = nwords - 1; i >= 0; i--) {
        if (exp[i]) {
            uint32_t etop = exp[i];
            ebits = i * 32;
            while (etop) { ebits++; etop >>= 1; }
            break;
        }
    }

    for (int i = 0; i < ebits; i++) {
        int word_idx = i / 32;
        int bit_idx = i % 32;
        
        if (exp[word_idx] & (1 << bit_idx)) {
            bn_mul_mod(result, result, b, m, nwords);
        }
        bn_mul_mod(b, b, b, m, nwords);
    }

    bn_copy(r, result, nwords);
}

/* Convert byte array to bignum words (little-endian per word, big-endian byte order) */
static void bytes_to_bn(uint32_t *r, const uint8_t *bytes, int nwords, int nbytes)
{
    bn_zero(r, nwords);
    for (int i = 0; i < nbytes; i++) {
        int word_idx = i / 4;
        int byte_idx = i % 4;
        if (word_idx < nwords)
            r[word_idx] |= (uint32_t)bytes[nbytes - 1 - i] << (byte_idx * 8);
    }
}

/* Convert bignum to bytes */
static void bn_to_bytes(uint8_t *bytes, const uint32_t *r, int nwords, int nbytes)
{
    memset(bytes, 0, nbytes);
    for (int i = 0; i < nbytes; i++) {
        int word_idx = i / 4;
        int byte_idx = i % 4;
        if (word_idx < nwords)
            bytes[nbytes - 1 - i] = (uint8_t)(r[word_idx] >> (byte_idx * 8));
    }
}

/* RSA public encrypt / verify: ciphertext = plaintext^e mod n */
int rsa_public_encrypt(uint8_t *out, size_t *out_len,
                        const uint8_t *in, size_t in_len,
                        const uint8_t *n, size_t n_len,
                        uint32_t e)
{
    int nwords = (int)(n_len / 4);
    uint32_t bn_n[RSA_MAX_WORDS];
    uint32_t bn_in[RSA_MAX_WORDS];
    uint32_t bn_out[RSA_MAX_WORDS];

    if (n_len > RSA_MAX_BYTES || in_len > n_len)
        return -1;

    bytes_to_bn(bn_n, n, nwords, (int)n_len);
    bytes_to_bn(bn_in, in, nwords, (int)n_len);

    /* Convert e to bignum */
    uint32_t bn_e[RSA_MAX_WORDS];
    bn_zero(bn_e, nwords);
    bn_e[0] = e;

    bn_mod_exp(bn_out, bn_in, bn_e, bn_n, nwords);

    *out_len = n_len;
    bn_to_bytes(out, bn_out, nwords, (int)n_len);
    return 0;
}

/* RSA private decrypt / sign: plaintext = ciphertext^d mod n */
int rsa_private_decrypt(uint8_t *out, size_t *out_len,
                         const uint8_t *in, size_t in_len,
                         const uint8_t *n, size_t n_len,
                         const uint8_t *d, size_t d_len)
{
    int nwords = (int)(n_len / 4);
    uint32_t bn_n[RSA_MAX_WORDS];
    uint32_t bn_d[RSA_MAX_WORDS];
    uint32_t bn_in[RSA_MAX_WORDS];
    uint32_t bn_out[RSA_MAX_WORDS];

    if (n_len > RSA_MAX_BYTES || in_len > n_len || d_len > n_len)
        return -1;

    bytes_to_bn(bn_n, n, nwords, (int)n_len);
    bytes_to_bn(bn_d, d, nwords, (int)d_len);
    bytes_to_bn(bn_in, in, nwords, (int)n_len);

    bn_mod_exp(bn_out, bn_in, bn_d, bn_n, nwords);

    *out_len = n_len;
    bn_to_bytes(out, bn_out, nwords, (int)n_len);
    return 0;
}

/* PKCS#1 v1.5 signature verification */
int rsa_pkcs1_v15_verify(const uint8_t *sig, size_t sig_len,
                          const uint8_t *hash, size_t hash_len,
                          const uint8_t *n, size_t n_len,
                          uint32_t e,
                          const uint8_t *digest_info, size_t di_len)
{
    uint8_t decrypted[RSA_MAX_BYTES];
    size_t dec_len = 0;
    int ret;

    ret = rsa_public_encrypt(decrypted, &dec_len, sig, sig_len, n, n_len, e);
    if (ret < 0)
        return -1;

    /* Check PKCS#1 v1.5 padding: 0x00 0x01 [0xFF...] 0x00 digest_info */
    if (dec_len < 11) return -1;
    if (decrypted[0] != 0x00 || decrypted[1] != 0x01)
        return -1;

    size_t pos = 2;
    while (pos < dec_len && decrypted[pos] == 0xFF)
        pos++;
    if (pos >= dec_len || decrypted[pos] != 0x00)
        return -1;
    pos++;

    /* Compare DER-encoded DigestInfo */
    if (pos + hash_len > dec_len)
        return -1;
    if (memcmp(decrypted + pos, digest_info, hash_len) != 0)
        return -1;

    return 0;
}

/* PKCS#1 v1.5 signature generation */
int rsa_pkcs1_v15_sign(uint8_t *sig, size_t *sig_len,
                         const uint8_t *hash, size_t hash_len,
                         const uint8_t *n, size_t n_len,
                         const uint8_t *d, size_t d_len,
                         const uint8_t *digest_info, size_t di_len)
{
    uint8_t padded[RSA_MAX_BYTES];
    size_t mod_bytes = n_len;
    size_t pos;

    if (mod_bytes > RSA_MAX_BYTES || di_len + 3 > mod_bytes)
        return -1;

    /* Build PKCS#1 v1.5 padded message: 0x00 0x01 [FF...] 0x00 DI */
    padded[0] = 0x00;
    padded[1] = 0x01;

    pos = 2;
    /* Fill with 0xFF up to mod_bytes - di_len - 3 */
    size_t pad_len = mod_bytes - di_len - 3;
    memset(padded + pos, 0xFF, pad_len);
    pos += pad_len;
    padded[pos++] = 0x00;
    memcpy(padded + pos, digest_info, di_len);

    return rsa_private_decrypt(sig, sig_len, padded, mod_bytes, n, n_len, d, d_len);
}
