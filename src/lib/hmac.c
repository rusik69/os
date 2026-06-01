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
