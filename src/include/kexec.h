#ifndef KEXEC_H
#define KEXEC_H

#include "types.h"

/*
 * kexec — Load and execute a new kernel without firmware reboot
 *
 * Item 362 from the production-improvements plan.
 *
 * Usage:
 *   1. Userspace loads a new kernel ELF into a pre-reserved memory region
 *   2. Calls sys_kexec_load(phys_load_addr, entry_point)
 *   3. Kernel disables interrupts, masks APIC, and jumps to entry_point
 *
 * The caller must ensure:
 *   - The loaded kernel image is fully contained within the reserved region
 *   - Entry point is a 64-bit virtual address (identity-mapped by the new kernel)
 *   - All devices are in a quiescent state suitable for a kexec handover
 *
 * Architecture: x86-64 (long mode, paging enabled on entry)
 */

/* Physical address of the pre-reserved kexec region (contiguous, 16 MB). */
#define KEXEC_RESERVE_PHYS  0x7C000000ULL  /* just below kdump region */
#define KEXEC_REGION_SIZE   (16ULL * 1024 * 1024)  /* 16 MB */

/* Maximum size of a kexec'd kernel image */
#define KEXEC_MAX_IMAGE_SIZE (KEXEC_REGION_SIZE - PAGE_SIZE)

/* Syscall number */
/* (defined in syscall.h: #define SYS_KEXEC_LOAD 778) */

/* ── kexec_load flags ─────────────────────────────────────────────── */
#define KEXEC_FLAG_NONE      0x0000
#define KEXEC_FLAG_PRESERVE_CONTEXT 0x0001  /* preserve hardware state (not yet) */
#define KEXEC_FLAG_DEBUG     0x0002  /* print debug info before jumping */

/* ── Public API ────────────────────────────────────────────────────── */

/*
 * kexec_init — Reserve the kexec physical memory region at boot.
 * Called once during kernel init.  Prints a message on success.
 * Returns 0 on success, -1 if region cannot be reserved.
 */
int kexec_init(void);

/*
 * kexec_load — Validate and record a kexec load request.
 *
 * @phys_addr:  Physical address where the new kernel image resides
 *              (must be within the pre-reserved kexec region).
 * @entry:      Entry point of the new kernel (physical address to jump to).
 * @flags:      KEXEC_FLAG_* flags.
 *
 * Returns 0 on success, negative errno on error.
 */
int kexec_load(uint64_t phys_addr, uint64_t entry, uint32_t flags);

/*
 * kexec_reboot — Jump to the loaded kernel image.
 *
 * Disables interrupts, masks all APIC LVT entries, flushes TLBs,
 * and jumps to the previously loaded entry point in 64-bit long mode.
 *
 * This function does NOT return on success.
 */
void kexec_reboot(void) __attribute__((noreturn));

/* ── Debug / status ─────────────────────────────────────────────────── */
int kexec_is_loaded(void);
uint64_t kexec_get_entry(void);
uint64_t kexec_get_phys_addr(void);

#endif /* KEXEC_H */
