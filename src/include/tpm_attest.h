#ifndef TPM_ATTEST_H
#define TPM_ATTEST_H

/*
 * tpm_attest.h — TPM 2.0 remote attestation support
 *
 * Provides TPM quoting and verification for remote attestation.
 * Uses TPM2_Quote (TPM_CC_Quote = 0x00000158) to generate signed
 * PCR values, and RSA-based verification to validate quotes.
 *
 * AIK (Attestation Identity Key) is stored in TPM NVRAM if available.
 * Exposes /sys/kernel/security/tpm_attest/ for userspace interaction.
 */

#include "types.h"

/* ── Constants ─────────────────────────────────────────────────────── */

#define TPM_ATTEST_DIGEST_SIZE    32   /* SHA-256 digest */
#define TPM_ATTEST_SIG_SIZE      256   /* RSA-2048 signature */
#define TPM_ATTEST_MAX_NONCE      64   /* max nonce size */
#define TPM_ATTEST_AIK_NV_INDEX  0x01800001  /* NV index for AIK */

/* ── Quote result structure ───────────────────────────────────────── */

struct tpm_attest_quote {
    uint8_t  pcr_value[TPM_ATTEST_DIGEST_SIZE];  /* PCR digest */
    uint8_t  signature[TPM_ATTEST_SIG_SIZE];      /* RSA signature over quote */
    uint32_t sig_size;                             /* actual signature size */
    uint8_t  nonce[TPM_ATTEST_MAX_NONCE];          /* nonce used for quote */
    uint32_t nonce_size;                           /* actual nonce size */
    uint32_t pcr_index;                            /* PCR index quoted */
};

/* ── Public API ───────────────────────────────────────────────────── */

/* Initialize the TPM attestation subsystem */
int tpm_attest_init(void);

/* Generate a TPM quote for the given PCR index using the provided nonce.
 * Returns 0 on success with @quote populated, negative on error. */
int tpm_attest_quote(uint32_t pcr_index, const uint8_t *nonce,
                     uint32_t nonce_len, struct tpm_attest_quote *quote);

/* Verify a TPM quote given the expected PCR value and nonce,
 * using the AIK public key (raw RSA modulus + exponent).
 * Returns 0 on success (quote verified), negative on failure. */
int tpm_attest_verify(const struct tpm_attest_quote *quote,
                      const uint8_t *expected_pcr_value,
                      const uint8_t *nonce, uint32_t nonce_len,
                      const uint8_t *public_key, uint32_t key_len);

/* Store the AIK (Attestation Identity Key) in TPM NVRAM.
 * Returns 0 on success, negative on error. */
int tpm_attest_store_aik(const uint8_t *aik_data, uint32_t aik_len);

/* Load the AIK from TPM NVRAM.
 * Returns number of bytes loaded on success, negative on error. */
int tpm_attest_load_aik(uint8_t *aik_data, uint32_t *aik_len);

/* Get current nonce (for /sys/kernel/security/tpm_attest/nonce read) */
int tpm_attest_get_nonce(uint8_t *buf, uint32_t *len);

/* Set current nonce (for /sys/kernel/security/tpm_attest/nonce write) */
int tpm_attest_set_nonce(const uint8_t *buf, uint32_t len);

/* Get the latest quote as hex string (for /sys read) */
int tpm_attest_get_quote_hex(char *buf, uint32_t buf_size);

/* Hex encoding helper */
void tpm_attest_bin2hex(const uint8_t *bin, uint32_t bin_len, char *hex, uint32_t hex_size);

#endif /* TPM_ATTEST_H */
