/*
 * tpm_rng.c — Seed kernel entropy pool from TPM RNG
 *
 * Uses the TPM 2.0 GetRandom command to obtain 64 bytes of
 * hardware-generated random data and feeds them into the kernel's
 * entropy pool via rng_add_entropy().  This improves the quality
 * of the kernel PRNG by adding a true hardware entropy source.
 */
#define KERNEL_INTERNAL
#include "tpm_rng.h"
#include "tpm.h"
#include "rng.h"
#include "printf.h"

int tpm_rng_init(void)
{
    uint8_t entropy_buf[64];
    int ret;

    /* Read 64 bytes from TPM RNG */
    ret = tpm2_get_random(entropy_buf, sizeof(entropy_buf));
    if (ret < 0) {
        kprintf("[tpm_rng] TPM GetRandom failed (%d) — skipping seed\n", ret);
        return ret;
    }

    /* Feed the entropy into the kernel RNG */
    rng_add_entropy(entropy_buf, (uint32_t)ret);

    kprintf("[OK] tpm_rng: seeded kernel entropy pool with %d bytes from TPM\n", ret);

    /* Wipe the buffer for security */
    for (int i = 0; i < (int)sizeof(entropy_buf); i++)
        entropy_buf[i] = 0;

    return 0;
}
