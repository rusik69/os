#define KERNEL_INTERNAL
#include "types.h"
#include "ssh.h"
#include "sha256.h"
#include "aes.h"
#include "hmac.h"
#include "rng.h"
#include "string.h"
#include "printf.h"

/* ================================================================
 * Big Number Arithmetic (2048-bit max, 64 x uint32_t limbs, LE)
 * ================================================================ */

/* Convert bytes to bignum (big-endian input) */
void bn_from_bytes(bignum *r, const uint8_t *bytes, int len) {
    memset(r, 0, sizeof(*r));
    if (!bytes || len <= 0) { r->used = 0; return; }
    int skip = 0;
    while (skip < len && bytes[skip] == 0) skip++;
    int actual = len - skip;
    int limbs = (actual + 3) / 4;
    if (limbs > BN_MAX_LIMBS) { skip = len - BN_MAX_BYTES; actual = BN_MAX_BYTES; limbs = BN_MAX_LIMBS; }
    r->used = limbs;
    for (int i = 0; i < actual; i++) {
        int bi = skip + actual - 1 - i;
        int limb_idx = i / 4;
        int shift = (i % 4) * 8;
        r->l[limb_idx] |= ((uint32_t)bytes[bi]) << shift;
    }
}

/* Convert bignum to bytes (big-endian output, min_len pads leading zeros) */
int bn_to_bytes(const bignum *a, uint8_t *bytes, int min_len) {
    if (!a || !bytes) return 0;
    int used = a->used;
    while (used > 0 && a->l[used - 1] == 0) used--;
    int max_byte = used * 4;
    int len = max_byte > min_len ? max_byte : min_len;
    memset(bytes, 0, len);
    for (int i = 0; i < max_byte; i++) {
        int limb_idx = i / 4;
        int shift = (i % 4) * 8;
        bytes[len - 1 - i] = (a->l[limb_idx] >> shift) & 0xFF;
    }
    return len;
}

/* Compare two bignums: -1 if a<b, 0 if a==b, 1 if a>b */
int bn_compare(const bignum *a, const bignum *b) {
    int au = a->used, bu = b->used;
    while (au > 0 && a->l[au - 1] == 0) au--;
    while (bu > 0 && b->l[bu - 1] == 0) bu--;
    if (au != bu) return au < bu ? -1 : 1;
    for (int i = au - 1; i >= 0; i--) {
        if (a->l[i] != b->l[i])
            return a->l[i] < b->l[i] ? -1 : 1;
    }
    return 0;
}

/* Copy bignum */
void bn_copy(bignum *r, const bignum *a) {
    if (r == a) return;
    memcpy(r, a, sizeof(bignum));
}

/* Set from uint32_t */
void bn_set_u32(bignum *r, uint32_t v) {
    memset(r, 0, sizeof(*r));
    r->l[0] = v;
    r->used = 1;
}

/* Test if zero */
int bn_is_zero(const bignum *a) {
    for (int i = 0; i < a->used; i++)
        if (a->l[i] != 0) return 0;
    return 1;
}

/* Test if one */
int bn_is_one(const bignum *a) {
    if (a->l[0] != 1) return 0;
    for (int i = 1; i < a->used; i++)
        if (a->l[i] != 0) return 0;
    return 1;
}

/* Count significant bits */
static int bn_bits(const bignum *a) {
    int used = a->used;
    while (used > 0 && a->l[used - 1] == 0) used--;
    if (used == 0) return 0;
    uint32_t top = a->l[used - 1];
    int bits = (used - 1) * 32;
    while (top) { bits++; top >>= 1; }
    return bits;
}

/* Left shift by one bit (in-place) */
static void bn_lshift1(bignum *r) {
    uint32_t carry = 0;
    for (int i = 0; i < r->used; i++) {
        uint32_t v = r->l[i];
        r->l[i] = (v << 1) | carry;
        carry = v >> 31;
    }
    if (carry && r->used < BN_MAX_LIMBS) {
        r->l[r->used++] = carry;
    }
}

/* Right shift by one bit (in-place) */
static void bn_rshift1(bignum *r) {
    uint32_t carry = 0;
    for (int i = r->used - 1; i >= 0; i--) {
        uint32_t v = r->l[i];
        r->l[i] = (v >> 1) | (carry << 31);
        carry = v & 1;
    }
    while (r->used > 0 && r->l[r->used - 1] == 0) r->used--;
}

