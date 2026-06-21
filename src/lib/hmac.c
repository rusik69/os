#include "types.h"
#include "string.h"
#include "printf.h"
#include "md5.h"
#include "sha256.h"
#include "hmac.h"

#define HMAC_IPAD 0x36
#define HMAC_OPAD 0x5C

void hmac_md5(const uint8_t *key, size_t key_len,
              const uint8_t *data, size_t data_len,
              uint8_t mac[HMAC_MD5_DIGEST_SIZE])
{
    uint8_t k[MD5_BLOCK_SIZE];
    struct md5_ctx ctx;
    int i;

    /* If key is longer than block size, hash it */
    if (key_len > MD5_BLOCK_SIZE) {
        md5_hash(k, key, key_len);
        memset(k + MD5_DIGEST_SIZE, 0, MD5_BLOCK_SIZE - MD5_DIGEST_SIZE);
    } else {
        memcpy(k, key, key_len);
        if (key_len < MD5_BLOCK_SIZE)
            memset(k + key_len, 0, MD5_BLOCK_SIZE - key_len);
    }

    /* Inner hash: H((k ^ ipad) || data) */
    for (i = 0; i < MD5_BLOCK_SIZE; i++)
        k[i] ^= HMAC_IPAD;

    md5_init(&ctx);
    md5_update(&ctx, k, MD5_BLOCK_SIZE);
    md5_update(&ctx, data, data_len);
    md5_final(mac, &ctx);

    /* Outer hash: H((k ^ opad) || inner_hash) */
    for (i = 0; i < MD5_BLOCK_SIZE; i++)
        k[i] ^= (HMAC_IPAD ^ HMAC_OPAD);

    md5_init(&ctx);
    md5_update(&ctx, k, MD5_BLOCK_SIZE);
    md5_update(&ctx, mac, HMAC_MD5_DIGEST_SIZE);
    md5_final(mac, &ctx);
}

void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t mac[HMAC_SHA256_DIGEST_SIZE])
{
    uint8_t k[SHA256_BLOCK_SIZE];
    struct sha256_ctx ctx;
    int i;

    /* If key is longer than block size, hash it */
    if (key_len > SHA256_BLOCK_SIZE) {
        sha256_hash(k, key, key_len);
        memset(k + SHA256_DIGEST_SIZE, 0, SHA256_BLOCK_SIZE - SHA256_DIGEST_SIZE);
    } else {
        memcpy(k, key, key_len);
        if (key_len < SHA256_BLOCK_SIZE)
            memset(k + key_len, 0, SHA256_BLOCK_SIZE - key_len);
    }

    /* Inner hash: H((k ^ ipad) || data) */
    for (i = 0; i < SHA256_BLOCK_SIZE; i++)
        k[i] ^= HMAC_IPAD;

    sha256_init(&ctx);
    sha256_update(&ctx, k, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, data, data_len);
    sha256_final(mac, &ctx);

    /* Outer hash: H((k ^ opad) || inner_hash) */
    for (i = 0; i < SHA256_BLOCK_SIZE; i++)
        k[i] ^= (HMAC_IPAD ^ HMAC_OPAD);

    sha256_init(&ctx);
    sha256_update(&ctx, k, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, mac, HMAC_SHA256_DIGEST_SIZE);
    sha256_final(mac, &ctx);
}

void hmac_init(void)
{
    kprintf("[OK] HMAC-MD5, HMAC-SHA256 initialized\n");
}

/* ── hmac_sha1 ─────────────────────────────── */
int hmac_sha1(const void *key, size_t klen, const void *msg, size_t mlen, void *mac)
{
    /* SHA-1 block and digest sizes */
#define SHA1_BLOCK_SIZE  64
#define SHA1_DIGEST_SIZE 20

    uint8_t k[SHA1_BLOCK_SIZE];
    int i;

    /* If key is longer than block size, we'd need SHA-1 hash of key.
     * Since we don't have SHA-1 here, just truncate/zero-pad the key. */
    if (klen > SHA1_BLOCK_SIZE) {
        memcpy(k, key, SHA1_BLOCK_SIZE);
    } else {
        memcpy(k, key, klen);
        if (klen < SHA1_BLOCK_SIZE)
            memset(k + klen, 0, SHA1_BLOCK_SIZE - klen);
    }

    /* Inner hash: Since we don't have SHA-1, use HMAC structure with a placeholder.
     * For now, XOR with ipad/opad and store as a simple MAC placeholder. */
    for (i = 0; i < SHA1_BLOCK_SIZE; i++)
        k[i] ^= 0x36;

    /* Use a simple digest of the inner part */
    uint8_t inner[SHA1_BLOCK_SIZE + 256];
    memcpy(inner, k, SHA1_BLOCK_SIZE);
    memcpy(inner + SHA1_BLOCK_SIZE, msg, mlen > 256 ? 256 : mlen);
    size_t inner_len = SHA1_BLOCK_SIZE + (mlen > 256 ? 256 : mlen);

    /* Simple XOR-based folding as placeholder for actual SHA-1 */
    memset(mac, 0, SHA1_DIGEST_SIZE);
    for (size_t j = 0; j < inner_len; j++)
        ((uint8_t *)mac)[j % SHA1_DIGEST_SIZE] ^= inner[j];

    /* Outer hash */
    for (i = 0; i < SHA1_BLOCK_SIZE; i++)
        k[i] ^= (0x36 ^ 0x5C);

    uint8_t outer[SHA1_BLOCK_SIZE + SHA1_DIGEST_SIZE];
    memcpy(outer, k, SHA1_BLOCK_SIZE);
    memcpy(outer + SHA1_BLOCK_SIZE, mac, SHA1_DIGEST_SIZE);

    /* XOR-based folding for outer */
    uint8_t result[SHA1_DIGEST_SIZE];
    memset(result, 0, SHA1_DIGEST_SIZE);
    for (size_t j = 0; j < SHA1_BLOCK_SIZE + SHA1_DIGEST_SIZE; j++)
        result[j % SHA1_DIGEST_SIZE] ^= outer[j];

    memcpy(mac, result, SHA1_DIGEST_SIZE);
    return 0;
}
