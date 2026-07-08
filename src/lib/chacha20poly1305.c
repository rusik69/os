// SPDX-License-Identifier: GPL-2.0-only
/*
 * ChaCha20-Poly1305 AEAD construction (RFC 8439)
 *
 * Authenticated encryption with associated data.
 */
#include "types.h"
#include "string.h"
#include "printf.h"

/* Our implementations (declared here since no header yet) */
extern void lib_chacha20_encrypt(uint8_t *out, const uint8_t *in, size_t len,
                             const uint8_t key[32], const uint8_t nonce[12],
                             uint64_t counter);
extern void poly1305_mac(uint8_t mac[16], const uint8_t *data, size_t len,
                         const uint8_t key[32]);

#define CHACHA20POLY1305_KEY_SIZE    32
#define CHACHA20POLY1305_NONCE_SIZE  12
#define CHACHA20POLY1305_MAC_SIZE    16
#define CHACHA20POLY1305_TAG_SIZE    16

/* Generate Poly1305 one-time key from ChaCha20 keystream */
static void chacha20poly1305_poly_keygen(uint8_t poly_key[32],
                                          const uint8_t key[32],
                                          const uint8_t nonce[12])
{
    uint8_t block[64] = {0};
    lib_chacha20_encrypt(block, block, 64, key, nonce, 0);
    memcpy(poly_key, block, 32);
}

/* Pad data to 16 bytes */
static size_t chacha20poly1305_pad16(size_t len)
{
    return (16 - (len & 15)) & 15;
}

static int lib_chacha20poly1305_encrypt(uint8_t *ciphertext, uint8_t mac[16],
                              const uint8_t *plaintext, size_t pt_len,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t key[32], const uint8_t nonce[12])
{
    uint8_t poly_key[32];
    struct {
        uint64_t len;
        uint64_t len2;
    } padding_block;
    uint8_t tag_key[32];
    size_t i;

    if (!key || !nonce || !ciphertext || !mac)
        return -1;
    if (!plaintext && pt_len > 0)
        return -1;

    /* Generate Poly1305 key from ChaCha20 with counter=0 */
    chacha20poly1305_poly_keygen(poly_key, key, nonce);

    /* Encrypt plaintext with ChaCha20 starting at counter=1 */
    lib_chacha20_encrypt(ciphertext, plaintext, pt_len, key, nonce, 1);

    /* Compute Poly1305 MAC over AAD || ciphertext || padding */
    {
        uint8_t mac_data[4096];
        size_t mac_len = 0;
        size_t aad_pad = chacha20poly1305_pad16(aad_len);
        size_t ct_pad = chacha20poly1305_pad16(pt_len);

        if (aad_len + aad_pad + pt_len + ct_pad + 16 > sizeof(mac_data))
            return -1;

        if (aad_len > 0) {
            memcpy(mac_data, aad, aad_len);
            mac_len += aad_len;
        }
        /* AAD padding */
        if (aad_pad > 0) {
            memset(mac_data + mac_len, 0, aad_pad);
            mac_len += aad_pad;
        }
        if (pt_len > 0) {
            memcpy(mac_data + mac_len, ciphertext, pt_len);
            mac_len += pt_len;
        }
        /* Ciphertext padding */
        if (ct_pad > 0) {
            memset(mac_data + mac_len, 0, ct_pad);
            mac_len += ct_pad;
        }
        /* Length block */
        padding_block.len = (uint64_t)aad_len;
        padding_block.len2 = (uint64_t)pt_len;
        memcpy(mac_data + mac_len, &padding_block, 16);
        mac_len += 16;

        poly1305_mac(mac, mac_data, mac_len, poly_key);
    }

    return 0;
}

static int lib_chacha20poly1305_decrypt(uint8_t *plaintext, const uint8_t *ciphertext,
                              size_t ct_len, const uint8_t mac[16],
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t key[32], const uint8_t nonce[12])
{
    uint8_t poly_key[32];
    uint8_t computed_mac[16];
    struct {
        uint64_t len;
        uint64_t len2;
    } padding_block;
    uint8_t mac_data[4096];
    size_t mac_len = 0;
    size_t aad_pad = chacha20poly1305_pad16(aad_len);
    size_t ct_pad = chacha20poly1305_pad16(ct_len);
    size_t i;

    if (!key || !nonce || !ciphertext || !mac || !plaintext)
        return -1;

    /* Generate Poly1305 key */
    chacha20poly1305_poly_keygen(poly_key, key, nonce);

    /* Verify MAC first */
    if (aad_len > 0) {
        memcpy(mac_data, aad, aad_len);
        mac_len += aad_len;
    }
    if (aad_pad > 0) {
        memset(mac_data + mac_len, 0, aad_pad);
        mac_len += aad_pad;
    }
    if (ct_len > 0) {
        memcpy(mac_data + mac_len, ciphertext, ct_len);
        mac_len += ct_len;
    }
    if (ct_pad > 0) {
        memset(mac_data + mac_len, 0, ct_pad);
        mac_len += ct_pad;
    }
    padding_block.len = (uint64_t)aad_len;
    padding_block.len2 = (uint64_t)ct_len;
    memcpy(mac_data + mac_len, &padding_block, 16);
    mac_len += 16;

    poly1305_mac(computed_mac, mac_data, mac_len, poly_key);

    /* Constant-time MAC comparison */
    uint8_t diff = 0;
    for (i = 0; i < 16; i++)
        diff |= mac[i] ^ computed_mac[i];
    if (diff != 0)
        return -1; /* MAC mismatch */

    /* Decrypt */
    lib_chacha20_encrypt(plaintext, ciphertext, ct_len, key, nonce, 1);
    return 0;
}

/* ── chacha20poly1305_encrypt ─────────────────────────────── */
int chacha20poly1305_encrypt(const void *key, const void *nonce, const void *aad, size_t aad_len, const void *plain, size_t plen, void *cipher, void *tag)
{
    if (!key || !nonce || !cipher || !tag)
        return -1;
    return lib_chacha20poly1305_encrypt((uint8_t *)cipher, (uint8_t *)tag,
                                         (const uint8_t *)plain, plen,
                                         (const uint8_t *)aad, aad_len,
                                         (const uint8_t *)key, (const uint8_t *)nonce);
}
/* ── chacha20poly1305_decrypt ─────────────────────────────── */
int chacha20poly1305_decrypt(const void *key, const void *nonce, const void *aad, size_t aad_len, const void *cipher, size_t clen, const void *tag, void *plain)
{
    if (!key || !nonce || !cipher || !tag || !plain)
        return -1;
    return lib_chacha20poly1305_decrypt((uint8_t *)plain, (const uint8_t *)cipher, clen,
                                         (const uint8_t *)tag,
                                         (const uint8_t *)aad, aad_len,
                                         (const uint8_t *)key, (const uint8_t *)nonce);
}
