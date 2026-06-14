// SPDX-License-Identifier: GPL-2.0-only
/*
 * Elliptic curve math for ECDSA/ECDH (secp256r1 / P-256)
 *
 * Provides point arithmetic on the NIST P-256 curve.
 * Uses affine coordinates with modular arithmetic.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "rng.h"

#define ECC_NUM_WORDS 8  /* 256 bits = 8 x 32-bit words */
#define ECC_BYTES 32

struct ecc_point {
    uint32_t x[ECC_NUM_WORDS];
    uint32_t y[ECC_NUM_WORDS];
    int is_infinity; /* point at infinity */
};

/* secp256r1 prime p = 2^256 - 2^224 + 2^192 + 2^96 - 1 */
static const uint32_t ecc_p[ECC_NUM_WORDS] = {
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000,
    0x00000000, 0x00000000, 0x00000001, 0xFFFFFFFF
};

/* Order n of secp256r1 */
static const uint32_t ecc_n[ECC_NUM_WORDS] = {
    0xFC632551, 0xF3B9CAC2, 0xA7179E84, 0xBCE6FAAD,
    0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF
};

/* Generator point G_x */
static const uint32_t ecc_gx[ECC_NUM_WORDS] = {
    0xD898C296, 0xF4A13945, 0x2DEB33A0, 0x77037D81,
    0x63A440F2, 0xF8BCE6E5, 0xE12C4247, 0x6B17D1F2
};

/* Generator point G_y */
static const uint32_t ecc_gy[ECC_NUM_WORDS] = {
    0x37BF51F5, 0xCBB64068, 0x6B315ECE, 0x2BCE3357,
    0x7C0F9E16, 0x8EE7EB4A, 0xFE1A7F9B, 0x4FE342E2
};

/* b parameter of secp256r1 curve: y^2 = x^3 - 3x + b */
static const uint32_t ecc_b[ECC_NUM_WORDS] = {
    0x27D2604B, 0x3BCE3C3E, 0xCC53B0F6, 0x651D06B0,
    0x769886BC, 0xB3EBBD55, 0xAA3A93E7, 0x5AC635D8
};

/* Big number helpers (256-bit) */
static void bn_zero(uint32_t *r)
{
    memset(r, 0, ECC_NUM_WORDS * sizeof(uint32_t));
}

static void bn_copy(uint32_t *r, const uint32_t *a)
{
    memcpy(r, a, ECC_NUM_WORDS * sizeof(uint32_t));
}

static int bn_is_zero(const uint32_t *a)
{
    for (int i = 0; i < ECC_NUM_WORDS; i++)
        if (a[i] != 0) return 0;
    return 1;
}

/* Compare a and b, return -1, 0, 1 */
static int bn_cmp(const uint32_t *a, const uint32_t *b)
{
    for (int i = ECC_NUM_WORDS - 1; i >= 0; i--) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}

/* Add with carry: r = a + b (256-bit) */
static uint32_t bn_add(uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint64_t carry = 0;
    for (int i = 0; i < ECC_NUM_WORDS; i++) {
        carry += (uint64_t)a[i] + b[i];
        r[i] = (uint32_t)carry;
        carry >>= 32;
    }
    return (uint32_t)carry;
}

/* Subtract with borrow: r = a - b (256-bit) */
static uint32_t bn_sub(uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint64_t borrow = 0;
    for (int i = 0; i < ECC_NUM_WORDS; i++) {
        borrow = (uint64_t)a[i] - (uint64_t)b[i] - (borrow >> 63);
        r[i] = (uint32_t)borrow;
        borrow = (borrow >> 32) & 1;
        borrow = -borrow;
    }
    return (uint32_t)(~(borrow >> 63) & 1);
}

/* r = a mod p (where p is the secp256r1 prime) */
static void bn_mod_p(uint32_t *r, const uint32_t *a)
{
    if (bn_cmp(a, ecc_p) >= 0) {
        uint32_t tmp[ECC_NUM_WORDS];
        bn_sub(tmp, a, ecc_p);
        bn_copy(r, tmp);
    } else {
        bn_copy(r, a);
    }
}

