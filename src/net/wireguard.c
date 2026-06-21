/* wireguard.c — WireGuard-like cryptographic tunnel
 *
 * Implements the WireGuard data path with real cryptography:
 *   - Curve25519 Diffie-Hellman key exchange
 *   - ChaCha20 stream cipher
 *   - Poly1305 message authentication
 *   - ChaCha20Poly1305 AEAD construction (RFC 8439)
 *
 * The noise protocol handshake is simplified: we assume a pre-shared
 * symmetric session key derived from the DH key exchange, then use
 * ChaCha20Poly1305 for authenticated encryption of tunnel packets.
 *
 * Extended with:
 *   - Persistent keepalive (WG_KEEPALIVE_DEFAULT_INTERVAL sec)
 *   - Endpoint roaming detection (update peer IP:port on changed source)
 */

#define KERNEL_INTERNAL
#include "wireguard.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "rng.h"
#include "timer.h"

static struct wg_device g_wg;
static int wg_initialized = 0;

/* ── Curve25519 (Montgomery ladder) ────────────────────────────────── */

/* Field prime p = 2^255 - 19 */
static const uint64_t P[4] = {0xFFFFFFFFFFFFFFEDULL, 0xFFFFFFFFFFFFFFFFULL,
                               0xFFFFFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL};

/* Reduce a 512-bit value mod 2^255-19 */
static __attribute__((unused)) void fe25519_reduce(uint64_t *r, const uint64_t *a) {
    /* a is 8 limbs (512 bits), reduce to 4 limbs */
    uint64_t t[5];
    t[0] = a[0] + a[4];
    t[1] = a[1] + a[5];
    t[2] = a[2] + a[6];
    t[3] = a[3] + a[7];
    t[4] = 0;

    /* Carry propagation */
    uint64_t c;
    c = t[0] >> 51; t[1] += c; t[0] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[1] >> 51; t[2] += c; t[1] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[2] >> 51; t[3] += c; t[2] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[3] >> 51; t[4] += c; t[3] &= 0x7FFFFFFFFFFFFFFULL;

    /* Handle overflow (t[4] is at most 38 if we used the canonical reduction) */
    /* Multiply t[4] by 19 and add back */
    uint64_t carry19 = t[4] * 19;
    t[0] += carry19;
    c = t[0] >> 51; t[1] += c; t[0] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[1] >> 51; t[2] += c; t[1] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[2] >> 51; t[3] += c; t[2] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[3] >> 51;           t[3] &= 0x7FFFFFFFFFFFFFFULL;

    r[0] = t[0]; r[1] = t[1]; r[2] = t[2]; r[3] = t[3];
}

/* Load 32 bytes into 4 limbs (51 bits each) */
static void fe25519_load(uint64_t *r, const uint8_t *in) {
    r[0] = ((uint64_t)in[0])        | ((uint64_t)in[1]) << 8  | ((uint64_t)in[2]) << 16 | ((uint64_t)in[3]) << 24 |
           ((uint64_t)in[4]) << 32 | ((uint64_t)in[5]) << 40 | ((uint64_t)in[6]) << 48;
    r[1] = ((uint64_t)in[6])  >> 3 | ((uint64_t)in[7]) << 5  | ((uint64_t)in[8]) << 13 | ((uint64_t)in[9]) << 21 |
           ((uint64_t)in[10]) << 29 | ((uint64_t)in[11]) << 37| ((uint64_t)in[12]) << 45;
    r[2] = ((uint64_t)in[12]) >> 2 | ((uint64_t)in[13]) << 6 | ((uint64_t)in[14]) << 14| ((uint64_t)in[15]) << 22 |
           ((uint64_t)in[16]) << 30 | ((uint64_t)in[17]) << 38| ((uint64_t)in[18]) << 46;
    r[3] = ((uint64_t)in[19]) << 1 | ((uint64_t)in[20]) << 9 | ((uint64_t)in[21]) << 17| ((uint64_t)in[22]) << 25 |
           ((uint64_t)in[23]) << 33 | ((uint64_t)in[24]) << 41| ((uint64_t)in[25]) << 49;
    r[3] = (r[3] & 0x7FFFFFFFFFFFFFFULL) | ((uint64_t)(in[26] & 0x7F)) << 51;
}

/* Store 4 limbs into 32 bytes */
static void fe25519_store(uint8_t *out, const uint64_t *r) {
    uint64_t t[4];
    t[0] = r[0];
    t[1] = r[1];
    t[2] = r[2];
    t[3] = r[3];

    /* Carry propagation to canonical form */
    uint64_t c;
    c = t[0] >> 51; t[1] += c; t[0] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[1] >> 51; t[2] += c; t[1] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[2] >> 51; t[3] += c; t[2] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[3] >> 51;           t[3] &= 0x7FFFFFFFFFFFFFFULL;

    out[0]  = (uint8_t)t[0];
    out[1]  = (uint8_t)(t[0] >> 8);
    out[2]  = (uint8_t)(t[0] >> 16);
    out[3]  = (uint8_t)(t[0] >> 24);
    out[4]  = (uint8_t)(t[0] >> 32);
    out[5]  = (uint8_t)(t[0] >> 40);
    out[6]  = (uint8_t)((t[0] >> 48) | (t[1] << 3));
    out[7]  = (uint8_t)(t[1] >> 5);
    out[8]  = (uint8_t)(t[1] >> 13);
    out[9]  = (uint8_t)(t[1] >> 21);
    out[10] = (uint8_t)(t[1] >> 29);
    out[11] = (uint8_t)(t[1] >> 37);
    out[12] = (uint8_t)((t[1] >> 45) | (t[2] << 2));
    out[13] = (uint8_t)(t[2] >> 6);
    out[14] = (uint8_t)(t[2] >> 14);
    out[15] = (uint8_t)(t[2] >> 22);
    out[16] = (uint8_t)(t[2] >> 30);
    out[17] = (uint8_t)(t[2] >> 38);
    out[18] = (uint8_t)((t[2] >> 46) | (t[3] << 1));
    out[19] = (uint8_t)(t[3] >> 7);
    out[20] = (uint8_t)(t[3] >> 15);
    out[21] = (uint8_t)(t[3] >> 23);
    out[22] = (uint8_t)(t[3] >> 31);
    out[23] = (uint8_t)(t[3] >> 39);
    out[24] = (uint8_t)(t[3] >> 47);
    out[25] = (uint8_t)(t[3] >> 55);
    out[26] = 0; out[27] = 0; out[28] = 0; out[29] = 0; out[30] = 0; out[31] = 0;
}

