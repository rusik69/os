#ifndef SHA256_H
#define SHA256_H

#include "types.h"

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE  64

struct sha256_ctx {
    uint64_t count;
    uint32_t state[8];
    uint8_t  buffer[SHA256_BLOCK_SIZE];
};

/**
 * sha256_init - initialize SHA-256 context
 */
void sha256_init(struct sha256_ctx *ctx);

/**
 * sha256_update - feed data into the hash
 * @ctx: SHA-256 context
 * @data: input data
 * @len: length of data
 */
void sha256_update(struct sha256_ctx *ctx, const void *data, size_t len);

/**
 * sha256_final - finalize hash and write digest
 * @digest: output buffer (SHA256_DIGEST_SIZE bytes)
 * @ctx: SHA-256 context
 */
void sha256_final(uint8_t digest[SHA256_DIGEST_SIZE],
                  struct sha256_ctx *ctx);

/**
 * sha256_hash - all-in-one SHA-256 hash
 * @digest: output buffer (SHA256_DIGEST_SIZE bytes)
 * @data:   input data
 * @len:    length of data
 */
void sha256_hash(uint8_t digest[SHA256_DIGEST_SIZE],
                 const void *data, size_t len);

/* Initialize SHA-256 module */
void sha256_init_crypto(void);

#endif /* SHA256_H */
