#ifndef MD5_H
#define MD5_H

#include "types.h"

#define MD5_DIGEST_SIZE 16
#define MD5_BLOCK_SIZE  64

struct md5_ctx {
    uint64_t count;
    uint32_t state[4];
    uint8_t  buffer[MD5_BLOCK_SIZE];
};

/**
 * md5_init - initialize MD5 context
 */
void md5_init(struct md5_ctx *ctx);

/**
 * md5_update - feed data into the hash
 * @ctx: MD5 context
 * @data: input data
 * @len: length of data
 */
void md5_update(struct md5_ctx *ctx, const void *data, size_t len);

/**
 * md5_final - finalize hash and write digest
 * @digest: output buffer (MD5_DIGEST_SIZE bytes)
 * @ctx: MD5 context
 */
void md5_final(uint8_t digest[MD5_DIGEST_SIZE], struct md5_ctx *ctx);

/**
 * md5_hash - all-in-one MD5 hash
 * @digest: output buffer (MD5_DIGEST_SIZE bytes)
 * @data:   input data
 * @len:    length of data
 */
void md5_hash(uint8_t digest[MD5_DIGEST_SIZE],
              const void *data, size_t len);

/* Initialize MD5 module */
void md5_init_crypto(void);

#endif /* MD5_H */