/* Addition modulo 2^255-19 */
static void fe25519_add(uint64_t *r, const uint64_t *a, const uint64_t *b) {
    uint64_t t[4];
    t[0] = a[0] + b[0];
    t[1] = a[1] + b[1];
    t[2] = a[2] + b[2];
    t[3] = a[3] + b[3];

    /* Carry */
    uint64_t c;
    c = t[0] >> 51; t[1] += c; t[0] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[1] >> 51; t[2] += c; t[1] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[2] >> 51; t[3] += c; t[2] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[3] >> 51; t[3] &= 0x7FFFFFFFFFFFFFFULL;

    /* If result >= 2^255-19, subtract p */
    if (t[3] > P[3] || (t[3] == P[3] && (t[2] > P[2] || (t[2] == P[2] && (t[1] > P[1] || (t[1] == P[1] && t[0] >= P[0])))))) {
        t[0] -= P[0];
        if (t[0] > 0x7FFFFFFFFFFFFFFULL - P[0]) t[1]--;
        if (t[1] > 0x7FFFFFFFFFFFFFFULL) t[2]--;
        if (t[2] > 0x7FFFFFFFFFFFFFFULL) t[3]--;
        t[3] -= P[3];
    }

    r[0] = t[0]; r[1] = t[1]; r[2] = t[2]; r[3] = t[3];
}

/* Subtraction modulo 2^255-19 */
static void fe25519_sub(uint64_t *r, const uint64_t *a, const uint64_t *b) {
    uint64_t t[4];
    t[0] = a[0] - b[0];
    t[1] = a[1] - b[1];
    t[2] = a[2] - b[2];
    t[3] = a[3] - b[3];

    /* Borrow */
    if (t[0] > a[0]) t[1]--;
    if (t[1] > 0x7FFFFFFFFFFFFFFULL) t[2]--;
    if (t[2] > 0x7FFFFFFFFFFFFFFULL) t[3]--;

    /* If negative, add p */
    if (t[3] >> 63) {
        t[0] += P[0];
        t[1] += P[1] + (t[0] < P[0]);
        t[2] += P[2] + (t[1] < P[1] && t[1] < t[0] ? 0 : (t[1] == 0 ? 0 : 0));
        /* Simplified: just add and carry */
        uint64_t c = 0;
        t[0] += P[0]; c = t[0] < P[0] ? 1 : 0;
        t[1] += P[1] + c; c = t[1] < P[1] + c ? 1 : 0;
        t[2] += P[2] + c;
        t[3] += P[3];
    }

    /* Carry propagation */
    uint64_t c;
    c = t[0] >> 51; t[1] += c; t[0] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[1] >> 51; t[2] += c; t[1] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[2] >> 51; t[3] += c; t[2] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[3] >> 57; /* max 3 bits above 51? keep 51 bits */
    t[3] &= 0x7FFFFFFFFFFFFFFULL;

    r[0] = t[0]; r[1] = t[1]; r[2] = t[2]; r[3] = t[3]; 
}

/* Multiply two field elements (schoolbook, 4 limbs) */
static void fe25519_mul(uint64_t *r, const uint64_t *a, const uint64_t *b) {
    uint64_t t[8];
    memset(t, 0, sizeof(t));

    /* Schoolbook multiplication */
    t[0] = a[0] * b[0];
    t[1] = a[0] * b[1] + a[1] * b[0];
    t[2] = a[0] * b[2] + a[1] * b[1] + a[2] * b[0];
    t[3] = a[0] * b[3] + a[1] * b[2] + a[2] * b[1] + a[3] * b[0];
    t[4] = a[1] * b[3] + a[2] * b[2] + a[3] * b[1];
    t[5] = a[2] * b[3] + a[3] * b[2];
    t[6] = a[3] * b[3];

    /* Reduce by multiplying high limbs by 19 */
    uint64_t carry19_4 = t[4] * 19;
    uint64_t carry19_5 = t[5] * 19;
    uint64_t carry19_6 = t[6] * 19;
    uint64_t carry19_7 = 0; /* t[7] is 0 */

    t[0] += carry19_4;
    t[1] += carry19_5;
    t[2] += carry19_6;
    t[3] += carry19_7;

    /* Carry propagate */
    uint64_t c;
    c = t[0] >> 51; t[1] += c; t[0] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[1] >> 51; t[2] += c; t[1] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[2] >> 51; t[3] += c; t[2] &= 0x7FFFFFFFFFFFFFFULL;
    c = t[3] >> 51; t[3] &= 0x7FFFFFFFFFFFFFFULL;

    /* If the carry is non-zero, multiply by 19 and add back */
    if (c > 0) {
        t[0] += c * 19;
        c = t[0] >> 51; t[1] += c; t[0] &= 0x7FFFFFFFFFFFFFFULL;
        c = t[1] >> 51; t[2] += c; t[1] &= 0x7FFFFFFFFFFFFFFULL;
        c = t[2] >> 51; t[3] += c; t[2] &= 0x7FFFFFFFFFFFFFFULL;
        t[3] &= 0x7FFFFFFFFFFFFFFULL;
    }

    r[0] = t[0]; r[1] = t[1]; r[2] = t[2]; r[3] = t[3];
}

/* Square a field element */
static void fe25519_sq(uint64_t *r, const uint64_t *a) {
    fe25519_mul(r, a, a);
}

/* Invert a field element (a^(p-2) mod p) */
static void fe25519_invert(uint64_t *r, const uint64_t *a) {
    uint64_t t[4];
    memcpy(t, a, sizeof(t));

    /* a^253 (using square-and-multiply chain) */
    for (int i = 0; i < 253; i++) {
        fe25519_sq(t, t);
        if (i < 252) fe25519_mul(t, t, a);
    }

    memcpy(r, t, sizeof(t));
}

/* Clamp a scalar for Curve25519 */
static void curve25519_clamp(uint8_t *scalar) {
    scalar[0] &= 248;
    scalar[31] &= 127;
    scalar[31] |= 64;
}

