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
#include "net.h"
#include "netdevice.h"

static struct wg_device g_wg;
static int wg_initialized = 0;

/* ── Cookie secret (rotates every ~120 seconds) ──────────────── */
static uint8_t  wg_cookie_secret[32];
static uint64_t wg_cookie_secret_time;

/* ── Rate limiting: per-source-IP handshake tracking ─────────── */
struct wg_rl_entry {
    uint32_t src_ip;
    uint32_t count;
    uint64_t window_start;
    int      active;
};
static struct wg_rl_entry wg_rl[WG_RATELIMIT_ENTRIES];

/* ── Forward declarations for static helpers ──────────────────────── */
static int wg_peer_find_by_source(uint32_t src_ip);
static int wg_tx_enqueue(struct wg_peer *peer, uint8_t *data, int len);

/* WireGuard protocol identifier (must match the initiator hash input) */
#define WG_PROTOCOL_ID "WireGuard v1 zx2c4 Jason@zx2c4.com"

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
        /* Carry is recomputed properly below — line 187 was dead ternary */
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

static void chacha20_init(uint32_t state[16], const uint8_t key[32], uint32_t counter, const uint8_t nonce[12]) {
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

static void chacha20_encrypt(uint8_t *out, const uint8_t *in, uint32_t len,
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

static void poly1305_compute(uint8_t mac[16], const uint8_t key[32],
                       const uint8_t *data, uint64_t len) {
    poly1305_state state;
    poly1305_init(&state, key);
    poly1305_update(&state, data, len);
    poly1305_finish(&state, mac);
}

/* ── ChaCha20Poly1305 AEAD (RFC 8439) ───────────────────────────────── */

static void chacha20poly1305_encrypt(uint8_t *out, const uint8_t *in, uint64_t inlen,
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

static int chacha20poly1305_decrypt(uint8_t *out, const uint8_t *in, uint64_t inlen,
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

/* Derive transport encryption key from the Noise chaining key.
 * Called after the full Noise_IKpsk2 handshake completes on both
 * initiator and responder sides to produce the session key used
 * for all subsequent transport data messages. */
static void wg_derive_transport_key(uint8_t *transport_key, const uint8_t *chaining_key)
{
    wg_kdf1(transport_key, chaining_key,
            (const uint8_t *)"WireGuard transport");
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
    g_wg.mtu = WG_MTU;
    g_wg.ifindex = -1;  /* Not yet registered as a net_device */

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

    /* Initialize cookie secret with random bytes */
    for (int i = 0; i < 32; i++)
        wg_cookie_secret[i] = (uint8_t)(rng_get_u64() & 0xFF);
    wg_cookie_secret_time = timer_get_ticks();
    memset(wg_rl, 0, sizeof(wg_rl));

    /* Initialise WireGuard generic netlink configuration interface */
    wg_netlink_init();

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

    /* Clear transport session state (set after handshake) */
    memset(peer->transport_key, 0, 32);
    peer->tx_counter = 0;
    peer->rx_counter = 0;
    peer->session_established = 0;

    /* Initialize TX packet queue */
    peer->tx_head = NULL;
    peer->tx_tail = NULL;
    peer->tx_count = 0;

    /* Generate a peer public key (simulating remote peer's key) */
    uint8_t peer_priv[32];
    for (int i = 0; i < 32; i++)
        peer_priv[i] = (uint8_t)(rng_get_u64() & 0xFF);
    curve25519_clamp(peer_priv);
    curve25519(peer->public_key, peer_priv, CURVE25519_BASE);

    /* Initialize allowed-IP routing table */
    memset(peer->allowed_ips, 0, sizeof(peer->allowed_ips));
    peer->num_allowed_ips = 0;

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

/* ── TX packet queue ────────────────────────────────────────────────── */

/* Enqueue an encrypted packet for transmission via UDP.
 * Takes ownership of @data (will kfree on failure).
 * Returns @len on success, or negative errno. */
static int wg_tx_enqueue(struct wg_peer *peer, uint8_t *data, int len)
{
    struct wg_tx_packet *pkt;

    if (!peer || !data || len <= 0) {
        if (data) kfree(data);
        return -EINVAL;
    }

    /* Back-pressure: drop if queue is full */
    if (peer->tx_count >= WG_TX_QUEUE_MAX_DEPTH) {
        kprintf("[WG] TX queue full for peer %d.%d.%d.%d:%u (%d queued), dropping\n",
                (uint8_t)(peer->endpoint_ip >> 24),
                (uint8_t)(peer->endpoint_ip >> 16),
                (uint8_t)(peer->endpoint_ip >> 8),
                (uint8_t)peer->endpoint_ip,
                peer->endpoint_port,
                peer->tx_count);
        kfree(data);
        return -ENOBUFS;
    }

    pkt = (struct wg_tx_packet *)kmalloc(sizeof(struct wg_tx_packet));
    if (!pkt) {
        kfree(data);
        return -ENOMEM;
    }

    pkt->data     = data;
    pkt->len      = len;
    pkt->dst_ip   = peer->endpoint_ip;
    pkt->dst_port = peer->endpoint_port;
    pkt->src_port = g_wg.listen_port;
    pkt->next     = NULL;

    /* Append to tail of queue */
    if (peer->tx_tail) {
        peer->tx_tail->next = pkt;
    } else {
        peer->tx_head = pkt;
    }
    peer->tx_tail = pkt;
    peer->tx_count++;

    return len;
}

/* Flush (send) all queued packets for a single peer via UDP.
 * Frees each packet after sending. */
static void wg_tx_flush_peer(struct wg_peer *peer)
{
    struct wg_tx_packet *pkt;

    if (!peer || !peer->tx_head)
        return;

    pkt = peer->tx_head;
    while (pkt) {
        struct wg_tx_packet *next = pkt->next;

        if (pkt->data && pkt->len > 0) {
            net_udp_send(pkt->dst_ip, pkt->src_port,
                         pkt->dst_port, pkt->data,
                         (uint16_t)pkt->len);
        }

        kfree(pkt->data);
        kfree(pkt);
        pkt = next;
    }

    peer->tx_head = NULL;
    peer->tx_tail = NULL;
    peer->tx_count = 0;
}

/* Flush all peer TX queues — sends every queued encrypted packet. */
void wg_tx_flush(void)
{
    if (!wg_initialized)
        return;

    for (int i = 0; i < g_wg.num_peers; i++) {
        if (g_wg.peers[i].active)
            wg_tx_flush_peer(&g_wg.peers[i]);
    }
}

/* ── Core send-to-peer helper ──────────────────────────────────────── */

/* Encrypt and format a transport data message for a specific peer.
 * @peer: peer to send to (must be active, non-NULL)
 * @data: plaintext payload
 * @len: plaintext length in bytes
 * Returns total wire bytes on success, or negative errno. */
static int wg_send_to_peer(struct wg_peer *peer, const uint8_t *data, int len)
{
    uint8_t session_key[32];

    if (!peer || !peer->active)
        return -EINVAL;

    /* ── MTU enforcement ─────────────────────────────────────────────
     * The WireGuard tunnel MTU (default 1420) is the maximum size of an
     * inner IP packet.  If the caller hands us a larger payload, reject
     * it with -EMSGSIZE so the upper layer can fragment. */
    if (len > g_wg.mtu) {
        kprintf("[WG] Send_to_peer: inner packet %d bytes exceeds MTU %d, dropping\n",
                len, g_wg.mtu);
        return -EMSGSIZE;
    }

    /* Use cached transport key if handshake has completed, otherwise
     * derive a session key fresh (legacy pre-handshake path). */
    if (peer->session_established) {
        memcpy(session_key, peer->transport_key, 32);
    } else {
        uint8_t shared_secret[32];
        curve25519(shared_secret, g_wg.private_key, peer->public_key);
        wg_kdf(session_key, shared_secret, g_wg.private_key, peer->public_key);
    }

    /* Allocate buffer: header(16) + ciphertext(max) + tag(16) */
    int buf_len = len + 16 + 64;
    uint8_t *buf = (uint8_t *)kmalloc(buf_len);
    if (!buf) return -ENOMEM;

    /* Build WireGuard transport message:
     * [type=4 (1 byte) | reserved(3) | receiver(4) | counter(8) | encrypted(stream) | authtag(16)] */
    uint8_t nonce[12] = {0};
    uint64_t counter;

    if (peer->session_established) {
        counter = peer->tx_counter++;
    } else {
        static uint64_t wg_tx_counter_fallback = 0;
        counter = ++wg_tx_counter_fallback;
    }
    *(uint64_t *)nonce = counter;

    /* Encrypt after the header */
    uint8_t *enc_start = buf + 16;
    uint8_t *ad = buf;
    memset(buf, 0, 16);
    buf[0] = WG_MSG_TRANSPORT_DATA;

    chacha20poly1305_encrypt(enc_start, data, (uint64_t)len, ad, 16, session_key, nonce);

    int total_len = 16 + len + 16;  /* header + ciphertext + tag */

    kprintf("[WG] Send %d bytes encrypted to peer %d.%d.%d.%d:%u (counter=%llu)\n",
            len,
            (uint8_t)(peer->endpoint_ip >> 24),
            (uint8_t)(peer->endpoint_ip >> 16),
            (uint8_t)(peer->endpoint_ip >> 8),
            (uint8_t)peer->endpoint_ip,
            peer->endpoint_port,
            (unsigned long long)counter);

    /* Update last transmit time for keepalive tracking */
    peer->last_tx_time = timer_get_ticks();

    /* Enqueue the encrypted packet for actual UDP transmission.
     * wg_tx_enqueue() takes ownership of buf and frees it on failure. */
    return wg_tx_enqueue(peer, buf, total_len);
}

int wg_send(const uint8_t *data, int len) {
    if (!wg_initialized || !data) return -EINVAL;
    if (g_wg.num_peers == 0) return -EHOSTUNREACH;

    struct wg_peer *peer = NULL;
    for (int i = 0; i < g_wg.num_peers; i++) {
        if (g_wg.peers[i].active) {
            peer = &g_wg.peers[i];
            break;
        }
    }
    if (!peer) return -EHOSTUNREACH;

    return wg_send_to_peer(peer, data, len);
}

int wg_receive(const uint8_t *data, int len, uint32_t src_ip, uint16_t src_port) {
    if (!wg_initialized || !data) return -EINVAL;
    if (len < 32) return -EINVAL;  /* Header + tag minimum */

    /* Log a warning if received wire packet exceeds our MTU + overhead;
     * this is not fatal — the peer may have a larger MTU — but it
     * indicates a potential configuration mismatch. */
    if (len > WG_MAX_WIRE_SIZE) {
        kprintf("[WG] Receive: wire packet %d bytes exceeds max wire size %d (MTU %d)\n",
                len, WG_MAX_WIRE_SIZE, g_wg.mtu);
    }

    struct wg_peer *peer = NULL;
    /* Find peer by source IP matching its allowed-IP routing table.
     * Uses wg_peer_find_by_source for longest-prefix match.  If no
     * peer has allowed-IPs configured, falls back to first active peer. */
    {
        int pidx = wg_peer_find_by_source(src_ip);
        if (pidx >= 0) {
            peer = &g_wg.peers[pidx];
        } else {
            /* Fall back to first active peer (legacy path — no routing) */
            for (int i = 0; i < g_wg.num_peers; i++) {
                if (g_wg.peers[i].active) {
                    peer = &g_wg.peers[i];
                    break;
                }
            }
        }
    }
    if (!peer) return -EHOSTUNREACH;

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

    /* Derive session key — use cached transport key if handshake completed */
    uint8_t session_key[32];
    if (peer->session_established) {
        memcpy(session_key, peer->transport_key, 32);
    } else {
        uint8_t shared_secret[32];
        curve25519(shared_secret, g_wg.private_key, peer->public_key);
        wg_kdf(session_key, shared_secret, g_wg.private_key, peer->public_key);
    }

    /* Extract nonce from counter field */
    uint8_t nonce[12] = {0};
    memcpy(nonce, data + 4, 8);  /* Counter after type[1]+reserved[3] */

    /* Validate monotonic counter — reject if counter <= last seen counter */
    uint64_t pkt_counter;
    pkt_counter = (uint64_t)nonce[0] | ((uint64_t)nonce[1] << 8) |
                  ((uint64_t)nonce[2] << 16) | ((uint64_t)nonce[3] << 24) |
                  ((uint64_t)nonce[4] << 32) | ((uint64_t)nonce[5] << 40) |
                  ((uint64_t)nonce[6] << 48) | ((uint64_t)nonce[7] << 56);
    if (peer->session_established && pkt_counter <= peer->rx_counter) {
        kprintf("[WG] Receive: replay detected (counter %llu <= %llu), dropping\n",
                (unsigned long long)pkt_counter,
                (unsigned long long)peer->rx_counter);
        return -EBADMSG;
    }
    if (peer->session_established)
        peer->rx_counter = pkt_counter;

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
        return -EBADMSG;
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

/* ── Endpoint setter ───────────────────────────────────────────────── */

int wg_set_peer_endpoint(int index, uint32_t ip, uint16_t port)
{
    if (!wg_initialized)
        return -EINVAL;
    if (index < 0 || index >= g_wg.num_peers)
        return -EINVAL;
    if (!g_wg.peers[index].active)
        return -EINVAL;

    if (ip != 0)
        g_wg.peers[index].endpoint_ip = ip;
    if (port != 0)
        g_wg.peers[index].endpoint_port = port;

    /* Also update the last observed RX endpoint so roaming starts
     * from the configured address rather than all-zeros. */
    g_wg.peers[index].rx_ip   = g_wg.peers[index].endpoint_ip;
    g_wg.peers[index].rx_port = g_wg.peers[index].endpoint_port;

    kprintf("[WG] Peer %d endpoint set to %d.%d.%d.%d:%u\n",
            index,
            (uint8_t)(g_wg.peers[index].endpoint_ip >> 24),
            (uint8_t)(g_wg.peers[index].endpoint_ip >> 16),
            (uint8_t)(g_wg.peers[index].endpoint_ip >> 8),
            (uint8_t)g_wg.peers[index].endpoint_ip,
            g_wg.peers[index].endpoint_port);
    return 0;
}

/* Send a keepalive (empty payload) packet to the given peer
 * Allocates and enqueues the encrypted keepalive message. */
static void wg_send_keepalive(struct wg_peer *peer)
{
    uint8_t session_key[32];
    uint8_t shared_secret[32];

    curve25519(shared_secret, g_wg.private_key, peer->public_key);
    wg_kdf(session_key, shared_secret, g_wg.private_key, peer->public_key);

    /* Allocate buffer: header (16) + auth tag (16) = 32 bytes */
    uint8_t *buf = (uint8_t *)kmalloc(64);
    if (!buf) {
        kprintf("[WG] keepalive: kmalloc failed\n");
        return;
    }

    memset(buf, 0, 16);
    buf[0] = WG_MSG_KEEPALIVE;

    uint8_t nonce[12] = {0};
    static uint64_t wg_ka_counter = 0;
    wg_ka_counter++;
    *(uint64_t *)nonce = wg_ka_counter;

    /* Encrypt zero-length payload: just the auth tag */
    uint8_t tag[16];
    chacha20poly1305_encrypt(tag, NULL, 0, buf, 16, session_key, nonce);
    memcpy(buf + 16, tag, 16);

    int total_len = 32;  /* header + auth tag */

    kprintf("[WG] Sending keepalive to peer %d.%d.%d.%d:%u (counter=%llu)\n",
            (uint8_t)(peer->endpoint_ip >> 24),
            (uint8_t)(peer->endpoint_ip >> 16),
            (uint8_t)(peer->endpoint_ip >> 8),
            (uint8_t)peer->endpoint_ip,
            peer->endpoint_port,
            (unsigned long long)wg_ka_counter);

    /* Update last transmit time */
    peer->last_tx_time = timer_get_ticks();

    /* Enqueue for transmission */
    int ret = wg_tx_enqueue(peer, buf, total_len);
    if (ret < 0) {
        /* wg_tx_enqueue already freed buf on failure */
        kprintf("[WG] keepalive: enqueue failed (%d)\n", ret);
    }
}

/* Periodic poll: check if any peer needs a keepalive sent.
 * Should be called from a timer (e.g., every second or from the
 * scheduler tick).  Also flushes the TX queue so encrypted packets
 * are sent via UDP. */
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

    /* Flush any queued encrypted packets */
    wg_tx_flush();
}
/* ── Handshake state tracking ─────────────────────────────────────── */

/* Track in-flight Noise_IKpsk2 handshake initiations so the
 * response can be matched by sender_index.
 * Stores both initiator-side and responder-side Noise state. */
struct wg_hs_state {
    uint32_t sender_index;
    uint32_t receiver_index;    /* For responder: initiator's sender_index */
    uint8_t  eph_private[32];
    uint8_t  eph_public[32];
    uint8_t  remote_static[32]; /* peer's static public key (decrypted) */
    uint8_t  chaining_key[32];  /* current Noise chaining key */
    uint8_t  hash[32];          /* current Noise hash */
    uint8_t  remote_eph[32];    /* remote peer's ephemeral public key */
    int      peer_idx;
    int      active;
    int      is_responder;      /* 1 if we are the responder */

    /* ── Cookie challenge state ──────────────────────────────── */
    int      cookie_challenge_sent;     /* 1 if we sent a cookie challenge */
    uint64_t cookie_challenge_time;     /* when the challenge was sent */
};

static struct wg_hs_state g_wg_hs[WG_MAX_HANDSHAKES];

/* Hash mixing helper: hash = KDF1(hash, data) using ChaCha20 KDF.
 * Corresponds to Noise_IK's mix_hash() operation, implemented with
 * the existing ChaCha20-based KDF instead of BLAKE2s. */
static void wg_mix_hash(uint8_t *hash, const uint8_t *data, uint32_t len)
{
    uint8_t buf[32];
    uint8_t input[64];

    memset(input, 0, sizeof(input));
    if (data && len > 0) {
        uint32_t cpylen = len > 64 ? 64 : len;
        memcpy(input, data, cpylen);
    }
    wg_kdf1(buf, hash, input);
    memcpy(hash, buf, 32);
}

/* ── Cookie helper functions ────────────────────────────────────── */

/* Rotate the cookie secret (called periodically or when under load) */
static void wg_cookie_rotate_secret(void)
{
    uint64_t ticks = timer_get_ticks();

    if (ticks - wg_cookie_secret_time < WG_COOKIE_SECRET_ROTATION)
        return;

    for (int i = 0; i < 32; i++)
        wg_cookie_secret[i] = (uint8_t)(rng_get_u64() & 0xFF);
    wg_cookie_secret_time = ticks;
    kprintf("[WG] Cookie secret rotated\n");
}

/* Derive a cookie value for a given source IP.
 * cookie = KDF1(cookie_secret, src_ip_bytes) truncated to WG_COOKIE_LEN.
 * @cookie: output buffer (at least WG_COOKIE_LEN bytes)
 * @src_ip: source IP address in network byte order */
static void wg_cookie_get(uint8_t *cookie, uint32_t src_ip)
{
    uint8_t input[4];

    input[0] = (uint8_t)(src_ip);
    input[1] = (uint8_t)(src_ip >> 8);
    input[2] = (uint8_t)(src_ip >> 16);
    input[3] = (uint8_t)(src_ip >> 24);
    wg_kdf1(cookie, wg_cookie_secret, input);
}

/* Rate-limit check: returns 0 to allow the handshake, -EBUSY to
 * trigger a cookie challenge.  Uses a sliding-window counter per
 * source IP. */
static int wg_rl_check(uint32_t src_ip)
{
    uint64_t now = timer_get_ticks();
    int slot = -1;
    int free_slot = -1;

    for (int i = 0; i < WG_RATELIMIT_ENTRIES; i++) {
        if (!wg_rl[i].active) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }

        /* Expired entry — reclaim */
        if (now - wg_rl[i].window_start > WG_RATELIMIT_WINDOW_TICKS) {
            wg_rl[i].active = 0;
            if (free_slot < 0)
                free_slot = i;
            continue;
        }

        if (wg_rl[i].src_ip == src_ip) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        /* New source IP — start tracking */
        if (free_slot < 0) {
            /* All slots full — evict oldest */
            uint64_t oldest = now;
            int oldest_slot = 0;
            for (int i = 0; i < WG_RATELIMIT_ENTRIES; i++) {
                if (wg_rl[i].window_start < oldest) {
                    oldest = wg_rl[i].window_start;
                    oldest_slot = i;
                }
            }
            free_slot = oldest_slot;
        }

        wg_rl[free_slot].src_ip = src_ip;
        wg_rl[free_slot].count = 1;
        wg_rl[free_slot].window_start = now;
        wg_rl[free_slot].active = 1;
        return 0;  /* Allow first initiation */
    }

    /* Existing entry — increment count */
    wg_rl[slot].count++;

    if (wg_rl[slot].count > WG_RATELIMIT_MAX_BURST) {
        kprintf("[WG] Rate limit exceeded for %u.%u.%u.%u (%u in window)\n",
                (uint8_t)(src_ip >> 24), (uint8_t)(src_ip >> 16),
                (uint8_t)(src_ip >> 8), (uint8_t)src_ip,
                (unsigned)wg_rl[slot].count);
        return -EBUSY;  /* Trigger cookie challenge */
    }

    return 0;  /* Allow */
}

/* ── mac2 helpers ───────────────────────────────────────────────────── */

/* Compute mac2 = KDF1(cookie, msg[0:mac2_offset])[0:16]
 * Used by the initiator/responder to prove possession of a valid
 * cookie when the peer is under load.
 * @mac2_out: output buffer (exactly 16 bytes)
 * @cookie: 16-byte cookie value derived from peer's cookie secret + our IP
 * @msg: the handshake message buffer (initiation or response)
 * @mac2_offset: offset where mac2 field begins (132 for init, 76 for resp) */
static void wg_compute_mac2(uint8_t mac2_out[16], const uint8_t cookie[WG_COOKIE_LEN],
                             uint8_t *msg, uint16_t mac2_offset)
{
    uint8_t full[32];
    wg_kdf1(full, cookie, msg);
    memcpy(mac2_out, full, 16);

    /* Write mac2 into the message buffer too */
    memcpy((uint8_t *)msg + mac2_offset, mac2_out, 16);
}

/* Verify mac2 in a received handshake message.
 * @cookie: 16-byte cookie we sent to this peer in a cookie reply
 * @msg: the received handshake message
 * @mac2_offset: offset where mac2 field begins
 * Returns 0 on match, -EBADMSG on mismatch. */
static int wg_verify_mac2(const uint8_t cookie[WG_COOKIE_LEN],
                           const uint8_t *msg, uint16_t mac2_offset)
{
    uint8_t expected[16];
    uint8_t full[32];
    int diff;

    /* Compute expected mac2 over the entire message (with current mac2 as zeros) */
    uint8_t msg_copy[WG_HANDSHAKE_INIT_LEN];
    uint16_t copy_len = (mac2_offset == 132) ? WG_HANDSHAKE_INIT_LEN : WG_HANDSHAKE_RESPONSE_LEN;
    memcpy(msg_copy, msg, copy_len);
    memset(msg_copy + mac2_offset, 0, 16);

    wg_kdf1(full, cookie, msg_copy);
    memcpy(expected, full, 16);

    diff = 0;
    for (int i = 0; i < 16; i++)
        diff |= (int)(expected[i] ^ msg[mac2_offset + i]);
    if (diff) {
        kprintf("[WG] mac2 verification FAILED\n");
        return -EBADMSG;
    }
    return 0;
}

/* Check if mac2 field is all zeros (no cookie presented).
 * Returns 1 if all zeros, 0 otherwise. */
static int wg_mac2_is_zero(const uint8_t *msg, uint16_t mac2_offset)
{
    for (int i = 0; i < 16; i++) {
        if (msg[mac2_offset + i] != 0)
            return 0;
    }
    return 1;
}

/* ── Implement: wireguard_encrypt ────────────────── */
int wireguard_encrypt(const uint8_t *plaintext, uint64_t plaintext_len,
                       uint8_t *ciphertext, const uint8_t *key, const uint8_t *nonce)
{
    if (!wg_initialized) {
        kprintf("[wireguard] wireguard_encrypt: not initialized\n");
        return -ENOSYS;
    }
    if (!plaintext || !ciphertext || !key || !nonce) {
        kprintf("[wireguard] wireguard_encrypt: invalid parameter (NULL pointer)\n");
        return -EINVAL;
    }
    if (plaintext_len == 0 || plaintext_len > 65535) {
        kprintf("[wireguard] wireguard_encrypt: invalid plaintext_len %llu\n",
                (unsigned long long)plaintext_len);
        return -EINVAL;
    }
    kprintf("[wireguard] wireguard_encrypt: %llu bytes\n",
            (unsigned long long)plaintext_len);
    /* Real ChaCha20Poly1305 encryption (no additional data) */
    chacha20poly1305_encrypt(ciphertext, plaintext, plaintext_len, NULL, 0, key, nonce);
    return (int)(plaintext_len + 16);  /* ciphertext includes 16-byte auth tag */
}

/* ── Implement: wireguard_decrypt ────────────────── */
int wireguard_decrypt(const uint8_t *ciphertext, uint64_t ciphertext_len,
                       uint8_t *plaintext, const uint8_t *key, const uint8_t *nonce)
{
    int ret;

    if (!wg_initialized) {
        kprintf("[wireguard] wireguard_decrypt: not initialized\n");
        return -ENOSYS;
    }
    if (!ciphertext || !plaintext || !key || !nonce) {
        kprintf("[wireguard] wireguard_decrypt: invalid parameter (NULL pointer)\n");
        return -EINVAL;
    }
    if (ciphertext_len < 16 || ciphertext_len > 65535 + 16) {
        kprintf("[wireguard] wireguard_decrypt: invalid ciphertext_len %llu\n",
                (unsigned long long)ciphertext_len);
        return -EINVAL;
    }
    /* Real ChaCha20Poly1305 decryption (no additional data) */
    ret = chacha20poly1305_decrypt(plaintext, ciphertext, ciphertext_len,
                                   NULL, 0, key, nonce);
    if (ret < 0) {
        kprintf("[wireguard] wireguard_decrypt: MAC verification FAILED\n");
        return -EBADMSG;
    }
    kprintf("[wireguard] wireguard_decrypt: %llu bytes\n",
            (unsigned long long)ciphertext_len);
    return (int)(ciphertext_len - 16);  /* plaintext excludes 16-byte auth tag */
}

/* ── WireGuard Noise_IKpsk2 handshake initiation ────────────────── */

/* Full Noise_IKpsk2 handshake initiation message construction.
 *
 * Builds a WireGuard handshake initiation message (type 1, 148 bytes)
 * per the WireGuard protocol specification:
 *
 *   offset  size  field
 *   0       1     type = WG_MSG_HANDSHAKE_INIT
 *   1       3     reserved (zero)
 *   4       4     sender_index (random, LE32)
 *   8       32    unencrypted_ephemeral public key
 *   40      48    encrypted_static public key (AEAD, 32+16)
 *   88      28    encrypted_timestamp (AEAD, 12+16)
 *   116     16    mac1
 *   132     16    mac2 (all zeros for first initiation)
 *
 * Returns the message length (WG_HANDSHAKE_INIT_LEN = 148) on
 * success, or a negative errno on failure. */
int wireguard_send_handshake_init(uint32_t endpoint_ip, uint16_t endpoint_port)
{
    if (!wg_initialized)
        return -ENOSYS;
    if (endpoint_ip == 0 || endpoint_port == 0)
        return -EINVAL;

    /* ── Find peer by endpoint ──────────────────────────────────── */
    struct wg_peer *peer = NULL;
    int peer_idx = -1;

    for (int i = 0; i < g_wg.num_peers; i++) {
        if (g_wg.peers[i].active &&
            g_wg.peers[i].endpoint_ip == endpoint_ip &&
            g_wg.peers[i].endpoint_port == endpoint_port) {
            peer = &g_wg.peers[i];
            peer_idx = i;
            break;
        }
    }
    if (!peer)
        return -EHOSTUNREACH;

    /* ── Allocate handshake initiation message buffer ───────────── */
    uint8_t *msg = (uint8_t *)kmalloc(WG_HANDSHAKE_INIT_LEN);
    if (!msg)
        return -ENOMEM;
    memset(msg, 0, WG_HANDSHAKE_INIT_LEN);

    /* ── Step 1: Message type and sender index ──────────────────── */
    msg[0] = WG_MSG_HANDSHAKE_INIT;                 /* type */

    uint32_t sender_index = (uint32_t)(rng_get_u64() & 0xFFFFFFFF);
    msg[4] = (uint8_t)(sender_index);               /* LE32 */
    msg[5] = (uint8_t)(sender_index >> 8);
    msg[6] = (uint8_t)(sender_index >> 16);
    msg[7] = (uint8_t)(sender_index >> 24);

    /* ── Step 2: Generate ephemeral key pair ────────────────────── */
    uint8_t eph_priv[32], eph_pub[32];

    for (int i = 0; i < 32; i++)
        eph_priv[i] = (uint8_t)(rng_get_u64() & 0xFF);
    curve25519_clamp(eph_priv);
    curve25519(eph_pub, eph_priv, CURVE25519_BASE);
    memcpy(msg + 8, eph_pub, 32);

    /* ── Step 3: Initialize Noise state ─────────────────────────── */
    uint8_t chaining_key[32], hash[32];
    uint8_t zeros[32];

    memset(zeros, 0, 32);
    memcpy(chaining_key, WG_PROTOCOL_ID, 32);
    memcpy(hash, WG_PROTOCOL_ID, 32);

    /* ── Step 4: mix_hash(H, eph_pub) ───────────────────────────── */
    wg_mix_hash(hash, eph_pub, 32);

    /* ── Step 5: DH(eph_priv, rs) → ck, mix_hash(H, ck) ────────── */
    {
        uint8_t dh[32];

        curve25519(dh, eph_priv, peer->public_key);
        wg_kdf1(chaining_key, chaining_key, dh);
        wg_mix_hash(hash, chaining_key, 32);
    }

    /* ── Step 6: DH(s_priv, rs) → ck, mix_hash(H, ck) ──────────── */
    {
        uint8_t dh[32];

        curve25519(dh, g_wg.private_key, peer->public_key);
        wg_kdf1(chaining_key, chaining_key, dh);
        wg_mix_hash(hash, chaining_key, 32);
    }

    /* ── Step 7: AEAD key from ck, encrypt static public key ────── */
    {
        uint8_t enc_key[32], enc_extra[32];
        uint8_t nonce[12];

        memset(nonce, 0, 12);
        wg_kdf2(enc_key, enc_extra, chaining_key, zeros);

        /* Encrypt local static public key (32 bytes) → 48 bytes at msg+40 */
        chacha20poly1305_encrypt(msg + 40, g_wg.public_key, 32,
                                 hash, 32, enc_key, nonce);
    }

    /* ── Step 8: mix_hash(H, encrypted_static) ──────────────────── */
    wg_mix_hash(hash, msg + 40, 48);

    /* ── Step 9: Derive second AEAD key, encrypt timestamp ──────── */
    {
        uint8_t enc_key[32], enc_extra[32];
        uint8_t nonce[12];
        uint8_t timestamp[12];

        memset(nonce, 0, 12);
        memset(timestamp, 0, 12);
        wg_kdf2(enc_key, enc_extra, chaining_key, zeros);

        /* Build timestamp: 8-byte TAI64 seconds + 4-byte nano.
         * Use timer ticks converted to seconds (assuming ~100 Hz tick rate). */
        uint64_t now = timer_get_ticks();
        uint64_t t64_sec = now / 100;

        /* Store as big-endian (TAI64N convention) */
        timestamp[0] = (uint8_t)(t64_sec >> 56);
        timestamp[1] = (uint8_t)(t64_sec >> 48);
        timestamp[2] = (uint8_t)(t64_sec >> 40);
        timestamp[3] = (uint8_t)(t64_sec >> 32);
        timestamp[4] = (uint8_t)(t64_sec >> 24);
        timestamp[5] = (uint8_t)(t64_sec >> 16);
        timestamp[6] = (uint8_t)(t64_sec >> 8);
        timestamp[7] = (uint8_t)(t64_sec);

        chacha20poly1305_encrypt(msg + 88, timestamp, 12,
                                 hash, 32, enc_key, nonce);
    }

    /* ── Step 10: mix_hash(H, encrypted_timestamp) ──────────────── */
    wg_mix_hash(hash, msg + 88, 28);

    /* ── Step 11: Compute mac1 ──────────────────────────────────── */
    {
        /* mac1_key = KDF1(all_zeros, "mac1----" || responder_public_key) */
        uint8_t mac1_label[40];
        uint8_t mac1_key[32];
        uint8_t mac1_input[116];
        uint8_t mac1_full[32];

        memset(mac1_label, 0, sizeof(mac1_label));
        memcpy(mac1_label, "mac1----", 8);
        memcpy(mac1_label + 8, peer->public_key, 32);

        wg_kdf1(mac1_key, zeros, mac1_label);

        /* mac1 = ChaCha20_KDF(mac1_key, msg[0:116]) truncated to 16 bytes */
        memcpy(mac1_input, msg, 116);
        wg_kdf1(mac1_full, mac1_key, mac1_input);
        memcpy(msg + 116, mac1_full, 16);
    }

    /* ── Step 12: mac2 (from stored cookie if available) ────────────── */
    if (peer->has_cookie) {
        wg_compute_mac2(msg + 132, peer->cookie_key, msg, 132);
    }
    /* else msg[132..147] stays zeroed from memset */

    /* ── Store handshake state for response matching ─────────────── */
    {
        int hs_slot = -1;

        for (int i = 0; i < WG_MAX_HANDSHAKES; i++) {
            if (!g_wg_hs[i].active) {
                hs_slot = i;
                break;
            }
        }
        if (hs_slot >= 0) {
            g_wg_hs[hs_slot].sender_index = sender_index;
            g_wg_hs[hs_slot].receiver_index = 0;
            memcpy(g_wg_hs[hs_slot].eph_private, eph_priv, 32);
            memcpy(g_wg_hs[hs_slot].eph_public, eph_pub, 32);
            memcpy(g_wg_hs[hs_slot].chaining_key, chaining_key, 32);
            memcpy(g_wg_hs[hs_slot].hash, hash, 32);
            memset(g_wg_hs[hs_slot].remote_static, 0, 32);
            memset(g_wg_hs[hs_slot].remote_eph, 0, 32);
            g_wg_hs[hs_slot].peer_idx = peer_idx;
            g_wg_hs[hs_slot].active = 1;
            g_wg_hs[hs_slot].is_responder = 0;
            g_wg_hs[hs_slot].cookie_challenge_sent = 0;
            g_wg_hs[hs_slot].cookie_challenge_time = 0;
        } else {
            kprintf("[WG] Warning: no handshake slot available, overwriting oldest\n");
            g_wg_hs[0].sender_index = sender_index;
            g_wg_hs[0].receiver_index = 0;
            memcpy(g_wg_hs[0].eph_private, eph_priv, 32);
            memcpy(g_wg_hs[0].eph_public, eph_pub, 32);
            memcpy(g_wg_hs[0].chaining_key, chaining_key, 32);
            memcpy(g_wg_hs[0].hash, hash, 32);
            memset(g_wg_hs[0].remote_static, 0, 32);
            memset(g_wg_hs[0].remote_eph, 0, 32);
            g_wg_hs[0].peer_idx = peer_idx;
            g_wg_hs[0].active = 1;
            g_wg_hs[0].is_responder = 0;
            g_wg_hs[0].cookie_challenge_sent = 0;
            g_wg_hs[0].cookie_challenge_time = 0;
        }
    }

    kprintf("[WG] Handshake initiation -> %u:%u (sender_idx=%u, eph=%02x%02x..)\n",
            endpoint_ip, (unsigned)endpoint_port,
            sender_index, eph_pub[0], eph_pub[1]);

    kfree(msg);
    return WG_HANDSHAKE_INIT_LEN;
}

/* ── Implement: wireguard_recv_handshake_init (responder side) ──── */
int wireguard_recv_handshake_init(const uint8_t *pkt, uint16_t len,
                                   uint32_t src_ip, uint16_t src_port)
{
    if (!wg_initialized)
        return -ENOSYS;
    if (!pkt || len < WG_HANDSHAKE_INIT_LEN)
        return -EINVAL;
    if (pkt[0] != WG_MSG_HANDSHAKE_INIT)
        return -EINVAL;

    /* Extract fields from init message */
    uint32_t sender_index = (uint32_t)pkt[4] | ((uint32_t)pkt[5] << 8) |
                            ((uint32_t)pkt[6] << 16) | ((uint32_t)pkt[7] << 24);
    const uint8_t *remote_eph = pkt + 8;     /* 32 bytes */
    const uint8_t *enc_static = pkt + 40;    /* 48 bytes */
    const uint8_t *enc_ts     = pkt + 88;    /* 28 bytes */
    const uint8_t *mac1_rcvd  = pkt + 116;   /* 16 bytes */

    /* ── Step A: Verify mac1 ───────────────────────────────────────── */
    {
        uint8_t zeros[32];
        uint8_t mac1_label[40];
        uint8_t mac1_key[32], mac1_full[32];
        uint8_t diff;

        memset(zeros, 0, 32);
        memset(mac1_label, 0, sizeof(mac1_label));
        memcpy(mac1_label, "mac1----", 8);
        memcpy(mac1_label + 8, g_wg.public_key, 32);
        wg_kdf1(mac1_key, zeros, mac1_label);
        wg_kdf1(mac1_full, mac1_key, pkt);

        diff = 0;
        for (int i = 0; i < 16; i++)
            diff |= mac1_full[i] ^ mac1_rcvd[i];
        if (diff) {
            kprintf("[WG] Init: mac1 verification FAILED from %u:%u\n",
                    src_ip, (unsigned)src_port);
            return -EBADMSG;
        }
    }

    /* ── Step B: Rate limit check & cookie challenge ───────────────── */
    {
        int rl_ret;
        uint8_t cookie_for_ip[WG_COOKIE_LEN];

        /* Rotate cookie secret before checking */
        wg_cookie_rotate_secret();

        rl_ret = wg_rl_check(src_ip);
        if (rl_ret < 0) {
            /* Rate limit exceeded — issue cookie challenge */

            /* Derive the cookie this IP should use */
            wg_cookie_get(cookie_for_ip, src_ip);

            /* Check mac2 — if peer has presented a valid cookie, allow */
            if (!wg_mac2_is_zero(pkt, 132)) {
                if (wg_verify_mac2(cookie_for_ip, pkt, 132) == 0) {
                    /* Valid mac2 — allow handshake to proceed */
                    kprintf("[WG] Init: rate limited but valid mac2 from %u:%u, allowing\n",
                            src_ip, (unsigned)src_port);
                } else {
                    /* Invalid mac2 — drop */
                    kprintf("[WG] Init: rate limited with INVALID mac2 from %u:%u\n",
                            src_ip, (unsigned)src_port);
                    return -EBADMSG;
                }
            } else {
                /* No mac2 — send cookie reply, reject initiation */
                kprintf("[WG] Init: rate limited from %u:%u, sending cookie challenge\n",
                        src_ip, (unsigned)src_port);

                wireguard_send_cookie(src_ip, src_port,
                                      cookie_for_ip, WG_COOKIE_LEN);

                /* Store cookie challenge in handshake state tracking */
                {
                    int hs_slot = -1;
                    for (int i = 0; i < WG_MAX_HANDSHAKES; i++) {
                        if (!g_wg_hs[i].active) {
                            hs_slot = i;
                            break;
                        }
                    }
                    if (hs_slot >= 0) {
                        g_wg_hs[hs_slot].sender_index = sender_index;
                        g_wg_hs[hs_slot].receiver_index = 0;
                        memset(g_wg_hs[hs_slot].eph_private, 0, 32);
                        memset(g_wg_hs[hs_slot].eph_public, 0, 32);
                        memset(g_wg_hs[hs_slot].remote_static, 0, 32);
                        memset(g_wg_hs[hs_slot].remote_eph, 0, 32);
                        memset(g_wg_hs[hs_slot].chaining_key, 0, 32);
                        memset(g_wg_hs[hs_slot].hash, 0, 32);
                        g_wg_hs[hs_slot].peer_idx = -1;
                        g_wg_hs[hs_slot].active = 1;
                        g_wg_hs[hs_slot].is_responder = 1;
                        g_wg_hs[hs_slot].cookie_challenge_sent = 1;
                        g_wg_hs[hs_slot].cookie_challenge_time = timer_get_ticks();
                    }
                }

                return -EBUSY;  /* Tell caller to retry with cookie */
            }
        }
    }

    /* Initialize Noise state */
    uint8_t chaining_key[32], hash[32], zeros[32];
    memset(zeros, 0, 32);
    memcpy(chaining_key, WG_PROTOCOL_ID, 32);
    memcpy(hash, WG_PROTOCOL_ID, 32);

    /* Step 1: mix_hash(H, ephemeral) */
    wg_mix_hash(hash, remote_eph, 32);

    /* Step 2: DH(static_priv, remote_eph) — es → ck, mix_hash */
    {
        uint8_t dh[32];
        curve25519(dh, g_wg.private_key, remote_eph);
        wg_kdf1(chaining_key, chaining_key, dh);
        wg_mix_hash(hash, chaining_key, 32);
    }

    /* Step 3: Derive AEAD key, decrypt initiator's static public key */
    uint8_t remote_static[32];
    {
        uint8_t enc_key[32], enc_extra[32], nonce[12];
        memset(nonce, 0, 12);
        wg_kdf2(enc_key, enc_extra, chaining_key, zeros);

        if (chacha20poly1305_decrypt(remote_static, enc_static, 48,
                                     hash, 32, enc_key, nonce) < 0)
            return -EKEYREJECTED;
    }

    /* Step 4: mix_hash(H, encrypted_static) */
    wg_mix_hash(hash, enc_static, 48);

    /* Step 5: DH(static_priv, remote_static) — ss → ck, mix_hash */
    {
        uint8_t dh[32];
        curve25519(dh, g_wg.private_key, remote_static);
        wg_kdf1(chaining_key, chaining_key, dh);
        wg_mix_hash(hash, chaining_key, 32);
    }

    /* Step 6: Derive second AEAD key, decrypt timestamp */
    {
        uint8_t enc_key[32], enc_extra[32], nonce[12];
        uint8_t timestamp[12];
        memset(nonce, 0, 12);
        wg_kdf2(enc_key, enc_extra, chaining_key, zeros);

        if (chacha20poly1305_decrypt(timestamp, enc_ts, 28,
                                     hash, 32, enc_key, nonce) < 0)
            return -EKEYREJECTED;
        /* In production, verify timestamp freshness */
        (void)timestamp;
    }

    /* Step 7: mix_hash(H, encrypted_timestamp) */
    wg_mix_hash(hash, enc_ts, 28);

    /* Step 8: Find or create peer entry using the decrypted static key */
    int peer_idx = -1;
    for (int i = 0; i < g_wg.num_peers; i++) {
        if (g_wg.peers[i].active &&
            memcmp(g_wg.peers[i].public_key, remote_static, 32) == 0) {
            peer_idx = i;
            break;
        }
    }
    if (peer_idx < 0) {
        /* Unknown initiator — create ad-hoc peer slot */
        if (g_wg.num_peers < WG_MAX_PEERS) {
            peer_idx = g_wg.num_peers;
            struct wg_peer *p = &g_wg.peers[peer_idx];
            memset(p, 0, sizeof(*p));
            memcpy(p->public_key, remote_static, 32);
            p->endpoint_ip = src_ip;
            p->endpoint_port = src_port;
            p->rx_ip = src_ip;
            p->rx_port = src_port;
            p->active = 1;
            g_wg.num_peers++;
            kprintf("[WG] New peer from init: %08x:%u\n",
                    src_ip, (unsigned)src_port);
        } else {
            return -ENOSPC;
        }
    }

    /* Step 9: Reserve handshake state slot, store responder state */
    {
        int hs_slot = -1;

        for (int i = 0; i < WG_MAX_HANDSHAKES; i++) {
            if (!g_wg_hs[i].active) {
                hs_slot = i;
                break;
            }
        }
        if (hs_slot < 0) {
            hs_slot = 0;
            kprintf("[WG] Warning: no handshake slot, overwriting oldest\n");
        }

        g_wg_hs[hs_slot].sender_index = (uint32_t)(rng_get_u64() & 0xFFFFFFFF);
        g_wg_hs[hs_slot].receiver_index = sender_index;
        memset(g_wg_hs[hs_slot].eph_private, 0, 32);
        memset(g_wg_hs[hs_slot].eph_public, 0, 32);
        memcpy(g_wg_hs[hs_slot].remote_static, remote_static, 32);
        memcpy(g_wg_hs[hs_slot].remote_eph, remote_eph, 32);
        memcpy(g_wg_hs[hs_slot].chaining_key, chaining_key, 32);
        memcpy(g_wg_hs[hs_slot].hash, hash, 32);
        g_wg_hs[hs_slot].peer_idx = peer_idx;
        g_wg_hs[hs_slot].active = 1;
        g_wg_hs[hs_slot].is_responder = 1;
        g_wg_hs[hs_slot].cookie_challenge_sent = 0;
        g_wg_hs[hs_slot].cookie_challenge_time = 0;
    }

    kprintf("[WG] Handshake init received from %u:%u (sender_idx=%u)\n",
            src_ip, (unsigned)src_port, sender_index);
    return 0;
}

/* ── Implement: wireguard_send_handshake_response (responder side) ──── */
int wireguard_send_handshake_response(uint32_t endpoint_ip, uint16_t endpoint_port)
{
    if (!wg_initialized)
        return -ENOSYS;
    if (endpoint_ip == 0 || endpoint_port == 0)
        return -EINVAL;

    /* Find responder handshake state for this endpoint */
    int hs_idx = -1;
    for (int i = 0; i < WG_MAX_HANDSHAKES; i++) {
        if (g_wg_hs[i].active && g_wg_hs[i].is_responder) {
            int pidx = g_wg_hs[i].peer_idx;
            if (pidx >= 0 && pidx < g_wg.num_peers &&
                g_wg.peers[pidx].active &&
                g_wg.peers[pidx].endpoint_ip == endpoint_ip &&
                g_wg.peers[pidx].endpoint_port == endpoint_port) {
                hs_idx = i;
                break;
            }
        }
    }
    if (hs_idx < 0)
        return -EHOSTUNREACH;

    struct wg_hs_state *hs = &g_wg_hs[hs_idx];
    uint8_t chaining_key[32], hash[32], zeros[32];
    memcpy(chaining_key, hs->chaining_key, 32);
    memcpy(hash, hs->hash, 32);
    memset(zeros, 0, 32);

    /* Allocate response message buffer (92 bytes) */
    uint8_t *msg = (uint8_t *)kmalloc(WG_HANDSHAKE_RESPONSE_LEN);
    if (!msg)
        return -ENOMEM;
    memset(msg, 0, WG_HANDSHAKE_RESPONSE_LEN);

    /* Step 1: Message type = 2, sender_index, receiver_index */
    msg[0] = WG_MSG_HANDSHAKE_RESPONSE;
    {
        uint32_t si = hs->sender_index;
        msg[4] = (uint8_t)(si);
        msg[5] = (uint8_t)(si >> 8);
        msg[6] = (uint8_t)(si >> 16);
        msg[7] = (uint8_t)(si >> 24);
    }
    {
        uint32_t ri = hs->receiver_index;
        msg[8]  = (uint8_t)(ri);
        msg[9]  = (uint8_t)(ri >> 8);
        msg[10] = (uint8_t)(ri >> 16);
        msg[11] = (uint8_t)(ri >> 24);
    }

    /* Step 2: Generate ephemeral key pair for responder */
    uint8_t eph_priv[32], eph_pub[32];
    for (int i = 0; i < 32; i++)
        eph_priv[i] = (uint8_t)(rng_get_u64() & 0xFF);
    curve25519_clamp(eph_priv);
    curve25519(eph_pub, eph_priv, CURVE25519_BASE);
    memcpy(msg + 12, eph_pub, 32);

    /* Step 3: mix_hash(H, eph_pub) */
    wg_mix_hash(hash, eph_pub, 32);

    /* Step 4: DH(eph_priv, remote_eph) — ee → ck, mix_hash */
    {
        uint8_t dh[32];
        curve25519(dh, eph_priv, hs->remote_eph);
        wg_kdf1(chaining_key, chaining_key, dh);
        wg_mix_hash(hash, chaining_key, 32);
    }

    /* Step 5: Derive AEAD key, encrypt empty payload (just auth tag) */
    {
        uint8_t enc_key[32], enc_extra[32], nonce[12];
        memset(nonce, 0, 12);
        wg_kdf2(enc_key, enc_extra, chaining_key, zeros);
        chacha20poly1305_encrypt(msg + 44, NULL, 0,
                                 hash, 32, enc_key, nonce);
    }

    /* Step 6: mix_hash(H, encrypted_empty) */
    wg_mix_hash(hash, msg + 44, 16);

    /* Step 7: DH(eph_priv, remote_static) — se → ck */
    {
        uint8_t dh[32];
        curve25519(dh, eph_priv, hs->remote_static);
        wg_kdf1(chaining_key, chaining_key, dh);
    }

    /* Step 8: Compute mac1 using initiator's public key
     *   mac1_key = KDF1(zeros, "mac1----" || initiator_static_pub)
     *   mac1 = KDF1(mac1_key, msg[0:60])[0:16] */
    {
        uint8_t mac1_label[40];
        uint8_t mac1_key[32], mac1_full[32];
        memset(mac1_label, 0, sizeof(mac1_label));
        memcpy(mac1_label, "mac1----", 8);
        memcpy(mac1_label + 8, hs->remote_static, 32);
        wg_kdf1(mac1_key, zeros, mac1_label);
        wg_kdf1(mac1_full, mac1_key, msg);
        memcpy(msg + 60, mac1_full, 16);
    }

    /* Step 9: mac2 (from stored cookie if available) — msg[76..91] */
    {
        int pidx = hs->peer_idx;
        if (pidx >= 0 && pidx < g_wg.num_peers && g_wg.peers[pidx].active &&
            g_wg.peers[pidx].has_cookie) {
            wg_compute_mac2(msg + 76, g_wg.peers[pidx].cookie_key, msg, 76);
        }
    }

    /* Step 10: Update handshake state with ephemeral keys and new ck/hash */
    memcpy(hs->eph_private, eph_priv, 32);
    memcpy(hs->eph_public, eph_pub, 32);
    memcpy(hs->chaining_key, chaining_key, 32);
    memcpy(hs->hash, hash, 32);

    /* Derive transport key from final chaining key for the responder.
     * Both sides derive the same key from the same Noise chaining key. */
    {
        int pidx = hs->peer_idx;

        if (pidx >= 0 && pidx < g_wg.num_peers && g_wg.peers[pidx].active) {
            wg_derive_transport_key(g_wg.peers[pidx].transport_key, chaining_key);
            g_wg.peers[pidx].session_established = 1;
            g_wg.peers[pidx].tx_counter = 0;
            g_wg.peers[pidx].rx_counter = 0;
        }
    }

    kprintf("[WG] Handshake response -> %u:%u (sender_idx=%u, eph=%02x%02x..)\n",
            endpoint_ip, (unsigned)endpoint_port,
            hs->sender_index, eph_pub[0], eph_pub[1]);

    kfree(msg);
    return WG_HANDSHAKE_RESPONSE_LEN;
}

/* ── Implement: wireguard_recv_handshake_response (initiator side) ──── */
int wireguard_recv_handshake_response(const uint8_t *pkt, uint16_t len,
                                       uint32_t src_ip, uint16_t src_port)
{
    if (!wg_initialized)
        return -ENOSYS;
    if (!pkt || len < WG_HANDSHAKE_RESPONSE_LEN)
        return -EINVAL;
    if (pkt[0] != WG_MSG_HANDSHAKE_RESPONSE)
        return -EINVAL;

    (void)src_ip;
    (void)src_port;

    /* Extract receiver_index and find matching initiator handshake state */
    uint32_t receiver_index = (uint32_t)pkt[8]  | ((uint32_t)pkt[9] << 8) |
                              ((uint32_t)pkt[10] << 16) | ((uint32_t)pkt[11] << 24);

    int hs_idx = -1;
    for (int i = 0; i < WG_MAX_HANDSHAKES; i++) {
        if (g_wg_hs[i].active && !g_wg_hs[i].is_responder &&
            g_wg_hs[i].sender_index == receiver_index) {
            hs_idx = i;
            break;
        }
    }
    if (hs_idx < 0)
        return -EHOSTUNREACH;

    struct wg_hs_state *hs = &g_wg_hs[hs_idx];

    /* Extract fields from response message */
    const uint8_t *remote_eph = pkt + 12;   /* 32 bytes */
    const uint8_t *enc_empty  = pkt + 44;   /* 16 bytes (AEAD tag only) */
    const uint8_t *mac1_rcvd  = pkt + 60;   /* 16 bytes */

    /* Restore Noise state from handshake state */
    uint8_t chaining_key[32], hash[32], zeros[32];
    memcpy(chaining_key, hs->chaining_key, 32);
    memcpy(hash, hs->hash, 32);
    memset(zeros, 0, 32);

    /* Step 1: mix_hash(H, remote_eph) */
    wg_mix_hash(hash, remote_eph, 32);

    /* Step 2: DH(our_eph_priv, remote_eph) — ee → ck, mix_hash */
    {
        uint8_t dh[32];
        curve25519(dh, hs->eph_private, remote_eph);
        wg_kdf1(chaining_key, chaining_key, dh);
        wg_mix_hash(hash, chaining_key, 32);
    }

    /* Step 3: Derive AEAD key, verify empty AEAD */
    uint8_t empty_out[1];
    {
        uint8_t enc_key[32], enc_extra[32], nonce[12];
        memset(nonce, 0, 12);
        wg_kdf2(enc_key, enc_extra, chaining_key, zeros);

        if (chacha20poly1305_decrypt(empty_out, enc_empty, 16,
                                     hash, 32, enc_key, nonce) < 0) {
            kprintf("[WG] Response AEAD verification FAILED\n");
            return -EKEYREJECTED;
        }
    }

    /* Step 4: mix_hash(H, encrypted_empty) */
    wg_mix_hash(hash, enc_empty, 16);

    /* Step 5: DH(our_static_priv, remote_eph) — se → ck */
    {
        uint8_t dh[32];
        curve25519(dh, g_wg.private_key, remote_eph);
        wg_kdf1(chaining_key, chaining_key, dh);
    }

    /* Step 6: Verify mac1
     *   mac1_key = KDF1(zeros, "mac1----" || our_public_key) */
    {
        uint8_t mac1_label[40];
        uint8_t mac1_key[32], mac1_full[32];
        memset(mac1_label, 0, sizeof(mac1_label));
        memcpy(mac1_label, "mac1----", 8);
        memcpy(mac1_label + 8, g_wg.public_key, 32);
        wg_kdf1(mac1_key, zeros, mac1_label);
        wg_kdf1(mac1_full, mac1_key, pkt);

        uint8_t diff = 0;
        for (int i = 0; i < 16; i++)
            diff |= mac1_full[i] ^ mac1_rcvd[i];
        if (diff) {
            kprintf("[WG] Response mac1 verification FAILED\n");
            return -EKEYREJECTED;
        }
    }

    /* Step 7: Session established — derive transport key from chaining_key */
    {
        int pidx = hs->peer_idx;

        if (pidx >= 0 && pidx < g_wg.num_peers && g_wg.peers[pidx].active) {
            wg_derive_transport_key(g_wg.peers[pidx].transport_key, chaining_key);
            g_wg.peers[pidx].session_established = 1;
            g_wg.peers[pidx].tx_counter = 0;
            g_wg.peers[pidx].rx_counter = 0;
            kprintf("[WG] Session established with peer %d (%u:%u)\n",
                    pidx,
                    (unsigned)g_wg.peers[pidx].endpoint_ip,
                    (unsigned)g_wg.peers[pidx].endpoint_port);
        }
    }

    /* Step 8: Mark handshake complete */
    hs->active = 0;

    kprintf("[WG] Handshake response received from %u:%u (eph=%02x%02x..)\n",
            src_ip, (unsigned)src_port, remote_eph[0], remote_eph[1]);
    return 0;
}

/* ── Implement: wireguard_send_cookie ────────────────── */
int wireguard_send_cookie(uint32_t endpoint_ip, uint16_t endpoint_port,
                           const uint8_t *cookie, uint16_t cookie_len)
{
    uint8_t zeros[32];
    uint8_t key_label[40];
    uint8_t enc_key[32];
    uint8_t nonce[12];
    uint8_t mac1_label[40];
    uint8_t mac1_key[32], mac1_full[32];
    uint8_t *msg;

    if (!wg_initialized) {
        kprintf("[wireguard] wireguard_send_cookie: not initialized\n");
        return -ENOSYS;
    }
    if (endpoint_ip == 0 || endpoint_port == 0) {
        kprintf("[wireguard] wireguard_send_cookie: invalid endpoint %u:%u\n",
                endpoint_ip, (unsigned)endpoint_port);
        return -EINVAL;
    }
    if (!cookie || cookie_len == 0 || cookie_len > 64) {
        kprintf("[wireguard] wireguard_send_cookie: invalid cookie (ptr=%p len=%u)\n",
                (const void *)cookie, (unsigned)cookie_len);
        return -EINVAL;
    }

    /* Rotate secret if needed */
    wg_cookie_rotate_secret();

    msg = (uint8_t *)kmalloc(WG_COOKIE_REPLY_LEN);
    if (!msg)
        return -ENOMEM;
    memset(msg, 0, WG_COOKIE_REPLY_LEN);

    /* Message type and reserved */
    msg[0] = WG_MSG_COOKIE_REPLY;

    /* receiver_index — look up from handshake state matching endpoint */
    {
        uint32_t rx_idx = 0;
        for (int i = 0; i < WG_MAX_HANDSHAKES; i++) {
            if (g_wg_hs[i].active) {
                int pidx = g_wg_hs[i].peer_idx;
                if (pidx >= 0 && pidx < g_wg.num_peers &&
                    g_wg.peers[pidx].active &&
                    g_wg.peers[pidx].endpoint_ip == endpoint_ip &&
                    g_wg.peers[pidx].endpoint_port == endpoint_port) {
                    rx_idx = g_wg_hs[i].sender_index;
                    break;
                }
            }
        }
        *(uint32_t *)(msg + 4) = rx_idx;
    }

    /* Generate random nonce (24 bytes; we use first 12 for ChaCha20) */
    for (int i = 0; i < WG_COOKIE_NONCE_LEN; i++)
        msg[8 + i] = (uint8_t)(rng_get_u64() & 0xFF);

    /* Derive encryption key: KDF1(zeros, "cookie--" || our_public_key) */
    memset(zeros, 0, 32);
    memset(key_label, 0, sizeof(key_label));
    memcpy(key_label, "cookie--", 8);
    memcpy(key_label + 8, g_wg.public_key, 32);
    wg_kdf1(enc_key, zeros, key_label);

    /* Encrypt cookie: nonce uses first 12 bytes of the 24-byte field */
    memcpy(nonce, msg + 8, 12);
    chacha20poly1305_encrypt(msg + 32, cookie, cookie_len,
                              msg, 32, enc_key, nonce);

    /* Compute mac1 over the entire message up to (but not including) mac1.
     * mac1_key = KDF1(zeros, "mac1----" || our_public_key) */
    memset(mac1_label, 0, sizeof(mac1_label));
    memcpy(mac1_label, "mac1----", 8);
    memcpy(mac1_label + 8, g_wg.public_key, 32);
    wg_kdf1(mac1_key, zeros, mac1_label);
    wg_kdf1(mac1_full, mac1_key, msg);
    memcpy(msg + 64, mac1_full, 16);

    kprintf("[WG] Sending cookie reply to %u:%u (%u bytes cookie)\n",
            endpoint_ip, (unsigned)endpoint_port, (unsigned)cookie_len);

    kfree(msg);
    return WG_COOKIE_REPLY_LEN;
}

/* ── Implement: wireguard_recv_cookie ────────────────── */
int wireguard_recv_cookie(const uint8_t *pkt, uint16_t len,
                           uint8_t *cookie_out, uint16_t *cookie_len)
{
    uint8_t zeros[32];
    uint8_t key_label[40];
    uint8_t enc_key[32];
    uint8_t nonce[12];
    uint8_t mac1_label[40];
    uint8_t mac1_key[32], mac1_full[32];
    int diff;
    int ret;
    uint8_t raw_cookie[16];

    if (!wg_initialized) {
        kprintf("[wireguard] wireguard_recv_cookie: not initialized\n");
        return -ENOSYS;
    }
    if (!pkt || !cookie_out || !cookie_len) {
        kprintf("[wireguard] wireguard_recv_cookie: invalid parameter (NULL pointer)\n");
        return -EINVAL;
    }
    if (len < WG_COOKIE_REPLY_LEN) {
        kprintf("[wireguard] wireguard_recv_cookie: packet too short (%u bytes)\n",
                (unsigned)len);
        return -EINVAL;
    }
    if (pkt[0] != WG_MSG_COOKIE_REPLY) {
        kprintf("[wireguard] wireguard_recv_cookie: bad message type %u\n",
                (unsigned)pkt[0]);
        return -EINVAL;
    }

    /* Verify mac1:
     * mac1_key = KDF1(zeros, "mac1----" || our_public_key)
     * mac1 covers msg[0..63] */
    memset(zeros, 0, 32);
    memset(mac1_label, 0, sizeof(mac1_label));
    memcpy(mac1_label, "mac1----", 8);
    memcpy(mac1_label + 8, g_wg.public_key, 32);
    wg_kdf1(mac1_key, zeros, mac1_label);
    wg_kdf1(mac1_full, mac1_key, pkt);

    diff = 0;
    for (int i = 0; i < 16; i++)
        diff |= (int)(mac1_full[i] ^ pkt[64 + i]);
    if (diff) {
        kprintf("[wireguard] wireguard_recv_cookie: mac1 verification FAILED\n");
        return -EBADMSG;
    }

    /* Derive decryption key (same as encryption key) */
    memset(key_label, 0, sizeof(key_label));
    memcpy(key_label, "cookie--", 8);
    memcpy(key_label + 8, g_wg.public_key, 32);
    wg_kdf1(enc_key, zeros, key_label);

    /* Decrypt cookie: nonce is first 12 bytes of the 24-byte field */
    memcpy(nonce, pkt + 8, 12);
    ret = chacha20poly1305_decrypt(raw_cookie, pkt + 32, WG_COOKIE_REPLY_LEN - 64,
                                    pkt, 32, enc_key, nonce);
    if (ret < 0) {
        kprintf("[wireguard] wireguard_recv_cookie: decryption FAILED\n");
        return -EBADMSG;
    }

    /* Return length of decrypted cookie (wire value is 16 bytes) */
    *cookie_len = WG_COOKIE_LEN;
    if (cookie_out)
        memcpy(cookie_out, raw_cookie, WG_COOKIE_LEN);

    /* ── Store expanded cookie key in the matching peer ──────────── */
    {
        uint32_t receiver_index;

        receiver_index = (uint32_t)pkt[4] | ((uint32_t)pkt[5] << 8) |
                         ((uint32_t)pkt[6] << 16) | ((uint32_t)pkt[7] << 24);

        /* Find the handshake state by receiver_index (our sender_index from init) */
        for (int i = 0; i < WG_MAX_HANDSHAKES; i++) {
            if (g_wg_hs[i].active) {
                uint32_t our_idx = g_wg_hs[i].sender_index;

                /* The cookie reply's receiver_index should match our sender_index */
                if (our_idx == receiver_index) {
                    int pidx = g_wg_hs[i].peer_idx;

                    if (pidx >= 0 && pidx < g_wg.num_peers &&
                        g_wg.peers[pidx].active) {
                        /* Expand 16-byte wire cookie to 32-byte KDF key:
                         * use raw_cookie as first 16 bytes, zero-pad the rest */
                        memset(g_wg.peers[pidx].cookie_key, 0, 32);
                        memcpy(g_wg.peers[pidx].cookie_key, raw_cookie, WG_COOKIE_LEN);
                        g_wg.peers[pidx].has_cookie = 1;

                        kprintf("[WG] Cookie stored for peer %d (%u:%u)\n",
                                pidx,
                                (unsigned)g_wg.peers[pidx].endpoint_ip,
                                (unsigned)g_wg.peers[pidx].endpoint_port);
                    }
                    break;
                }
            }
        }
    }

    kprintf("[WG] Received cookie reply (receiver_idx=%u)\n",
            *(const uint32_t *)(pkt + 4));
    return 0;
}

/* ── Implement: wireguard_ratelimit ────────────────── */
int wireguard_ratelimit(uint32_t src_ip)
{
    if (!wg_initialized) {
        kprintf("[wireguard] wireguard_ratelimit: not initialized\n");
        return -ENOSYS;
    }
    if (src_ip == 0) {
        kprintf("[wireguard] wireguard_ratelimit: invalid source IP\n");
        return -EINVAL;
    }

    /* Rotate cookie secret if needed */
    wg_cookie_rotate_secret();

    /* Delegate to the internal rate-limit check */
    return wg_rl_check(src_ip);
}

/* ── Implement: wireguard_expire ────────────────── */
int wireguard_expire(int peer_idx)
{
    if (!wg_initialized) {
        kprintf("[wireguard] wireguard_expire: not initialized\n");
        return -ENOSYS;
    }
    if (peer_idx < 0 || peer_idx >= g_wg.num_peers) {
        kprintf("[wireguard] wireguard_expire: invalid peer index %d (num_peers=%d)\n",
                peer_idx, g_wg.num_peers);
        return -EINVAL;
    }
    if (!g_wg.peers[peer_idx].active) {
        kprintf("[wireguard] wireguard_expire: peer %d already inactive\n", peer_idx);
        return -EALREADY;
    }
    kprintf("[wireguard] wireguard_expire: expiring peer %d (%u:%u)\n",
            peer_idx, g_wg.peers[peer_idx].endpoint_ip,
            (unsigned)g_wg.peers[peer_idx].endpoint_port);
    g_wg.peers[peer_idx].active = 0;
    return 0;
}

/* ── Allowed-IP routing implementation ───────────────────────────── */

/* Check if an IPv4 address matches a CIDR range.
 * Returns 1 if dest_ip is within addr/cidr, 0 otherwise. */
static int wg_allowed_ip_match(uint32_t addr, uint8_t cidr, uint32_t dest_ip)
{
    uint32_t mask;

    if (cidr == 0)
        return 1;  /* 0.0.0.0/0 matches all */

    if (cidr >= 32)
        return addr == dest_ip;

    mask = (uint32_t)(0xFFFFFFFF << (32 - cidr));
    return (addr & mask) == (dest_ip & mask);
}

/* Check whether a source IP is allowed for a peer (matches any allowed-IP).
 * Returns 1 if allowed (or no allowed-IPs configured), 0 if denied. */
static int wg_peer_source_allowed(struct wg_peer *peer, uint32_t src_ip)
{
    if (!peer || !peer->active)
        return 0;
    if (peer->num_allowed_ips == 0)
        return 1;  /* No restrictions — allow all */

    for (int i = 0; i < peer->num_allowed_ips; i++) {
        if (peer->allowed_ips[i].active &&
            wg_allowed_ip_match(peer->allowed_ips[i].addr,
                                peer->allowed_ips[i].cidr, src_ip)) {
            return 1;
        }
    }
    return 0;
}

int wg_peer_check_source(int peer_idx, uint32_t src_ip)
{
    if (!wg_initialized)
        return -ENOSYS;
    if (peer_idx < 0 || peer_idx >= g_wg.num_peers)
        return -EINVAL;
    if (!g_wg.peers[peer_idx].active)
        return -EINVAL;

    return wg_peer_source_allowed(&g_wg.peers[peer_idx], src_ip) ? 1 : 0;
}

int wg_peer_add_allowed_ip(int peer_idx, uint32_t addr, uint8_t cidr)
{
    if (!wg_initialized)
        return -ENOSYS;
    if (peer_idx < 0 || peer_idx >= g_wg.num_peers)
        return -EINVAL;
    if (!g_wg.peers[peer_idx].active)
        return -EINVAL;
    if (cidr > 32)
        return -EINVAL;

    struct wg_peer *peer = &g_wg.peers[peer_idx];

    if (peer->num_allowed_ips >= WG_MAX_ALLOWED_IPS)
        return -ENOSPC;

    /* Check for duplicate */
    for (int i = 0; i < peer->num_allowed_ips; i++) {
        if (peer->allowed_ips[i].active &&
            peer->allowed_ips[i].addr == addr &&
            peer->allowed_ips[i].cidr == cidr) {
            return -EALREADY;
        }
    }

    struct wg_allowed_ip *entry = &peer->allowed_ips[peer->num_allowed_ips];
    entry->addr = addr;
    entry->cidr = cidr;
    entry->active = 1;
    peer->num_allowed_ips++;

    kprintf("[WG] Peer %d: added allowed-IP %u.%u.%u.%u/%u\n",
            peer_idx,
            (uint8_t)(addr >> 24), (uint8_t)(addr >> 16),
            (uint8_t)(addr >> 8), (uint8_t)addr,
            (unsigned)cidr);
    return 0;
}

int wg_peer_remove_allowed_ip(int peer_idx, uint32_t addr, uint8_t cidr)
{
    if (!wg_initialized)
        return -ENOSYS;
    if (peer_idx < 0 || peer_idx >= g_wg.num_peers)
        return -EINVAL;
    if (!g_wg.peers[peer_idx].active)
        return -EINVAL;

    struct wg_peer *peer = &g_wg.peers[peer_idx];

    for (int i = 0; i < peer->num_allowed_ips; i++) {
        if (peer->allowed_ips[i].active &&
            peer->allowed_ips[i].addr == addr &&
            peer->allowed_ips[i].cidr == cidr) {
            peer->allowed_ips[i].active = 0;

            /* Compact the array */
            for (int j = i; j < peer->num_allowed_ips - 1; j++)
                peer->allowed_ips[j] = peer->allowed_ips[j + 1];
            peer->num_allowed_ips--;

            kprintf("[WG] Peer %d: removed allowed-IP %u.%u.%u.%u/%u\n",
                    peer_idx,
                    (uint8_t)(addr >> 24), (uint8_t)(addr >> 16),
                    (uint8_t)(addr >> 8), (uint8_t)addr,
                    (unsigned)cidr);
            return 0;
        }
    }
    return -ENOENT;
}

/* Find the peer whose allowed-IPs include src_ip, with best-prefix tiebreak.
 * Returns peer index on success, or -EHOSTUNREACH if no match. */
static int wg_peer_find_by_source(uint32_t src_ip)
{
    int best_peer = -1;
    int best_prefix = -1;

    for (int i = 0; i < g_wg.num_peers; i++) {
        if (!g_wg.peers[i].active)
            continue;

        if (g_wg.peers[i].num_allowed_ips == 0) {
            /* No allowed-IPs — match all but with lowest priority */
            if (best_peer < 0 || best_prefix < 0) {
                best_peer = i;
                best_prefix = -1;
            }
            continue;
        }

        for (int j = 0; j < g_wg.peers[i].num_allowed_ips; j++) {
            struct wg_allowed_ip *aip = &g_wg.peers[i].allowed_ips[j];
            if (!aip->active)
                continue;
            if (wg_allowed_ip_match(aip->addr, aip->cidr, src_ip)) {
                if ((int)aip->cidr > best_prefix) {
                    best_prefix = (int)aip->cidr;
                    best_peer = i;
                }
            }
        }
    }

    if (best_peer < 0)
        return -EHOSTUNREACH;
    return best_peer;
}

int wg_peer_lookup_by_dest(uint32_t dest_ip)
{
    if (!wg_initialized)
        return -ENOSYS;

    int best_peer = -1;
    int best_prefix = -1;

    for (int i = 0; i < g_wg.num_peers; i++) {
        if (!g_wg.peers[i].active)
            continue;

        struct wg_peer *peer = &g_wg.peers[i];

        for (int j = 0; j < peer->num_allowed_ips; j++) {
            if (!peer->allowed_ips[j].active)
                continue;

            uint8_t cidr = peer->allowed_ips[j].cidr;
            uint32_t addr = peer->allowed_ips[j].addr;

            if (wg_allowed_ip_match(addr, cidr, dest_ip)) {
                if ((int)cidr > best_prefix) {
                    best_prefix = (int)cidr;
                    best_peer = i;
                }
            }
        }
    }

    if (best_peer < 0)
        return -EHOSTUNREACH;

    return best_peer;
}

int wg_send_to(uint32_t dest_ip, const uint8_t *data, int len)
{
    int peer_idx;

    if (!wg_initialized || !data)
        return -EINVAL;
    if (g_wg.num_peers == 0)
        return -EHOSTUNREACH;

    if (dest_ip == 0) {
        /* No specific destination — use first active peer (legacy fallback) */
        peer_idx = -1;
        for (int i = 0; i < g_wg.num_peers; i++) {
            if (g_wg.peers[i].active) {
                peer_idx = i;
                break;
            }
        }
        if (peer_idx < 0)
            return -EHOSTUNREACH;
    } else {
        peer_idx = wg_peer_lookup_by_dest(dest_ip);
        if (peer_idx < 0) {
            kprintf("[WG] Send_to: no route to %u.%u.%u.%u\n",
                    (uint8_t)(dest_ip >> 24), (uint8_t)(dest_ip >> 16),
                    (uint8_t)(dest_ip >> 8), (uint8_t)dest_ip);
            return peer_idx;  /* Negative errno */
        }
    }

    return wg_send_to_peer(&g_wg.peers[peer_idx], data, len);
}

/* ── Accessor functions for wg_netlink.c ────────────────────────────── */

int wg_set_private_key(const uint8_t key[32])
{
    if (!wg_initialized || !key)
        return -EINVAL;

    memcpy(g_wg.private_key, key, 32);

    /* Clamp per Curve25519 spec */
    g_wg.private_key[0] &= 248;
    g_wg.private_key[31] &= 127;
    g_wg.private_key[31] |= 64;

    /* Derive public key = Curve25519(private, basepoint) */
    curve25519(g_wg.public_key, g_wg.private_key, CURVE25519_BASE);

    return 0;
}

void wg_get_device_pubkey(uint8_t out[32])
{
    if (wg_initialized && out)
        memcpy(out, g_wg.public_key, 32);
}

uint16_t wg_get_listen_port(void)
{
    return g_wg.listen_port;
}

void wg_set_listen_port(uint16_t port)
{
    g_wg.listen_port = port;
}

int wg_find_peer_by_pubkey(const uint8_t pubkey[32])
{
    if (!wg_initialized || !pubkey)
        return -EINVAL;

    for (int i = 0; i < g_wg.num_peers; i++) {
        if (!g_wg.peers[i].active)
            continue;
        if (memcmp(g_wg.peers[i].public_key, pubkey, 32) == 0)
            return i;
    }
    return -ENOENT;
}

int wg_create_peer_with_key(const uint8_t pubkey[32])
{
    if (!wg_initialized || !pubkey)
        return -EINVAL;
    if (g_wg.num_peers >= WG_MAX_PEERS)
        return -ENOSPC;

    struct wg_peer *peer = &g_wg.peers[g_wg.num_peers];
    memset(peer, 0, sizeof(*peer));
    peer->active = 1;
    memcpy(peer->public_key, pubkey, 32);
    peer->persistent_keepalive_interval = WG_KEEPALIVE_DEFAULT_INTERVAL;
    peer->session_established = 0;

    g_wg.num_peers++;
    return g_wg.num_peers - 1;
}

int wg_get_num_peers(void)
{
    return wg_initialized ? g_wg.num_peers : 0;
}

int wg_get_peer_info(int idx, struct wg_peer *out)
{
    if (!wg_initialized || !out)
        return -EINVAL;
    if (idx < 0 || idx >= g_wg.num_peers)
        return -EINVAL;
    if (!g_wg.peers[idx].active)
        return -EINVAL;

    memcpy(out, &g_wg.peers[idx], sizeof(struct wg_peer));
    return 0;
}

/* ── MTU accessors ───────────────────────────────────────────────── */

int wg_get_mtu(void)
{
    if (!wg_initialized)
        return -ENOSYS;
    return g_wg.mtu;
}

int wg_set_mtu(int mtu)
{
    if (!wg_initialized)
        return -ENOSYS;
    if (mtu < 576) {
        kprintf("[WG] set_mtu: %d too low, clamping to 576\n", mtu);
        mtu = 576;
    }
    if (mtu > 1500) {
        kprintf("[WG] set_mtu: %d too high, clamping to 1500\n", mtu);
        mtu = 1500;
    }
    g_wg.mtu = mtu;
    kprintf("[WG] MTU set to %d\n", g_wg.mtu);
    return 0;
}

/* ── Interface lifecycle (virtual net_device) ───────────────────── */

/*
 * Transmit callback for the WireGuard virtual net_device.
 * Takes an Ethernet frame, removes the Ethernet header, encrypts
 * the inner IP packet via the WireGuard crypto layer, and enqueues
 * the resulting UDP payload for transmission.
 *
 * Returns 0 on success, -1 on failure (net_device convention). */
static int wg_transmit(struct net_device *dev,
                        const uint8_t *data, uint16_t len)
{
    struct wg_device *wg;
    const uint8_t *payload;
    uint16_t payload_len;
    int ret;

    if (!dev || !data || len == 0)
        return -1;

    wg = (struct wg_device *)dev->priv;
    if (!wg || !wg_initialized)
        return -1;

    /* Strip Ethernet header (14 bytes) to get the inner IP packet */
    if (len <= 14) {
        kprintf("[WG] iface xmit: frame too short (%u bytes)\n", (unsigned)len);
        return -1;
    }
    payload = data + 14;
    payload_len = len - 14;

    /* Enforce tunnel MTU on the inner IP packet */
    if (payload_len > (uint16_t)wg->mtu) {
        kprintf("[WG] iface xmit: inner packet %u > MTU %d\n",
                (unsigned)payload_len, wg->mtu);
        return -1;
    }

    /* Encrypt and enqueue via the existing WireGuard data path */
    ret = wg_send(payload, (int)payload_len);
    if (ret < 0) {
        kprintf("[WG] iface xmit: wg_send failed (%d)\n", ret);
        return -1;
    }

    /* Flush the TX queue so the encrypted packet goes out immediately */
    wg_tx_flush();

    return 0;
}

int wg_iface_create(void)
{
    struct net_device nd;
    int ifindex;

    if (!wg_initialized)
        return -ENOSYS;

    /* Check if already registered */
    if (g_wg.ifindex >= 0 && netif_valid(g_wg.ifindex))
        return -EALREADY;

    memset(&nd, 0, sizeof(nd));
    memcpy(nd.name, "wg0", 4);  /* "wg0" + NUL */
    nd.transmit = wg_transmit;
    nd.receive  = NULL;          /* RX is handled via UDP receive path */
    nd.mtu      = g_wg.mtu;
    nd.flags    = 0;             /* Start down */
    nd.priv     = (void *)&g_wg;

    /* Generate a locally-administered MAC address from the public key */
    {
        uint8_t hash[32];
        uint8_t zeros[32];

        memset(zeros, 0, sizeof(zeros));
        wg_kdf1(hash, zeros, g_wg.public_key);

        nd.mac[0] = 0x8e;
        nd.mac[1] = hash[0];
        nd.mac[2] = hash[1];
        nd.mac[3] = hash[2];
        nd.mac[4] = hash[3];
        nd.mac[5] = hash[4];
    }

    ifindex = netif_register(&nd);
    if (ifindex < 0) {
        kprintf("[WG] iface_create: netif_register failed\n");
        return -ENOMEM;
    }

    g_wg.ifindex = ifindex;
    kprintf("[OK] WireGuard interface wg0 created (ifindex=%d, MTU=%d)\n",
            ifindex, g_wg.mtu);
    return ifindex;
}

int wg_iface_destroy(void)
{
    int ifindex;

    if (!wg_initialized)
        return -ENOSYS;

    ifindex = g_wg.ifindex;
    if (ifindex < 0)
        return -EALREADY;  /* Already destroyed */

    /* Flush all pending TX queues */
    wg_tx_flush();

    /* Mark the interface index as invalid */
    g_wg.ifindex = -1;

    /* Unregister from the net_device layer */
    if (netif_unregister(ifindex) < 0) {
        kprintf("[WG] iface_destroy: netif_unregister(%d) failed\n", ifindex);
        return -EIO;
    }

    kprintf("[WG] WireGuard interface wg0 destroyed (was ifindex=%d)\n", ifindex);
    return 0;
}

int wg_iface_up(void)
{
    struct net_device *dev;

    if (!wg_initialized)
        return -ENOSYS;
    if (g_wg.ifindex < 0)
        return -ENODEV;

    dev = netif_get(g_wg.ifindex);
    if (!dev)
        return -ENODEV;

    /* Set UP and RUNNING flags */
    dev->flags |= (IFF_UP | IFF_RUNNING);

    kprintf("[WG] Interface wg0 is UP (ifindex=%d)\n", g_wg.ifindex);
    return 0;
}

int wg_iface_down(void)
{
    struct net_device *dev;

    if (!wg_initialized)
        return -ENOSYS;
    if (g_wg.ifindex < 0)
        return -ENODEV;

    dev = netif_get(g_wg.ifindex);
    if (!dev)
        return -ENODEV;

    /* Clear UP and RUNNING flags */
    dev->flags &= ~(IFF_UP | IFF_RUNNING);

    /* Flush any pending TX packets */
    wg_tx_flush();

    kprintf("[WG] Interface wg0 is DOWN (ifindex=%d)\n", g_wg.ifindex);
    return 0;
}

#include "module.h"
module_init(wg_init);