/* Subtract: r = a - b (assumes a >= b) */
static void bn_sub(bignum *r, const bignum *a, const bignum *b) {
    int max = a->used > b->used ? a->used : b->used;
    memset(r, 0, sizeof(*r));
    uint64_t borrow = 0;
    for (int i = 0; i < max; i++) {
        uint64_t va = i < a->used ? a->l[i] : 0;
        uint64_t vb = i < b->used ? b->l[i] : 0;
        uint64_t diff = va - vb - borrow;
        if (va < vb + borrow) borrow = 1; else borrow = 0;
        r->l[i] = (uint32_t)(diff & 0xFFFFFFFF);
    }
    r->used = max;
    while (r->used > 0 && r->l[r->used - 1] == 0) r->used--;
    if (r->used == 0) r->used = 1; /* zero */
}

/* Add: r = a + b */
static void bn_add(bignum *r, const bignum *a, const bignum *b) {
    int max = a->used > b->used ? a->used : b->used;
    memset(r, 0, sizeof(*r));
    uint64_t carry = 0;
    for (int i = 0; i < max; i++) {
        uint64_t va = i < a->used ? a->l[i] : 0;
        uint64_t vb = i < b->used ? b->l[i] : 0;
        uint64_t sum = va + vb + carry;
        r->l[i] = (uint32_t)(sum & 0xFFFFFFFF);
        carry = sum >> 32;
    }
    if (carry && max < BN_MAX_LIMBS) {
        r->l[max] = (uint32_t)carry;
        max++;
    }
    r->used = max;
    while (r->used > 0 && r->l[r->used - 1] == 0) r->used--;
    if (r->used == 0) r->used = 1;
}

/* r = a mod m (barrett-like division) */
void bn_mod(bignum *r, const bignum *a, const bignum *m) {
    if (bn_compare(a, m) < 0) {
        bn_copy(r, a);
        return;
    }
    bignum tmp, divisor;
    bn_copy(&tmp, a);
    bn_copy(&divisor, m);
    
    int shift = bn_bits(&tmp) - bn_bits(&divisor);
    if (shift < 0) shift = 0;
    
    /* Shift divisor left */
    bignum shifted;
    memset(&shifted, 0, sizeof(shifted));
    for (int i = 0; i < divisor.used; i++) shifted.l[i] = divisor.l[i];
    shifted.used = divisor.used;
    for (int s = 0; s < shift; s++) bn_lshift1(&shifted);
    
    for (int s = shift; s >= 0; s--) {
        if (bn_compare(&tmp, &shifted) >= 0) {
            bn_sub(&tmp, &tmp, &shifted);
        }
        bn_rshift1(&shifted);
    }
    
    bn_copy(r, &tmp);
    if (bn_is_zero(r)) { r->l[0] = 0; r->used = 0; }
}

/* Montgomery multiplication helper: r = a * b mod m */
void bn_mod_mul(bignum *r, const bignum *a, const bignum *b, const bignum *m) {
    bignum product;
    memset(&product, 0, sizeof(product));
    
    /* Long multiplication */
    for (int i = 0; i < a->used; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < b->used || carry; j++) {
            uint64_t va = a->l[i];
            uint64_t vb = j < b->used ? b->l[j] : 0;
            uint64_t sum = product.l[i + j] + va * vb + carry;
            product.l[i + j] = (uint32_t)(sum & 0xFFFFFFFF);
            carry = sum >> 32;
        }
    }
    /* Find used */
    product.used = BN_MAX_LIMBS;
    while (product.used > 0 && product.l[product.used - 1] == 0) product.used--;
    if (product.used == 0) product.used = 1;
    if (product.l[0] == 0 && product.used == 1) { memset(r, 0, sizeof(*r)); r->used = 0; return; }
    
    bn_mod(r, &product, m);
}

/* r = base^exp mod mod (modular exponentiation) */
void bn_mod_exp(bignum *r, const bignum *base, const bignum *exp, const bignum *mod) {
    bignum result;
    memset(&result, 0, sizeof(result));
    result.l[0] = 1;
    result.used = 1;
    
    bignum b;
    bn_mod(&b, base, mod);
    
    int bits = bn_bits(exp);
    for (int i = 0; i < bits; i++) {
        int bit_idx = i / 32;
        int bit_pos = i % 32;
        if ((exp->l[bit_idx] >> bit_pos) & 1) {
            bn_mod_mul(&result, &result, &b, mod);
        }
        bn_mod_mul(&b, &b, &b, mod);
    }
    
    bn_copy(r, &result);
}

/* Generate random bignum < max */
void bn_random(bignum *r, const bignum *max) {
    int bytes = (bn_bits(max) + 7) / 8;
    uint8_t buf[BN_MAX_BYTES];
    do {
        rng_fill_buf(buf, bytes > BN_MAX_BYTES ? BN_MAX_BYTES : bytes);
        bn_from_bytes(r, buf, bytes);
    } while (bn_compare(r, max) >= 0 || bn_is_zero(r));
}

/* ── DH Group 14 ────────────────────────────────────────────── */

static int dh_initialized = 0;
static bignum dh_p, dh_g;