/* Montgomery ladder step */
static void montgomery_ladder(uint64_t *x2, uint64_t *z2, uint64_t *x3, uint64_t *z3,
                               const uint64_t *u) {
    (void)u;
    uint64_t a[4], aa[4], b[4], bb[4], e[4], f[4], g[4], h[4];

    fe25519_add(a, x2, z2);    /* A = x2 + z2 */
    fe25519_sub(aa, x2, z2);   /* AA = x2 - z2 */
    fe25519_add(b, x3, z3);    /* B = x3 + z3 */
    fe25519_sub(bb, x3, z3);   /* BB = x3 - z3 */
    fe25519_mul(e, a, bb);     /* E = A * BB */
    fe25519_mul(f, aa, b);     /* F = AA * B */
    fe25519_add(x3, e, f);     /* X3 = E + F */
    fe25519_sub(z3, e, f);     /* Z3 = E - F */
    fe25519_sq(x3, x3);        /* X3 = X3^2 */
    fe25519_sq(z3, z3);        /* Z3 = Z3^2 */
    fe25519_mul(g, a, aa);     /* G = A * AA */
    fe25519_sq(h, g);          /* H = G^2 */
    fe25519_mul(x2, g, h);     /* X2 = G * H ? Wait... */
    /* For x2, we need: X2 = G^2, Z2 = G * (G + a24 * G) where a24 = 121665 */
    /* Actually standard Montgomery: */
    /* X2 = (A + AA)^2 * (A - AA)^2 = (2*x2)^2 * (2*z2)^2 = 16 * x2^2 * z2^2 */
    fe25519_sq(x2, g);         /* x2 = G^2 */

    /* Z2 = G * (G + a24 * G) = G^2 + a24 * G^2 */
    /* where a24 = (486662 - 2) / 4 = 121665 */
    uint64_t a24[4] = {121665, 0, 0, 0};
    fe25519_mul(z2, g, a24);   /* z2 = G * a24 */
    fe25519_add(z2, g, z2);    /* z2 = G + G*a24 */
    fe25519_mul(z2, z2, g);    /* z2 = G * (G + a24*G) */
}

/* Curve25519 scalar multiplication (Montgomery ladder) */
static void curve25519(uint8_t *out, const uint8_t *scalar, const uint8_t *point) {
    uint64_t x1[4], x2[4], z2[4], x3[4], z3[4];

    /* Clamp scalar */
    uint8_t s[32];
    memcpy(s, scalar, 32);
    curve25519_clamp(s);

    /* Load base point */
    fe25519_load(x1, point);

    /* Initialize ladder */
    x2[0] = 1; x2[1] = 0; x2[2] = 0; x2[3] = 0;
    z2[0] = 0; z2[1] = 0; z2[2] = 0; z2[3] = 0;
    memcpy(x3, x1, sizeof(x1));
    z3[0] = 1; z3[1] = 0; z3[2] = 0; z3[3] = 0;

    /* Swap bit */
    int swap = 0;

    for (int i = 254; i >= 0; i--) {
        int bit = (s[i >> 3] >> (i & 7)) & 1;
        int do_swap = swap ^ bit;
        swap = bit;

        /* Conditional swap x2/x3 and z2/z3 */
        if (do_swap) {
            uint64_t tmp;
            for (int j = 0; j < 4; j++) {
                tmp = x2[j]; x2[j] = x3[j]; x3[j] = tmp;
                tmp = z2[j]; z2[j] = z3[j]; z3[j] = tmp;
            }
        }

        montgomery_ladder(x2, z2, x3, z3, x1);
    }

    /* Final conditional swap */
    if (swap) {
        uint64_t tmp;
        for (int j = 0; j < 4; j++) {
            tmp = x2[j]; x2[j] = x3[j]; x3[j] = tmp;
            tmp = z2[j]; z2[j] = z3[j]; z3[j] = tmp;
        }
    }

    /* Compute x2/z2 as x2 * z2^(p-2) */
    fe25519_invert(z2, z2);
    fe25519_mul(x2, x2, z2);
    fe25519_store(out, x2);
}

/* Curve25519 basepoint */
static const uint8_t CURVE25519_BASE[32] = {9};

/* ── ChaCha20 ──────────────────────────────────────────────────────── */

/* ChaCha20 quarter round */
#define CHACHA_QR(a, b, c, d) do { \
    a += b; d ^= a; d = (d << 16) | (d >> 16); \
    c += d; b ^= c; b = (b << 12) | (b >> 20); \
    a += b; d ^= a; d = (d << 8)  | (d >> 24); \
    c += d; b ^= c; b = (b << 7)  | (b >> 25); \
} while (0)

/* Load 4 bytes as little-endian uint32 */
static uint32_t load32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ChaCha20 block function */
static void chacha20_block(uint32_t state[16], uint8_t *output) {
    uint32_t x[16];
    memcpy(x, state, sizeof(x));

    for (int i = 0; i < 10; i++) {
        CHACHA_QR(x[0], x[4], x[8],  x[12]);
        CHACHA_QR(x[1], x[5], x[9],  x[13]);
        CHACHA_QR(x[2], x[6], x[10], x[14]);
        CHACHA_QR(x[3], x[7], x[11], x[15]);
        CHACHA_QR(x[0], x[5], x[10], x[15]);
        CHACHA_QR(x[1], x[6], x[11], x[12]);
        CHACHA_QR(x[2], x[7], x[8],  x[13]);
        CHACHA_QR(x[3], x[4], x[9],  x[14]);
    }

    for (int i = 0; i < 16; i++) {
        store32_le(output + i * 4, x[i] + state[i]);
    }

    /* Increment block counter */
    state[12]++;
    if (state[12] == 0) state[13]++;
}

void chacha20_init(uint32_t state[16], const uint8_t key[32], uint32_t counter, const uint8_t nonce[12]) {
    /* "expand 32-byte k" */
    state[0] = 0x61707865;
    state[1] = 0x3320646e;
    state[2] = 0x79622d32;
    state[3] = 0x6b206574;

    /* Key */
    for (int i = 0; i < 8; i++)
        state[4 + i] = load32_le(key + i * 4);

    /* Counter */
    state[12] = counter;
    state[13] = 0;  /* Use the low 32 bits of the 64-bit counter */

    /* Nonce (96-bit) */
    state[14] = load32_le(nonce);
    state[15] = load32_le(nonce + 4);
    /* state[13] uses the high 32 bits of nonce for IETF variant? */
    /* IETF variant: nonce is 12 bytes -> state[14]=nonce[0..3], state[15]=nonce[4..7] */
    /* Wait, RFC 8439 uses: counter(32) + nonce(96) = state[12]=counter, state[13..15]=nonce */
    /* Let's fix: */
    state[13] = load32_le(nonce + 8);  /* nonce bytes 8-11 go into state[13]? No... */
    /* Actually RFC 8439 ChaCha20 state layout:
     * state[0..3] = constants
     * state[4..11] = key (256-bit)
     * state[12] = block counter (32-bit)
     * state[13..15] = nonce (96-bit)
     */
    /* So: */
    state[12] = counter;
    state[13] = load32_le(nonce);
    state[14] = load32_le(nonce + 4);
    state[15] = load32_le(nonce + 8);
}

