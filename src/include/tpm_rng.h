#ifndef TPM_RNG_H
#define TPM_RNG_H

#include "types.h"

/**
 * tpm_rng_init — Seed kernel entropy pool from TPM RNG.
 *
 * Reads 64 bytes from the TPM via tpm2_get_random() and feeds them
 * into the kernel entropy pool via rng_add_entropy().  This improves
 * the quality of the kernel's PRNG by adding hardware-generated
 * entropy early in the boot process.
 *
 * Must be called after tpm_init() and rng_init() have completed.
 * Returns 0 on success, negative on error (no TPM, TPM error, etc.).
 */
int tpm_rng_init(void);

#endif /* TPM_RNG_H */