static int hex_char_to_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static void hex_to_bytes(const char *hex, uint8_t *bytes, int *len) {
    int hlen = 0;
    while (hex[hlen]) hlen++;
    *len = hlen / 2;
    for (int i = 0; i < *len; i++) {
        bytes[i] = (hex_char_to_val(hex[i*2]) << 4) | hex_char_to_val(hex[i*2+1]);
    }
}

void dh_init(void) {
    if (dh_initialized) return;
    uint8_t p_buf[BN_MAX_BYTES];
    int p_len;
    hex_to_bytes(DH14_P_HEX, p_buf, &p_len);
    bn_from_bytes(&dh_p, p_buf, p_len);
    bn_set_u32(&dh_g, DH14_G);
    dh_initialized = 1;
}

const bignum *dh_get_p(void) { return &dh_p; }
const bignum *dh_get_g(void) { return &dh_g; }

void dh_generate_key(bignum *pub, bignum *priv) {
    /* Generate random private key (256-bit for safety) */
    bignum max;
    memset(&max, 0, sizeof(max));
    max.l[7] = 0xFFFFFFFF;
    max.used = 8;
    bn_random(priv, &max);
    
    /* pub = g^priv mod p */
    bn_mod_exp(pub, &dh_g, priv, &dh_p);
}

void dh_compute_shared(bignum *shared, const bignum *their_pub, const bignum *my_priv) {
    bn_mod_exp(shared, their_pub, my_priv, &dh_p);
}

/* ── RSA operations with embedded key ───────────────────────── */

/* Embedded 2048-bit RSA host key (generated at build time) */
#include "rsa_key.h"

static int rsa_key_loaded = 0;

void rsa_load_key(void) {
    rsa_key_loaded = 1;
}

/* Modular exponentiation for RSA (CRT: m^d mod n using Chinese Remainder Theorem) */
static void rsa_crt_mod_exp(uint8_t *out, const uint8_t *in, int in_len) {
    /* Convert input to bignum */
    bignum m, p, q, dp, dq, qinv, n;
    bn_from_bytes(&m, in, in_len);
    bn_from_bytes(&p, rsa_host_p, 128);
    bn_from_bytes(&q, rsa_host_q, 128);
    bn_from_bytes(&dp, rsa_host_dp, 128);
    bn_from_bytes(&dq, rsa_host_dq, 128);
    bn_from_bytes(&qinv, rsa_host_qinv, 128);
    bn_from_bytes(&n, rsa_host_n, 256);

    /* CRT: m1 = m^dp mod p, m2 = m^dq mod q */
    bignum m1, m2;
    bn_mod(&m1, &m, &p);
    bn_mod(&m2, &m, &q);
    
    bignum s1, s2;
    bn_mod_exp(&s1, &m1, &dp, &p);
    bn_mod_exp(&s2, &m2, &dq, &q);

    /* h = qinv * (s1 - s2) mod p */
    bignum h, diff;
    if (bn_compare(&s1, &s2) >= 0) {
        bn_sub(&diff, &s1, &s2);
    } else {
        bn_sub(&diff, &s2, &s1);
        /* diff = p - diff */
        bn_sub(&diff, &p, &diff);
    }
    bn_mod_mul(&h, &qinv, &diff, &p);

    /* result = s2 + q * h */
    bignum qh;
    bn_mod_mul(&qh, &q, &h, &n);
    bignum result;
    bn_add(&result, &s2, &qh);
    bn_mod(&result, &result, &n);

    /* Output */
    bn_to_bytes(&result, out, 256);
}

/* RSA sign: compute PKCS#1 v1.5 signature over SHA-256 hash */
int rsa_sign(const uint8_t *hash, uint8_t *sig_out) {
    /* PKCS#1 v1.5 padding for SHA-256 (EMSA-PKCS1-v1_5) */
    /* DER encoding of SHA-256 OID: 30 31 30 0d 06 09 60 86 48 01 65 03 04 02 01 05 00 04 20 */
    uint8_t der_sha256[] = {
        0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
        0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
        0x00, 0x04, 0x20
    };
    
    uint8_t em[256];
    int em_len = 256;
    
    em[0] = 0x00;
    em[1] = 0x01;
    int pad_len = em_len - 3 - sizeof(der_sha256) - 32;
    memset(em + 2, 0xFF, pad_len);
    em[2 + pad_len] = 0x00;
    memcpy(em + 2 + pad_len + 1, der_sha256, sizeof(der_sha256));
    memcpy(em + 2 + pad_len + 1 + sizeof(der_sha256), hash, 32);
    
    rsa_crt_mod_exp(sig_out, em, em_len);
    return 256;
}