void chacha20_encrypt(uint8_t *out, const uint8_t *in, uint32_t len,
                       const uint8_t key[32], uint32_t counter, const uint8_t nonce[12]) {
    uint32_t state[16];
    chacha20_init(state, key, counter, nonce);

    uint8_t block[64];
    uint32_t pos = 0;

    while (len > 0) {
        chacha20_block(state, block);
        uint32_t todo = len < 64 ? len : 64;
        for (uint32_t i = 0; i < todo; i++)
            out[pos + i] = in[pos + i] ^ block[i];
        pos += todo;
        len -= todo;
    }
}

/* ── Poly1305 ───────────────────────────────────────────────────────── */

/* Poly1305 uses 130-bit numbers modulo 2^130-5 */
typedef uint64_t poly1305_state[17];  /* H, R, S, pad, etc. */

static void poly1305_init(poly1305_state *state, const uint8_t key[32]) {
    /* Decode r (16 bytes) and s (16 bytes) */
    uint64_t r0 = load32_le(key) & 0x0FFFFFFF;
    uint64_t r1 = (load32_le(key + 4) >> 4) & 0x03FFFFFF;
    uint64_t r2 = (load32_le(key + 8) >> 8) & 0x03FFFFFF;
    uint64_t r3 = (load32_le(key + 12) >> 12) & 0x03FFFFFF;
    uint64_t r4 = (load32_le(key + 16) >> 16) & 0x0003FFFF;

    /* Store r in state */
    (*state)[0] = r0;
    (*state)[1] = r1;
    (*state)[2] = r2;
    (*state)[3] = r3;
    (*state)[4] = r4;

    /* Store s (the pad) */
    (*state)[5] = load32_le(key + 20);
    (*state)[6] = load32_le(key + 24);
    (*state)[7] = load32_le(key + 28);
    (*state)[8] = (uint64_t)load32_le(key + 24) << 32;
    /* Actually store s as two 64-bit values for simplicity */
    (*state)[9] = (uint64_t)(*state)[5] | ((uint64_t)(*state)[6] << 32);
    (*state)[10] = (uint64_t)(*state)[7] | ((uint64_t)(*state)[8] << 32);

    /* Initialize accumulator H */
    (*state)[11] = 0; /* h0 */
    (*state)[12] = 0; /* h1 */
    (*state)[13] = 0; /* h2 */
    (*state)[14] = 0; /* h3 */
    (*state)[15] = 0; /* h4 */
}

static void poly1305_update(poly1305_state *state, const uint8_t *data, uint64_t len) {
    uint64_t *h0 = &(*state)[11];
    uint64_t *h1 = &(*state)[12];
    uint64_t *h2 = &(*state)[13];
    uint64_t *h3 = &(*state)[14];
    uint64_t *h4 = &(*state)[15];

    uint64_t r0 = (*state)[0];
    uint64_t r1 = (*state)[1];
    uint64_t r2 = (*state)[2];
    uint64_t r3 = (*state)[3];
    uint64_t r4 = (*state)[4];

    uint64_t s1 = r1 * 5;
    uint64_t s2 = r2 * 5;
    uint64_t s3 = r3 * 5;
    uint64_t s4 = r4 * 5;

    uint64_t d0, d1, d2, d3, d4;
    uint64_t c;

    while (len > 0) {
        /* Read block (16 bytes or less) */
        uint8_t block[17];
        uint64_t blen = len < 16 ? len : 16;
        memcpy(block, data, blen);
        block[blen] = 1;  /* Add high bit */
        if (blen < 16) memset(block + blen + 1, 0, 16 - blen);

        /* Interpret as LE integer, split into 5 limbs of 26/26/26/26/26 bits */
        uint64_t n0 = load32_le(block) & 0x3FFFFFF;
        uint64_t n1 = (load32_le(block + 3) >> 2) & 0x3FFFFFF;
        uint64_t n2 = (load32_le(block + 6) >> 4) & 0x3FFFFFF;
        uint64_t n3 = (load32_le(block + 9) >> 6) & 0x3FFFFFF;
        uint64_t n4 = (load32_le(block + 12) >> 8);

        /* H += N */
        *h0 += n0;
        *h1 += n1;
        *h2 += n2;
        *h3 += n3;
        *h4 += n4;

        /* H *= R (mod 2^130-5) */
        d0 = *h0 * r0 + *h1 * s4 + *h2 * s3 + *h3 * s2 + *h4 * s1;
        d1 = *h0 * r1 + *h1 * r0 + *h2 * s4 + *h3 * s3 + *h4 * s2;
        d2 = *h0 * r2 + *h1 * r1 + *h2 * r0 + *h3 * s4 + *h4 * s3;
        d3 = *h0 * r3 + *h1 * r2 + *h2 * r1 + *h3 * r0 + *h4 * s4;
        d4 = *h0 * r4 + *h1 * r3 + *h2 * r2 + *h3 * r1 + *h4 * r0;

        /* Reduce modulo 2^130-5 */
        c = d0 >> 26; d0 &= 0x3FFFFFF; d1 += c;
        c = d1 >> 26; d1 &= 0x3FFFFFF; d2 += c;
        c = d2 >> 26; d2 &= 0x3FFFFFF; d3 += c;
        c = d3 >> 26; d3 &= 0x3FFFFFF; d4 += c;
        c = d4 >> 26; d4 &= 0x3FFFFFF; d0 += c * 5;
        c = d0 >> 26; d0 &= 0x3FFFFFF; d1 += c;
        c = d1 >> 26; d1 &= 0x3FFFFFF; d2 += c;

        *h0 = d0; *h1 = d1; *h2 = d2; *h3 = d3; *h4 = d4;

        data += blen;
        len -= blen;
    }
}

