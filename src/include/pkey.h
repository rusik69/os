#ifndef PKEY_H
#define PKEY_H

#include "types.h"

/* ── Protection key definitions ────────────────────────────────────── */

/* Maximum number of protection keys (x86-64 supports up to 16) */
#define PKEY_MAX 16

/* Protection key rights (for pkey_alloc rights parameter) */
#define PKEY_DISABLE_ACCESS  0x1  /* Disable all access to pages with this key */
#define PKEY_DISABLE_WRITE   0x2  /* Disable write access */

/* ── Public API ────────────────────────────────────────────────────── */

/**
 * pkey_alloc — Allocate a protection key.
 * @flags:  Flags (currently unused, pass 0).
 * @rights: Initial rights (0 = full access, or PKEY_DISABLE_ACCESS,
 *          PKEY_DISABLE_WRITE, or both).
 *
 * Returns the allocated pkey number (0..15) on success, or -1 if no
 * keys are available or PKU is unsupported.
 */
int pkey_alloc(unsigned int flags, unsigned int rights);

/**
 * pkey_free — Release a previously allocated protection key.
 * @pkey: Pkey number to free.
 *
 * Returns 0 on success, -1 if the pkey is invalid or not allocated.
 */
int pkey_free(int pkey);

/**
 * pkey_mprotect — Set protection key on a memory mapping.
 * @addr:  Start address (must be page-aligned).
 * @len:   Length of the region.
 * @prot:  Memory protection flags (PROT_READ, PROT_WRITE, PROT_EXEC).
 * @pkey:  Protection key number (0..15), or -1 to clear.
 *
 * Returns 0 on success, -1 on error.
 */
int pkey_mprotect(void *addr, size_t len, int prot, int pkey);

/**
 * pkey_set_rights — Set protection key rights (via PKRU MSR).
 * @pkey:   Protection key number.
 * @rights: 0 = full access, PKEY_DISABLE_ACCESS, PKEY_DISABLE_WRITE, or both.
 *
 * Returns 0 on success, -1 if PKU is not available.
 */
int pkey_set_rights(int pkey, unsigned int rights);

/**
 * pkey_get_rights — Get protection key rights.
 * @pkey: Protection key number.
 *
 * Returns the current rights, or -1 if PKU is not available.
 */
int pkey_get_rights(int pkey);

/**
 * pkey_pku_available — Check if PKU (Protection Key Unit) is supported.
 *
 * Returns 1 if PKU is available, 0 otherwise.
 */
int pkey_pku_available(void);

#endif /* PKEY_H */