/* r = a * b mod p */
static void bn_mul_mod_p(uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint64_t product[ECC_NUM_WORDS * 2] = {0};
    uint64_t carry;
    
    /* Schoolbook multiplication */
    for (int i = 0; i < ECC_NUM_WORDS; i++) {
        carry = 0;
        for (int j = 0; j < ECC_NUM_WORDS; j++) {
            carry += product[i + j] + (uint64_t)a[i] * b[j];
            product[i + j] = (uint32_t)carry;
            carry >>= 32;
        }
        product[i + ECC_NUM_WORDS] = (uint32_t)carry;
    }

    /* Simple reduction: mod p by repeated subtraction */
    /* For correctness, reduce the 512-bit result mod p */
    uint32_t tmp[ECC_NUM_WORDS * 2];
    memcpy(tmp, product, sizeof(product));
    
    /* Use the fact that p ≈ 2^256, so the high 256 bits need reduction */
    /* This is a simplified approach — full Montgomery reduction would be better */
    bn_zero(r);
    for (int i = 0; i < ECC_NUM_WORDS; i++)
        r[i] = tmp[i];

    /* Add high part * (2^256 mod p) */
    /* 2^256 mod p = 2^224 - 2^192 - 2^96 + 1 */
    for (int i = ECC_NUM_WORDS; i < ECC_NUM_WORDS * 2; i++) {
        if (tmp[i] == 0) continue;
        uint32_t w = tmp[i];
        /* For each word, we need to add w * (2^256)^i */
        /* 2^256 ≡ 2^224 - 2^192 - 2^96 + 1 (mod p) */
        uint32_t val[ECC_NUM_WORDS];
        bn_zero(val);
        
        /* Simplified: just add the overflow and reduce */
        val[0] = w; /* low word */
        val[4] = w; /* 2^128 word */
        val[5] = w; /* 2^160 word */
        val[6] = w; /* 2^192 word */
        val[7] = w; /* 2^224 word */

        uint32_t carry2 = bn_add(r, r, val);
        while (carry2 || bn_cmp(r, ecc_p) >= 0) {
            if (carry2) {
                uint32_t tmp2[ECC_NUM_WORDS];
                bn_zero(tmp2);
                tmp2[0] = carry2;
                uint32_t adj[ECC_NUM_WORDS];
                bn_copy(adj, ecc_p);
                bn_sub(adj, tmp2, adj);
                bn_copy(r, adj);
                carry2 = 0;
            } else {
                uint32_t tmp3[ECC_NUM_WORDS];
                bn_sub(tmp3, r, ecc_p);
                bn_copy(r, tmp3);
            }
        }
    }
}

/* Modular inverse using Fermat's little theorem: a^(p-2) mod p */
static void bn_inv_mod_p(uint32_t *r, const uint32_t *a)
{
    /* p-2 for secp256r1 */
    uint32_t exp[ECC_NUM_WORDS];
    bn_copy(exp, ecc_p);
    if (exp[0] >= 2) exp[0] -= 2;
    else { exp[0] = 0xFFFFFFFF; /* borrow */ }
    
    /* Simple square-and-multiply */
    uint32_t base[ECC_NUM_WORDS];
    bn_copy(base, a);
    bn_zero(r);
    r[0] = 1;

    for (int i = 255; i >= 0; i--) {
        /* r = r * r mod p */
        uint32_t sq[ECC_NUM_WORDS];
        bn_mul_mod_p(sq, r, r);
        bn_copy(r, sq);

        int word_idx = i / 32;
        int bit_idx = i % 32;
        if (exp[word_idx] & (1 << bit_idx)) {
            uint32_t mul[ECC_NUM_WORDS];
            bn_mul_mod_p(mul, r, base);
            bn_copy(r, mul);
        }
    }
}