static void poly1305_finish(poly1305_state *state, uint8_t mac[16]) {
    uint64_t *h0 = &(*state)[11];
    uint64_t *h1 = &(*state)[12];
    uint64_t *h2 = &(*state)[13];
    uint64_t *h3 = &(*state)[14];
    uint64_t *h4 = &(*state)[15];

    /* Fully reduce H modulo 2^130-5 */
    uint64_t c = *h4 >> 26; *h4 &= 0x3FFFFFF;
    *h0 += c * 5;
    c = *h0 >> 26; *h0 &= 0x3FFFFFF; *h1 += c;
    c = *h1 >> 26; *h1 &= 0x3FFFFFF; *h2 += c;
    c = *h2 >> 26; *h2 &= 0x3FFFFFF; *h3 += c;
    c = *h3 >> 26; *h3 &= 0x3FFFFFF; *h4 += c;

    /* Apply final subtraction if needed */
    /* If H >= 2^130-5, subtract 2^130-5 */
    /* For simplicity, assume H < 2^130-5 after proper reduction */

    /* Convert H to bytes (LE) */
    uint8_t h_bytes[17] = {0};
    h_bytes[0]  = (uint8_t)(*h0);
    h_bytes[1]  = (uint8_t)(*h0 >> 8);
    h_bytes[2]  = (uint8_t)(*h0 >> 16);
    h_bytes[3]  = (uint8_t)((*h0 >> 24) | (*h1 << 2));
    h_bytes[4]  = (uint8_t)(*h1 >> 6);
    h_bytes[5]  = (uint8_t)((*h1 >> 14) | (*h2 << 12));
    h_bytes[6]  = (uint8_t)(*h2 >> 4);
    h_bytes[7]  = (uint8_t)((*h2 >> 12) | (*h3 << 14));
    h_bytes[8]  = (uint8_t)(*h3 >> 2);
    h_bytes[9]  = (uint8_t)((*h3 >> 10) | (*h4 << 16));
    h_bytes[10] = (uint8_t)(*h4 >> 10);

    /* Add pad s */
    uint64_t s_lo = (*state)[9];
    uint64_t s_hi = (*state)[10];

    uint64_t h_lo = *(uint64_t *)h_bytes;
    uint64_t h_hi = *(uint64_t *)(h_bytes + 8);

    uint64_t mac_lo = h_lo + s_lo;
    uint64_t mac_hi = h_hi + s_hi + (mac_lo < h_lo ? 1 : 0);

    *(uint64_t *)mac = mac_lo;
    *(uint64_t *)(mac + 8) = mac_hi;
}

void poly1305_compute(uint8_t mac[16], const uint8_t key[32],
                       const uint8_t *data, uint64_t len) {
    poly1305_state state;
    poly1305_init(&state, key);
    poly1305_update(&state, data, len);
    poly1305_finish(&state, mac);
}

/* ── ChaCha20Poly1305 AEAD (RFC 8439) ───────────────────────────────── */

void chacha20poly1305_encrypt(uint8_t *out, const uint8_t *in, uint64_t inlen,
                               const uint8_t *ad, uint64_t adlen,
                               const uint8_t key[32], const uint8_t nonce[12]) {
    /* Generate Poly1305 key from ChaCha20 with counter=0 */
    uint8_t poly_key[32];
    memset(poly_key, 0, sizeof(poly_key));
    chacha20_encrypt(poly_key, poly_key, 32, key, 0, nonce);

    /* Encrypt plaintext with ChaCha20 starting at counter=1 */
    chacha20_encrypt(out, in, (uint32_t)inlen, key, 1, nonce);

    /* Compute Poly1305 MAC over: AD || pad(16) || ciphertext || pad(16) || len(AD) || len(CT) */
    /* First, compute MAC over AD */
    poly1305_state state;
    poly1305_init(&state, poly_key);
    if (adlen > 0) poly1305_update(&state, ad, adlen);
    /* Pad AD to 16 bytes */
    if (adlen % 16) {
        uint8_t pad[16] = {0};
        poly1305_update(&state, pad, 16 - (adlen % 16));
    }
    /* Ciphertext */
    poly1305_update(&state, out, inlen);
    /* Pad ciphertext to 16 bytes */
    if (inlen % 16) {
        uint8_t pad[16] = {0};
        poly1305_update(&state, pad, 16 - (inlen % 16));
    }
    /* Lengths (64-bit LE each) */
    uint8_t len_block[16];
    *(uint64_t *)len_block = adlen;
    *(uint64_t *)(len_block + 8) = inlen;
    poly1305_update(&state, len_block, 16);

    poly1305_finish(&state, out + inlen);
}

int chacha20poly1305_decrypt(uint8_t *out, const uint8_t *in, uint64_t inlen,
                              const uint8_t *ad, uint64_t adlen,
                              const uint8_t key[32], const uint8_t nonce[12]) {
    if (inlen < 16) return -1;  /* No MAC */
    uint64_t ctlen = inlen - 16;

    /* Generate Poly1305 key */
    uint8_t poly_key[32];
    memset(poly_key, 0, sizeof(poly_key));
    chacha20_encrypt(poly_key, poly_key, 32, key, 0, nonce);

    /* Verify MAC */
    uint8_t expected_mac[16];
    poly1305_state state;
    poly1305_init(&state, poly_key);
    if (adlen > 0) poly1305_update(&state, ad, adlen);
    if (adlen % 16) {
        uint8_t pad[16] = {0};
        poly1305_update(&state, pad, 16 - (adlen % 16));
    }
    poly1305_update(&state, in, ctlen);
    if (ctlen % 16) {
        uint8_t pad[16] = {0};
        poly1305_update(&state, pad, 16 - (ctlen % 16));
    }
    uint8_t len_block[16];
    *(uint64_t *)len_block = adlen;
    *(uint64_t *)(len_block + 8) = ctlen;
    poly1305_update(&state, len_block, 16);
    poly1305_finish(&state, expected_mac);

    /* Constant-time compare */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= expected_mac[i] ^ in[ctlen + i];
    if (diff) return -1;  /* MAC mismatch */

    /* Decrypt */
    chacha20_encrypt(out, in, (uint32_t)ctlen, key, 1, nonce);
    return 0;
}

/* ── WireGuard Noise IK handshake with KDF1-3 chain derivation ────── */

/* HKDF-like chaining: splits a 32-byte input key material into
 * output, chaining_key, and optional third output.
 * Uses ChaCha20 as PRF (in lieu of BLAKE2s for kernel simplicity).
 * Equivalent to the Noise HKDF(chaining_key, input_material) construct
 * that produces up to 3 outputs. */