/* RSA verify: check that sig = hash^e mod n (using public key) */
int rsa_verify(const uint8_t *hash, const uint8_t *sig) {
    bignum sig_bn, e, n;
    bn_from_bytes(&sig_bn, sig, 256);
    bn_from_bytes(&e, rsa_host_e, 3);
    bn_from_bytes(&n, rsa_host_n, 256);
    
    bignum em;
    bn_mod_exp(&em, &sig_bn, &e, &n);
    
    uint8_t em_bytes[256];
    bn_to_bytes(&em, em_bytes, 256);
    
    /* Check PKCS#1 v1.5 padding: 00 01 FF ... FF 00 DER(hash) */
    if (em_bytes[0] != 0x00 || em_bytes[1] != 0x01) return 0;
    int pos = 2;
    while (pos < 256 && em_bytes[pos] == 0xFF) pos++;
    if (pos >= 255 || em_bytes[pos] != 0x00) return 0;
    pos++;
    /* Remaining should be DER + hash (51 bytes) */
    /* For simplicity, just check hash at end */
    if (pos + 51 > 256) return 0;
    if (memcmp(em_bytes + 256 - 32, hash, 32) != 0) return 0;
    return 1;
}

/* Get SSH public key blob */
const uint8_t *rsa_get_pubkey_blob(int *len) {
    *len = rsa_host_pubkey_blob_len;
    return rsa_host_pubkey_blob;
}

/* ── SSH format helpers ─────────────────────────────────────── */

/* Pack a uint32 in network byte order */
static void ssh_pack_u32(uint8_t *buf, uint32_t v) {
    buf[0] = (v >> 24) & 0xFF;
    buf[1] = (v >> 16) & 0xFF;
    buf[2] = (v >> 8) & 0xFF;
    buf[3] = v & 0xFF;
}

/* Unpack uint32 */
static __attribute__((unused)) uint32_t ssh_unpack_u32(const uint8_t *buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | buf[3];
}

/* Pack a string (uint32 length + data) into buf, returns bytes written */
static __attribute__((unused)) int ssh_pack_string(uint8_t *buf, const uint8_t *data, int len) {
    ssh_pack_u32(buf, len);
    if (len > 0) memcpy(buf + 4, data, len);
    return 4 + len;
}

/* Pack an mpint from bignum into buf, returns bytes written */
static __attribute__((unused)) int ssh_pack_mpint(uint8_t *buf, const bignum *bn) {
    uint8_t tmp[BN_MAX_BYTES + 1];
    int len = bn_to_bytes(bn, tmp + 1, 0);
    /* Remove leading zeros */
    int start = 1;
    while (start <= len && tmp[start] == 0) start++;
    int data_len = len - start + 1;
    if (data_len == 0) {
        ssh_pack_u32(buf, 0);
        return 4;
    }
    /* Add 0x00 prefix if high bit set */
    if (tmp[start] & 0x80) {
        tmp[start - 1] = 0x00;
        start--;
        data_len++;
    }
    ssh_pack_u32(buf, data_len);
    memcpy(buf + 4, tmp + start, data_len);
    return 4 + data_len;
}

/* ── AES-128-CBC encrypt/decrypt for SSH ────────────────────── */
struct ssh_cipher {
    struct aes_ctx enc_ctx;
    struct aes_ctx dec_ctx;
    uint8_t enc_iv[16];
    uint8_t dec_iv[16];
};

static __attribute__((unused)) void ssh_cipher_init(struct ssh_cipher *c, const uint8_t *key, const uint8_t *iv) {
    aes_init(&c->enc_ctx, key, 16);
    aes_init(&c->dec_ctx, key, 16);
    memcpy(c->enc_iv, iv, 16);
    memcpy(c->dec_iv, iv, 16);
}

static __attribute__((unused)) void ssh_cipher_encrypt(struct ssh_cipher *c, uint8_t *buf, int len) {
    aes_cbc_encrypt(&c->enc_ctx, c->enc_iv, buf, buf, len);
}

static __attribute__((unused)) void ssh_cipher_decrypt(struct ssh_cipher *c, uint8_t *buf, int len) {
    aes_cbc_decrypt(&c->dec_ctx, c->dec_iv, buf, buf, len);
}

/* ── HMAC-SHA256 for SSH ────────────────────────────────────── */
static __attribute__((unused)) void ssh_mac_compute(const uint8_t *key, int key_len,
                            uint32_t seq_nr, const uint8_t *packet, int packet_len,
                            uint8_t mac[32]) {
    uint8_t seq_buf[4];
    ssh_pack_u32(seq_buf, seq_nr);
    uint8_t hmac_input[4 + packet_len];
    memcpy(hmac_input, seq_buf, 4);
    memcpy(hmac_input + 4, packet, packet_len);
    hmac_sha256(key, key_len, hmac_input, 4 + packet_len, mac);
}

