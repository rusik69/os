#ifndef CRYPTO_H
#define CRYPTO_H
#include "types.h"
void crypto_init(void);
void crypto_aes_set_key(const uint8_t *key);
void crypto_aes_encrypt(const uint8_t in[16], uint8_t out[16]);
void crypto_aes_decrypt(const uint8_t in[16], uint8_t out[16]);
#endif
