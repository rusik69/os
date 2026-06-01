#ifndef HMAC_H
#define HMAC_H

#include "types.h"

#define HMAC_MD5_DIGEST_SIZE  16
#define HMAC_SHA256_DIGEST_SIZE 32

/**
 * hmac_md5 - compute HMAC-MD5
 * @key:    HMAC key
 * @key_len: length of key in bytes
 * @data:   input data
 * @data_len: length of input data in bytes
 * @mac:    output buffer (HMAC_MD5_DIGEST_SIZE bytes)
 */
void hmac_md5(const uint8_t *key, size_t key_len,
              const uint8_t *data, size_t data_len,
              uint8_t mac[HMAC_MD5_DIGEST_SIZE]);

/**
 * hmac_sha256 - compute HMAC-SHA256
 * @key:    HMAC key
 * @key_len: length of key in bytes
 * @data:   input data
 * @data_len: length of input data in bytes
 * @mac:    output buffer (HMAC_SHA256_DIGEST_SIZE bytes)
 */
void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t mac[HMAC_SHA256_DIGEST_SIZE]);

/* Initialize HMAC module */
void hmac_init(void);

#endif /* HMAC_H */