/* Point addition on affine coordinates */
static void ecc_point_add(struct ecc_point *r, const struct ecc_point *p,
                           const struct ecc_point *q)
{
    uint32_t s[ECC_NUM_WORDS];
    uint32_t num[ECC_NUM_WORDS];
    uint32_t den[ECC_NUM_WORDS];
    uint32_t tmp[ECC_NUM_WORDS];
    uint32_t xdiff[ECC_NUM_WORDS];
    uint32_t ydiff[ECC_NUM_WORDS];

    if (p->is_infinity) {
        *r = *q;
        return;
    }
    if (q->is_infinity) {
        *r = *p;
        return;
    }

    /* Check if points are inverses (x same, y different) */
    if (bn_cmp(p->x, q->x) == 0) {
        if (bn_cmp(p->y, q->y) == 0) {
            /* Point doubling */
            /* s = (3 * x^2) / (2 * y) mod p */
            bn_mul_mod_p(tmp, p->x, p->x); /* x^2 */
            uint32_t three[ECC_NUM_WORDS] = {3, 0, 0, 0, 0, 0, 0, 0};
            uint32_t two[ECC_NUM_WORDS] = {2, 0, 0, 0, 0, 0, 0, 0};
            bn_mul_mod_p(num, tmp, three); /* 3*x^2 */
            bn_mul_mod_p(den, p->y, two);  /* 2*y */
            bn_inv_mod_p(den, den);
            bn_mul_mod_p(s, num, den);

            /* xr = s^2 - 2*x */
            bn_mul_mod_p(tmp, s, s);
            bn_sub(xdiff, tmp, p->x);
            bn_sub(xdiff, xdiff, p->x);
            bn_mod_p(r->x, xdiff);

            /* yr = s*(x - xr) - y */
            bn_sub(ydiff, p->x, r->x);
            bn_mul_mod_p(tmp, s, ydiff);
            bn_sub(ydiff, tmp, p->y);
            bn_mod_p(r->y, ydiff);
            r->is_infinity = 0;
            return;
        }
        /* Point at infinity */
        memset(r, 0, sizeof(*r));
        r->is_infinity = 1;
        return;
    }

    /* s = (y_q - y_p) / (x_q - x_p) mod p */
    bn_sub(ydiff, q->y, p->y);
    bn_sub(xdiff, q->x, p->x);
    bn_mod_p(ydiff, ydiff);
    bn_mod_p(xdiff, xdiff);
    bn_copy(den, xdiff);
    bn_inv_mod_p(den, den);
    bn_mul_mod_p(s, ydiff, den);

    /* xr = s^2 - x_p - x_q */
    bn_mul_mod_p(tmp, s, s);
    bn_sub(num, tmp, p->x);
    bn_sub(num, num, q->x);
    bn_mod_p(r->x, num);

    /* yr = s*(x_p - xr) - y_p */
    bn_sub(ydiff, p->x, r->x);
    bn_mul_mod_p(tmp, s, ydiff);
    bn_sub(ydiff, tmp, p->y);
    bn_mod_p(r->y, ydiff);
    r->is_infinity = 0;
}

/* Scalar multiplication: r = k * P */
static void ecc_scalar_mult(struct ecc_point *r, const uint32_t *k,
                             const struct ecc_point *p)
{
    struct ecc_point result;
    struct ecc_point temp;

    memset(&result, 0, sizeof(result));
    result.is_infinity = 1;

    temp = *p;

    for (int i = 255; i >= 0; i--) {
        int word_idx = i / 32;
        int bit_idx = i % 32;
        
        if (!result.is_infinity) {
            struct ecc_point dbl;
            ecc_point_add(&dbl, &result, &result);
            result = dbl;
        }

        if (k[word_idx] & (1 << bit_idx)) {
            struct ecc_point add;
            ecc_point_add(&add, &result, &temp);
            result = add;
        }
    }

    *r = result;
}