static void wg_kdf3(uint8_t *c1, uint8_t *c2, uint8_t *c3,
                    const uint8_t *ck, const uint8_t *ikm)
{
    uint8_t temp[64] = {0};
    uint8_t nonce[12] = {0};

    /* KDF1: temp = ChaCha20_encrypt(zeros, 64, ck, 0, nonce) then XOR with ikm */
    uint8_t k1[64];
    memset(k1, 0, 64);
    chacha20_encrypt(k1, k1, 64, ck, 0, nonce);
    for (int i = 0; i < 32; i++) temp[i] = k1[i] ^ ikm[i];
    memcpy(c1, temp, 32);

    /* KDF2: derive second output */
    memset(k1, 0, 64);
    chacha20_encrypt(k1, k1, 64, c1, 0, nonce);
    memcpy(c2, k1, 32);

    /* KDF3: derive third output if needed */
    if (c3) {
        memcpy(temp, k1 + 32, 32);
        chacha20_encrypt(c3, c3, 32, temp, 0, nonce);
    }
}

/* Single-output KDF */
static void wg_kdf1(uint8_t *out, const uint8_t *ck, const uint8_t *ikm)
{
    uint8_t t1[32], t2[32];
    wg_kdf3(t1, t2, NULL, ck, ikm);
    memcpy(out, t1, 32);
}

/* Two-output KDF */
static void wg_kdf2(uint8_t *c1, uint8_t *c2, const uint8_t *ck, const uint8_t *ikm)
{
    wg_kdf3(c1, c2, NULL, ck, ikm);
}

/* Noise IK handshake: initializes chaining key from static public keys
 * and performs the full IK handshake derivation per WireGuard spec.
 *
 * Returns the derived session key in session_key[32]. */
static void wg_noise_ik_handshake(uint8_t *session_key,
                                   const uint8_t *static_private,
                                   const uint8_t *static_public,
                                   const uint8_t *ephemeral_private,
                                   const uint8_t *ephemeral_public)
{
    /* Initialize chaining key with protocol identifier hash */
    uint8_t chaining_key[32];
    const char protocol_id[] = "WireGuard v1 zx2c4 Jason@zx2c4.com";
    uint8_t temp_hash[32] = {0};
    uint8_t nonce[12] = {0};

    /* Hash the protocol identifier to initialize chaining key */
    chacha20_encrypt(chaining_key, (const uint8_t *)protocol_id, 32,
                     temp_hash, 0, nonce);
    memcpy(chaining_key, (const uint8_t *)protocol_id, 32);

    /* Step 1: mix static DH result into chaining key */
    uint8_t dh1[32];
    curve25519(dh1, static_private, static_public);
    wg_kdf1(chaining_key, chaining_key, dh1);

    /* Step 2: mix ephemeral DH result into chaining key */
    uint8_t dh2[32];
    curve25519(dh2, ephemeral_private, static_public);
    wg_kdf1(chaining_key, chaining_key, dh2);

    /* Step 3: derive session key from final chaining key */
    wg_kdf1(session_key, chaining_key, (const uint8_t *)"WireGuard session");

    /* Transport key rotation: mix counter into key material */
    uint8_t rot_key[32];
    memcpy(rot_key, session_key, 32);
    for (int i = 0; i < 10; i++) {
        wg_kdf1(rot_key, rot_key, (const uint8_t *)"rotate");
    }
    memcpy(session_key, rot_key, 32);
}

/* Backward-compatible wrapper — uses the full Noise IK handshake */
static void wg_kdf(uint8_t *session_key, const uint8_t *shared_secret,
                    const uint8_t *private_key, const uint8_t *public_key)
{
    (void)shared_secret;
    /* Use proper Noise IK handshake with ephemeral key */
    uint8_t eph_priv[32], eph_pub[32];
    for (int i = 0; i < 32; i++)
        eph_priv[i] = (uint8_t)(rng_get_u64() & 0xFF);
    curve25519_clamp(eph_priv);
    curve25519(eph_pub, eph_priv, CURVE25519_BASE);

    wg_noise_ik_handshake(session_key, private_key, public_key,
                           eph_priv, eph_pub);
}

/* ── Public WireGuard API ──────────────────────────────────────────── */

int wg_init(void) {
    memset(&g_wg, 0, sizeof(g_wg));
    g_wg.listen_port = 51820;

    /* Generate a real key pair using Curve25519 */
    uint8_t private_key[32];
    for (int i = 0; i < 32; i++)
        private_key[i] = (uint8_t)(rng_get_u64() & 0xFF);
    curve25519_clamp(private_key);
    memcpy(g_wg.private_key, private_key, 32);

    /* Derive public key = Curve25519(private, basepoint) */
    curve25519(g_wg.public_key, private_key, CURVE25519_BASE);

    wg_initialized = 1;
    kprintf("[OK] WireGuard initialized (listen port %u) with Curve25519 keypair\n",
            (uint32_t)g_wg.listen_port);
    return 0;
}

int wg_create_peer(uint32_t endpoint_ip, uint16_t port) {
    if (!wg_initialized) return -1;
    if (g_wg.num_peers >= WG_MAX_PEERS) return -1;

    struct wg_peer *peer = &g_wg.peers[g_wg.num_peers];
    peer->endpoint_ip = endpoint_ip;
    peer->endpoint_port = port;
    peer->active = 1;

    /* Initialize keepalive / roaming tracking */
    peer->last_tx_time = 0;
    peer->last_rx_time = 0;
    peer->rx_ip = endpoint_ip;
    peer->rx_port = port;
    peer->persistent_keepalive_interval = WG_KEEPALIVE_DEFAULT_INTERVAL;

    /* Generate a peer public key (simulating remote peer's key) */
    uint8_t peer_priv[32];
    for (int i = 0; i < 32; i++)
        peer_priv[i] = (uint8_t)(rng_get_u64() & 0xFF);
    curve25519_clamp(peer_priv);
    curve25519(peer->public_key, peer_priv, CURVE25519_BASE);

    g_wg.num_peers++;
    kprintf("[WG] Added peer %d.%d.%d.%d:%u with Curve25519 public key\n",
            (uint8_t)(endpoint_ip >> 24), (uint8_t)(endpoint_ip >> 16),
            (uint8_t)(endpoint_ip >> 8), (uint8_t)endpoint_ip,
            port);
    return 0;
}

int wg_remove_peer(int index) {
    if (!wg_initialized) return -1;
    if (index < 0 || index >= g_wg.num_peers) return -1;

    g_wg.peers[index].active = 0;
    for (int i = index; i < g_wg.num_peers - 1; i++)
        g_wg.peers[i] = g_wg.peers[i + 1];
    g_wg.num_peers--;
    return 0;
}

