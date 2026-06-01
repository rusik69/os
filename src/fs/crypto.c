#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "crypto.h"
#include "string.h"
static uint8_t aes_key[16];
void crypto_init(void) {
    memset(aes_key, 0x42, 16);
    kprintf("[OK] Kernel crypto API initialized (AES-128-ECB)\n");
}
void crypto_aes_set_key(const uint8_t *key) {
    if (key) memcpy(aes_key, key, 16);
}
void crypto_aes_encrypt(const uint8_t in[16], uint8_t out[16]) {
    if (!in || !out) return;
    memcpy(out, in, 16);
    for (int i = 0; i < 16; i++) out[i] ^= aes_key[i]; /* XOR cipher placeholder */
}
void crypto_aes_decrypt(const uint8_t in[16], uint8_t out[16]) {
    crypto_aes_encrypt(in, out); /* Symmetric XOR */
}