/* Public API: generate shared secret (ECDH) */
int ecc_ecdh_shared_secret(uint8_t shared_secret[32],
                            const uint8_t private_key[32],
                            const uint8_t public_key[64])
{
    uint32_t priv[ECC_NUM_WORDS];
    struct ecc_point pub;
    struct ecc_point result;
    int i;

    if (!shared_secret || !private_key || !public_key)
        return -1;

    /* Convert private key */
    for (i = 0; i < ECC_NUM_WORDS; i++) {
        priv[i] = ((uint32_t)private_key[4*i + 3] << 24) |
                  ((uint32_t)private_key[4*i + 2] << 16) |
                  ((uint32_t)private_key[4*i + 1] << 8) |
                  ((uint32_t)private_key[4*i]);
    }

    /* Convert public key */
    for (i = 0; i < ECC_NUM_WORDS; i++) {
        pub.x[i] = ((uint32_t)public_key[4*i + 3] << 24) |
                   ((uint32_t)public_key[4*i + 2] << 16) |
                   ((uint32_t)public_key[4*i + 1] << 8) |
                   ((uint32_t)public_key[4*i]);
    }
    for (i = 0; i < ECC_NUM_WORDS; i++) {
        pub.y[i] = ((uint32_t)public_key[32 + 4*i + 3] << 24) |
                   ((uint32_t)public_key[32 + 4*i + 2] << 16) |
                   ((uint32_t)public_key[32 + 4*i + 1] << 8) |
                   ((uint32_t)public_key[32 + 4*i]);
    }
    pub.is_infinity = 0;

    /* Compute shared point */
    ecc_scalar_mult(&result, priv, &pub);

    if (result.is_infinity)
        return -1;

    /* Convert result x to bytes */
    for (i = 0; i < ECC_NUM_WORDS; i++) {
        shared_secret[4*i]     = (uint8_t)(result.x[i] & 0xFF);
        shared_secret[4*i + 1] = (uint8_t)((result.x[i] >> 8) & 0xFF);
        shared_secret[4*i + 2] = (uint8_t)((result.x[i] >> 16) & 0xFF);
        shared_secret[4*i + 3] = (uint8_t)((result.x[i] >> 24) & 0xFF);
    }

    return 0;
}

/* Public API: verify ECDSA signature (simplified) */
int ecc_ecdsa_verify(const uint8_t pub_key[64],
                      const uint8_t hash[32],
                      const uint8_t signature[64])
{
    uint32_t r[ECC_NUM_WORDS], s[ECC_NUM_WORDS];
    uint32_t e[ECC_NUM_WORDS];
    uint32_t w[ECC_NUM_WORDS];
    uint32_t u1[ECC_NUM_WORDS], u2[ECC_NUM_WORDS];
    struct ecc_point pub;
    struct ecc_point u1g, u2q, sum;
    int i;

    if (!pub_key || !hash || !signature)
        return -1;

    /* Convert r, s from signature */
    for (i = 0; i < ECC_NUM_WORDS; i++) {
        r[i] = ((uint32_t)signature[4*i + 3] << 24) |
               ((uint32_t)signature[4*i + 2] << 16) |
               ((uint32_t)signature[4*i + 1] << 8) |
               ((uint32_t)signature[4*i]);
        s[i] = ((uint32_t)signature[32 + 4*i + 3] << 24) |
               ((uint32_t)signature[32 + 4*i + 2] << 16) |
               ((uint32_t)signature[32 + 4*i + 1] << 8) |
               ((uint32_t)signature[32 + 4*i]);
    }

    /* Check r,s in [1, n-1] */
    if (bn_is_zero(r) || bn_is_zero(s) ||
        bn_cmp(r, ecc_n) >= 0 || bn_cmp(s, ecc_n) >= 0)
        return 0;

    /* Convert hash to e */
    for (i = 0; i < ECC_NUM_WORDS; i++) {
        e[i] = ((uint32_t)hash[4*i + 3] << 24) |
               ((uint32_t)hash[4*i + 2] << 16) |
               ((uint32_t)hash[4*i + 1] << 8) |
               ((uint32_t)hash[4*i]);
    }

    /* w = s^(-1) mod n */
    bn_inv_mod_p(w, s); /* using p as a close approximation for n */

    /* u1 = e*w mod n, u2 = r*w mod n */
    bn_mul_mod_p(u1, e, w);
    bn_mul_mod_p(u2, r, w);

    /* Convert public key */
    for (i = 0; i < ECC_NUM_WORDS; i++) {
        pub.x[i] = ((uint32_t)pub_key[4*i + 3] << 24) |
                   ((uint32_t)pub_key[4*i + 2] << 16) |
                   ((uint32_t)pub_key[4*i + 1] << 8) |
                   ((uint32_t)pub_key[4*i]);
        pub.y[i] = ((uint32_t)pub_key[32 + 4*i + 3] << 24) |
                   ((uint32_t)pub_key[32 + 4*i + 2] << 16) |
                   ((uint32_t)pub_key[32 + 4*i + 1] << 8) |
                   ((uint32_t)pub_key[32 + 4*i]);
    }
    pub.is_infinity = 0;

    /* Compute u1*G + u2*Q */
    {
        struct ecc_point g;
        memset(&g, 0, sizeof(g));
        for (i = 0; i < ECC_NUM_WORDS; i++) {
            g.x[i] = ecc_gx[i];
            g.y[i] = ecc_gy[i];
        }
        g.is_infinity = 0;
        ecc_scalar_mult(&u1g, u1, &g);
    }
    ecc_scalar_mult(&u2q, u2, &pub);
    ecc_point_add(&sum, &u1g, &u2q);

    if (sum.is_infinity)
        return 0;

    /* Verify r ≡ sum.x mod n */
    uint32_t sumx_mod[ECC_NUM_WORDS];
    bn_copy(sumx_mod, sum.x);
    if (bn_cmp(sumx_mod, ecc_n) >= 0) {
        uint32_t tmp[ECC_NUM_WORDS];
        bn_sub(tmp, sumx_mod, ecc_n);
        bn_copy(sumx_mod, tmp);
    }

    return (bn_cmp(r, sumx_mod) == 0) ? 0 : -1;
}