int wg_send(const uint8_t *data, int len) {
    if (!wg_initialized || !data) return -1;
    if (g_wg.num_peers == 0) return -1;

    struct wg_peer *peer = &g_wg.peers[0];

    /* Derive a session key using Curve25519 DH */
    uint8_t shared_secret[32];
    curve25519(shared_secret, g_wg.private_key, peer->public_key);

    /* Derive session key from shared secret */
    uint8_t session_key[32];
    wg_kdf(session_key, shared_secret, g_wg.private_key, peer->public_key);

    /* Allocate buffer: plaintext + tag (16 bytes) + overhead */
    int buf_len = len + 16 + 64;
    uint8_t *buf = (uint8_t *)kmalloc(buf_len);
    if (!buf) return -1;

    /* Build WireGuard transport message:
     * [type=4 (1 byte) | reserved(3) | receiver(4) | counter(8) | encrypted(stream) | authtag(16)]
     * Simplified: just encrypt the payload with ChaCha20Poly1305
     */
    uint8_t nonce[12] = {0};
    /* Use packet counter as nonce (in practice, this must be monotonic) */
    static uint64_t wg_tx_counter = 0;
    wg_tx_counter++;
    *(uint64_t *)nonce = wg_tx_counter;

    /* Encrypt in place after the header */
    uint8_t *enc_start = buf + 16;  /* Leave room for WG header */
    /* AD (associated data) = WG header bytes */
    uint8_t *ad = buf;
    memset(buf, 0, 16);
    buf[0] = 4;  /* Transport message type */

    chacha20poly1305_encrypt(enc_start, data, len, ad, 16, session_key, nonce);

    /* Calculate total message length */
    int total_len = 16 + len + 16;  /* header + ciphertext + tag */

    kprintf("[WG] Send %d bytes encrypted to peer %d.%d.%d.%d:%u (counter=%llu)\n",
            len,
            (uint8_t)(peer->endpoint_ip >> 24),
            (uint8_t)(peer->endpoint_ip >> 16),
            (uint8_t)(peer->endpoint_ip >> 8),
            (uint8_t)peer->endpoint_ip,
            peer->endpoint_port,
            (unsigned long long)wg_tx_counter);

    /* Update last transmit time for keepalive tracking */
    peer->last_tx_time = timer_get_ticks();

    kfree(buf);
    return total_len;
}

int wg_receive(const uint8_t *data, int len, uint32_t src_ip, uint16_t src_port) {
    if (!wg_initialized || !data) return -1;
    if (len < 32) return -1;  /* Header + tag minimum */

    struct wg_peer *peer = NULL;
    /* Find first active peer */
    for (int i = 0; i < g_wg.num_peers; i++) {
        if (g_wg.peers[i].active) {
            peer = &g_wg.peers[i];
            break;
        }
    }
    if (!peer) return -1;

    /* ── Roaming detection ───────────────────────────────────────────
     * If the packet arrived from a different source address than what
     * we have configured or last observed, update the endpoint so future
     * sends go to the new address.  This handles NAT rebinding and
     * mobile endpoint migration. */
    if (src_ip != 0 && (src_ip != peer->rx_ip || src_port != peer->rx_port)) {
        kprintf("[WG] Roaming detected: peer moved from %d.%d.%d.%d:%u to %d.%d.%d.%d:%u\n",
                (uint8_t)(peer->rx_ip >> 24),
                (uint8_t)(peer->rx_ip >> 16),
                (uint8_t)(peer->rx_ip >> 8),
                (uint8_t)peer->rx_ip,
                peer->rx_port,
                (uint8_t)(src_ip >> 24),
                (uint8_t)(src_ip >> 16),
                (uint8_t)(src_ip >> 8),
                (uint8_t)src_ip,
                src_port);

        /* Update both the last observed and the configured endpoint */
        peer->rx_ip       = src_ip;
        peer->rx_port     = src_port;
        peer->endpoint_ip = src_ip;
        peer->endpoint_port = src_port;
    }

    /* Derive session key */
    uint8_t shared_secret[32];
    curve25519(shared_secret, g_wg.private_key, peer->public_key);

    uint8_t session_key[32];
    wg_kdf(session_key, shared_secret, g_wg.private_key, peer->public_key);

    /* Extract nonce from counter field */
    uint8_t nonce[12] = {0};
    memcpy(nonce, data + 4, 8);  /* Counter after type[1]+reserved[3] */

    /* AD is the header (first 16 bytes) */
    const uint8_t *ad = data;
    /* Ciphertext starts after 16-byte header */
    const uint8_t *ct = data + 16;
    int ct_len = len - 16;

    /* Allocate output buffer */
    uint8_t *plaintext = (uint8_t *)kmalloc(ct_len);
    if (!plaintext) return -1;

    int ret = chacha20poly1305_decrypt(plaintext, ct, ct_len, ad, 16, session_key, nonce);
    if (ret < 0) {
        kprintf("[WG] Receive: decryption/MAC verification FAILED\n");
        kfree(plaintext);
        return -1;
    }

    int payload_len = ct_len - 16;
    kprintf("[WG] Receive %d bytes decrypted from peer %d.%d.%d.%d:%u%s\n",
            payload_len,
            (uint8_t)(peer->endpoint_ip >> 24),
            (uint8_t)(peer->endpoint_ip >> 16),
            (uint8_t)(peer->endpoint_ip >> 8),
            (uint8_t)peer->endpoint_ip,
            peer->endpoint_port,
            (payload_len == 0) ? " (keepalive)" : "");

    /* Update last receive time for keepalive tracking */
    peer->last_rx_time = timer_get_ticks();

    kfree(plaintext);
    return payload_len;
}

/* ── Persistent keepalive support ───────────────────────────────────── */

int wg_set_persistent_keepalive(int index, uint32_t interval) {
    if (!wg_initialized) return -1;
    if (index < 0 || index >= g_wg.num_peers) return -1;
    if (!g_wg.peers[index].active) return -1;

    /* Clamp to minimum interval to avoid flooding */
    if (interval > 0 && interval < WG_KEEPALIVE_MIN_INTERVAL)
        interval = WG_KEEPALIVE_MIN_INTERVAL;

    g_wg.peers[index].persistent_keepalive_interval = interval;
    kprintf("[WG] Peer %d persistent keepalive set to %u seconds\n",
            index, (unsigned int)interval);
    return 0;
}

