#ifndef AES_H
#define AES_H

#include "types.h"

#define AES_BLOCK_SIZE 16
#define AES_MAX_KEY_SIZE 32
#define AES_MAX_ROUNDS 14

/* AES key sizes */
#define AES_128 16
#define AES_192 24
#define AES_256 32

struct aes_ctx {
    uint32_t ek[4 * (AES_MAX_ROUNDS + 1)]; /* encryption round keys */
    uint32_t dk[4 * (AES_MAX_ROUNDS + 1)]; /* decryption round keys */
    int rounds;                             /* number of rounds (10/12/14) */
    int key_len;                            /* key length in bytes */
};

/**
 * aes_init - initialize AES context with a key
 * @ctx:     AES context
 * @key:     key bytes
 * @key_len: key length (16, 24, or 32 bytes)
 * Returns: 0 on success, -EINVAL on invalid key length
 */
int aes_init(struct aes_ctx *ctx, const uint8_t *key, int key_len);

/**
 * aes_encrypt_block - encrypt a single 16-byte block
 * @ctx: AES context
 * @in:  input plaintext block (16 bytes)
 * @out: output ciphertext block (16 bytes)
 */
void aes_encrypt_block(const struct aes_ctx *ctx,
                       const uint8_t in[AES_BLOCK_SIZE],
                       uint8_t out[AES_BLOCK_SIZE]);

/**
 * aes_decrypt_block - decrypt a single 16-byte block
 * @ctx: AES context
 * @in:  input ciphertext block (16 bytes)
 * @out: output plaintext block (16 bytes)
 */
void aes_decrypt_block(const struct aes_ctx *ctx,
                       const uint8_t in[AES_BLOCK_SIZE],
                       uint8_t out[AES_BLOCK_SIZE]);

/**
 * aes_cbc_encrypt - encrypt data in CBC mode
 * @ctx: AES context
 * @iv:  initialization vector (16 bytes, read/written)
 * @in:  input plaintext
 * @out: output ciphertext
 * @len: length of data (must be multiple of AES_BLOCK_SIZE)
 */
void aes_cbc_encrypt(const struct aes_ctx *ctx, uint8_t iv[AES_BLOCK_SIZE],
                     const uint8_t *in, uint8_t *out, size_t len);

/**
 * aes_cbc_decrypt - decrypt data in CBC mode
 * @ctx: AES context
 * @iv:  initialization vector (16 bytes, read/written)
 * @in:  input ciphertext
 * @out: output plaintext
 * @len: length of data (must be multiple of AES_BLOCK_SIZE)
 */
void aes_cbc_decrypt(const struct aes_ctx *ctx, uint8_t iv[AES_BLOCK_SIZE],
                     const uint8_t *in, uint8_t *out, size_t len);

/* Initialize AES module */
void aes_init_crypto(void);

#endif /* AES_H */