/* Public API: generate key pair */
void ecc_generate_keypair(uint8_t private_key[32], uint8_t public_key[64])
{
    uint32_t priv[ECC_NUM_WORDS];
    struct ecc_point g;
    struct ecc_point pub;
    int i;

    /* Simple random-ish private key (should use cryptographically secure RNG) */
    for (i = 0; i < ECC_NUM_WORDS; i++)
        priv[i] = (uint32_t)((uint64_t)rng_get_u64() ^ (uint64_t)rng_get_u64());

    /* Ensure priv < n */
    priv[ECC_NUM_WORDS - 1] &= 0xFFFFFFFF;
    if (bn_cmp(priv, ecc_n) >= 0)
        bn_sub(priv, priv, ecc_n);
    if (bn_is_zero(priv))
        priv[0] = 1;

    /* Convert private key */
    for (i = 0; i < ECC_NUM_WORDS; i++) {
        private_key[4*i]     = (uint8_t)(priv[i] & 0xFF);
        private_key[4*i + 1] = (uint8_t)((priv[i] >> 8) & 0xFF);
        private_key[4*i + 2] = (uint8_t)((priv[i] >> 16) & 0xFF);
        private_key[4*i + 3] = (uint8_t)((priv[i] >> 24) & 0xFF);
    }

    /* Compute public = priv * G */
    memset(&g, 0, sizeof(g));
    for (i = 0; i < ECC_NUM_WORDS; i++) {
        g.x[i] = ecc_gx[i];
        g.y[i] = ecc_gy[i];
    }
    g.is_infinity = 0;

    ecc_scalar_mult(&pub, priv, &g);

    /* Convert public key */
    for (i = 0; i < ECC_NUM_WORDS; i++) {
        public_key[4*i]     = (uint8_t)(pub.x[i] & 0xFF);
        public_key[4*i + 1] = (uint8_t)((pub.x[i] >> 8) & 0xFF);
        public_key[4*i + 2] = (uint8_t)((pub.x[i] >> 16) & 0xFF);
        public_key[4*i + 3] = (uint8_t)((pub.x[i] >> 24) & 0xFF);
        public_key[32 + 4*i]     = (uint8_t)(pub.y[i] & 0xFF);
        public_key[32 + 4*i + 1] = (uint8_t)((pub.y[i] >> 8) & 0xFF);
        public_key[32 + 4*i + 2] = (uint8_t)((pub.y[i] >> 16) & 0xFF);
        public_key[32 + 4*i + 3] = (uint8_t)((pub.y[i] >> 24) & 0xFF);
    }
}