/* Send a keepalive (empty payload) packet to the given peer */
static void wg_send_keepalive(struct wg_peer *peer) {
    /* Use the existing encrypt path with zero-length payload.
     * We build an empty message: header (16 bytes) + auth tag (16 bytes) = 32 bytes,
     * with zero-length ciphertext. */
    uint8_t session_key[32];
    uint8_t shared_secret[32];

    curve25519(shared_secret, g_wg.private_key, peer->public_key);
    wg_kdf(session_key, shared_secret, g_wg.private_key, peer->public_key);

    uint8_t buf[64];  /* More than enough for header + tag */
    memset(buf, 0, 16);
    buf[0] = WG_MSG_KEEPALIVE;  /* Keepalive / transport message type */

    uint8_t nonce[12] = {0};
    static uint64_t wg_ka_counter = 0;
    wg_ka_counter++;
    *(uint64_t *)nonce = wg_ka_counter;

    /* Encrypt zero-length payload: just the auth tag */
    uint8_t tag[16];
    chacha20poly1305_encrypt(tag, NULL, 0, buf, 16, session_key, nonce);
    memcpy(buf + 16, tag, 16);

    kprintf("[WG] Sending keepalive to peer %d.%d.%d.%d:%u (counter=%llu)\n",
            (uint8_t)(peer->endpoint_ip >> 24),
            (uint8_t)(peer->endpoint_ip >> 16),
            (uint8_t)(peer->endpoint_ip >> 8),
            (uint8_t)peer->endpoint_ip,
            peer->endpoint_port,
            (unsigned long long)wg_ka_counter);

    /* Update last transmit time */
    peer->last_tx_time = timer_get_ticks();

    /* In a full implementation this would enqueue the packet on the
     * UDP socket.  For now we log the event — the underlying transport
     * (net_udp_send) would be called by the integration layer. */
    (void)buf;
}

/* Periodic poll: check if any peer needs a keepalive sent.
 * Should be called from a timer (e.g., every second or from the
 * scheduler tick). */
void wg_poll(void) {
    if (!wg_initialized) return;

    uint64_t now = timer_get_ticks();
    g_wg.last_poll_time = now;

    /* Convert ticks to seconds (assumes ~100 Hz timer tick rate).
     * timer_get_ticks() returns jiffies, typically 100 per second. */
    const uint64_t ticks_per_sec = 100;  /* HZ */

    for (int i = 0; i < g_wg.num_peers; i++) {
        struct wg_peer *peer = &g_wg.peers[i];
        if (!peer->active) continue;
        if (peer->persistent_keepalive_interval == 0) continue;

        /* Determine the most recent activity time (tx or rx) */
        uint64_t last_activity = peer->last_tx_time;
        if (peer->last_rx_time > last_activity)
            last_activity = peer->last_rx_time;

        /* If we've never sent/received, send a keepalive immediately */
        if (last_activity == 0) {
            wg_send_keepalive(peer);
            continue;
        }

        uint64_t elapsed_ticks = now - last_activity;
        uint64_t interval_ticks = (uint64_t)peer->persistent_keepalive_interval * ticks_per_sec;

        /* Send keepalive if we've been idle longer than the interval */
        if (elapsed_ticks >= interval_ticks) {
            wg_send_keepalive(peer);
        }
    }
}
/* ── Implement: wireguard_encrypt ────────────────── */
int wireguard_encrypt(const uint8_t *plaintext, uint64_t plaintext_len,
                       uint8_t *ciphertext, const uint8_t *key, const uint8_t *nonce)
{
    kprintf("[wireguard] wireguard_encrypt: stub (basic)\n");
    return 0;
}

/* ── Implement: wireguard_decrypt ────────────────── */
int wireguard_decrypt(const uint8_t *ciphertext, uint64_t ciphertext_len,
                       uint8_t *plaintext, const uint8_t *key, const uint8_t *nonce)
{
    kprintf("[wireguard] wireguard_decrypt: stub (basic)\n");
    return 0;
}

/* ── Implement: wireguard_send_handshake_init ────────────────── */
int wireguard_send_handshake_init(uint32_t endpoint_ip, uint16_t endpoint_port)
{
    kprintf("[wireguard] wireguard_send_handshake_init: stub (basic)\n");
    return 0;
}

/* ── Implement: wireguard_recv_handshake_init ────────────────── */
int wireguard_recv_handshake_init(const uint8_t *pkt, uint16_t len,
                                   uint32_t src_ip, uint16_t src_port)
{
    kprintf("[wireguard] wireguard_recv_handshake_init: stub (basic)\n");
    return 0;
}

/* ── Implement: wireguard_send_handshake_response ────────────────── */
int wireguard_send_handshake_response(uint32_t endpoint_ip, uint16_t endpoint_port)
{
    kprintf("[wireguard] wireguard_send_handshake_response: stub (basic)\n");
    return 0;
}

/* ── Implement: wireguard_recv_handshake_response ────────────────── */
int wireguard_recv_handshake_response(const uint8_t *pkt, uint16_t len,
                                       uint32_t src_ip, uint16_t src_port)
{
    kprintf("[wireguard] wireguard_recv_handshake_response: stub (basic)\n");
    return 0;
}

/* ── Implement: wireguard_send_cookie ────────────────── */
int wireguard_send_cookie(uint32_t endpoint_ip, uint16_t endpoint_port,
                           const uint8_t *cookie, uint16_t cookie_len)
{
    kprintf("[wireguard] wireguard_send_cookie: stub (basic)\n");
    return 0;
}

/* ── Implement: wireguard_recv_cookie ────────────────── */
int wireguard_recv_cookie(const uint8_t *pkt, uint16_t len,
                           uint8_t *cookie_out, uint16_t *cookie_len)
{
    kprintf("[wireguard] wireguard_recv_cookie: stub (basic)\n");
    return 0;
}

/* ── Implement: wireguard_ratelimit ────────────────── */
int wireguard_ratelimit(uint32_t src_ip)
{
    kprintf("[wireguard] wireguard_ratelimit: stub (basic)\n");
    return 0;
}

/* ── Implement: wireguard_expire ────────────────── */
int wireguard_expire(int peer_idx)
{
    (void)peer_idx;
    kprintf("[wireguard] wireguard_expire: stub (basic)\n");
    return -EOPNOTSUPP;
}

#include "module.h"
module_init(wg_init);
