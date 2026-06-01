#ifndef SHA512_H
#define SHA512_H

#include "types.h"

#define SHA512_DIGEST_SIZE 64
#define SHA512_BLOCK_SIZE  128

struct sha512_ctx {
    uint64_t count[2];
    uint64_t state[8];
    uint8_t  buffer[SHA512_BLOCK_SIZE];
};

/**
 * sha512_init - initialize SHA-512 context
 */
void sha512_init(struct sha512_ctx *ctx);

/**
 * sha512_update - feed data into the hash
 * @ctx: SHA-512 context
 * @data: input data
 * @len: length of data
 */
void sha512_update(struct sha512_ctx *ctx, const void *data, size_t len);

/**
 * sha512_final - finalize hash and write digest
 * @digest: output buffer (SHA512_DIGEST_SIZE bytes)
 * @ctx: SHA-512 context
 */
void sha512_final(uint8_t digest[SHA512_DIGEST_SIZE],
                  struct sha512_ctx *ctx);

/**
 * sha512_hash - all-in-one SHA-512 hash
 * @digest: output buffer (SHA512_DIGEST_SIZE bytes)
 * @data:   input data
 * @len:    length of data
 */
void sha512_hash(uint8_t digest[SHA512_DIGEST_SIZE],
                 const void *data, size_t len);

/* Initialize SHA-512 module */
void sha512_init_crypto(void);

#endif /* SHA512_H */
