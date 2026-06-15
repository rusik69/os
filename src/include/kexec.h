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

/* ── Multi-segment support ────────────────────────────────────────── */

/* Maximum number of memory segments that can be loaded via kexec_load.
 * This matches the Linux KEXEC_SEGMENT_MAX (16). */
#define KEXEC_SEGMENT_MAX    16

/* A single memory segment descriptor.
 * Used by kexec_load to describe one contiguous region to load. */
struct kexec_segment {
    uint64_t buf;       /* Virtual address of source buffer in caller space */
    uint64_t bufsz;     /* Size of source buffer */
    uint64_t mem;       /* Physical address to load the segment to */
    uint64_t memsz;     /* Size in memory (may be larger than bufsz, zero-filled) */
};

/* ── Crash kernel parameters ───────────────────────────────────────── */

/* Physical address and size of the crash kernel region, parsed from
 * the crashkernel= kernel command-line parameter.
 * Format: crashkernel=size[@offset]
 * Example: crashkernel=64M@256M  → 64 MB at physical 0x10000000
 *          crashkernel=128M       → 128 MB, auto-placement
 */
#define CRASH_KERNEL_DEFAULT_SIZE  (64ULL * 1024 * 1024)   /* 64 MB */
#define CRASH_KERNEL_DEFAULT_BASE  0x10000000ULL            /* 256 MB */

extern uint64_t crash_kernel_base;
extern uint64_t crash_kernel_size;
extern int      crash_kernel_reserved;

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
 * kexec_load_segments — Load a new kernel via segment descriptors.
 *
 * Accepts an array of up to KEXEC_SEGMENT_MAX segment descriptors.
 * Validates segments for:
 *   - No overlap between segments
 *   - Reasonable sizes (each ≤ 16 MB)
 *   - Physical addresses within the reserved kexec region
 *
 * @segments:  Array of segment descriptors.
 * @nr_segments: Number of segments (1..KEXEC_SEGMENT_MAX).
 * @entry:     Entry point physical address.
 * @flags:     KEXEC_FLAG_* flags.
 *
 * Returns 0 on success, negative errno on error.
 */
int kexec_load_segments(const struct kexec_segment *segments,
                        unsigned long nr_segments,
                        unsigned long entry,
                        unsigned long flags);

/*
 * kexec_reboot — Jump to the loaded kernel image.
 *
 * Disables interrupts, masks all APIC LVT entries, flushes TLBs,
 * and jumps to the previously loaded entry point in 64-bit long mode.
 *
 * This function does NOT return on success.
 */
void kexec_reboot(void) __attribute__((noreturn));

/* ── Crash kexec ───────────────────────────────────────────────────── */

/*
 * kexec_crash_load — Load a crash kernel into the reserved crash region.
 *
 * Analogous to kexec_load but for the crash kernel used by kdump.
 * The crash kernel image is loaded into the crash_kernel_base region.
 *
 * Returns 0 on success, negative errno on error.
 */
int kexec_crash_load(uint64_t phys_addr, uint64_t entry, uint32_t flags);

/*
 * kexec_crash_reboot — Jump to the loaded crash kernel.
 *
 * Called from panic() to boot into the crash kernel for memory dump.
 * Same transition as kexec_reboot but uses the crash kernel entry.
 */
void kexec_crash_reboot(void) __attribute__((noreturn));

/* ── Debug / status ─────────────────────────────────────────────────── */
int kexec_is_loaded(void);
uint64_t kexec_get_entry(void);
uint64_t kexec_get_phys_addr(void);

/* Sysfs toggle: if non-zero, kexec_load is disabled system-wide.
 * Default: 0 (enabled).  Writable via /sys/kernel/kexec_load_disabled. */
extern int kexec_load_disabled;

/* Sysfs toggle: if non-zero, crash kexec is invoked AFTER panic notifiers.
 * Default: 0 (immediate crash kexec, before notifiers).
 * Writable via /sys/kernel/crash_kexec_post_notifiers. */
extern int crash_kexec_post_notifiers;

/* Check if a crash kernel has been loaded via kexec_crash_load(). */
int kexec_crash_is_loaded(void);

#endif /* KEXEC_H */
