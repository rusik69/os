// SPDX-License-Identifier: GPL-2.0-only
/*
 * cfi.c — Forward-edge Control Flow Integrity
 *
 * Checks function pointer targets at indirect call sites
 * to ensure they point to valid functions.
 */
#include "types.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#include "process.h"

#define CFI_CHECK_HASH_SIZE 4096
#define CFI_MAX_SITES 1024

/* Tracking for valid indirect call targets */
struct cfi_target {
    uintptr_t addr;
    const char *func_name;
    int registered;
};

static struct cfi_target cfi_targets[CFI_MAX_SITES];
static int cfi_target_count = 0;
static spinlock_t cfi_lock;
static int cfi_enabled = 0;

/* Register a valid indirect call target */
static int cfi_register_target(uintptr_t addr, const char *name)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&cfi_lock, &irq_flags);

    if (cfi_target_count >= CFI_MAX_SITES) {
        spinlock_irqsave_release(&cfi_lock, irq_flags);
        return -ENOSYS;
    }

    cfi_targets[cfi_target_count].addr = addr;
    cfi_targets[cfi_target_count].func_name = name;
    cfi_targets[cfi_target_count].registered = 1;
    cfi_target_count++;

    spinlock_irqsave_release(&cfi_lock, irq_flags);
    return 0;
}

/* Unregister a previously registered CFI target by address.
 * Returns 0 on success, -ENOENT if the address was not found. */
static int cfi_unregister_target(uintptr_t addr)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&cfi_lock, &irq_flags);

    for (int i = 0; i < cfi_target_count; i++) {
        if (cfi_targets[i].registered && cfi_targets[i].addr == addr) {
            cfi_targets[i].registered = 0;
            spinlock_irqsave_release(&cfi_lock, irq_flags);
            return 0;
        }
    }

    spinlock_irqsave_release(&cfi_lock, irq_flags);
    return -ENOENT;
}

/* Unregister all CFI targets whose addresses fall within [base, base+size).
 * Used during module unload to ensure no dangling function pointers remain
 * in the CFI target table after module code pages are freed. */
static void cfi_unregister_range(uintptr_t base, uintptr_t size)
{
    if (size == 0)
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&cfi_lock, &irq_flags);

    uintptr_t end = base + size;
    for (int i = 0; i < cfi_target_count; i++) {
        if (cfi_targets[i].registered &&
            cfi_targets[i].addr >= base &&
            cfi_targets[i].addr < end) {
            cfi_targets[i].registered = 0;
        }
    }

    spinlock_irqsave_release(&cfi_lock, irq_flags);
}

/* Check if an address is a valid indirect call target */
static int cfi_check_target(uintptr_t addr)
{
    if (!cfi_enabled)
        return 0; /* CFI disabled, allow all */

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&cfi_lock, &irq_flags);

    int found = 0;
    for (int i = 0; i < cfi_target_count; i++) {
        if (cfi_targets[i].registered && cfi_targets[i].addr == addr) {
            found = 1;
            break;
        }
    }

    spinlock_irqsave_release(&cfi_lock, irq_flags);
    return found;
}

/* Indirect call check — called from instrumented code via asm stub */
static int __attribute__((used)) __cfi_check(uintptr_t target)
{
    if (!cfi_check_target(target)) {
        struct process *cur = process_get_current();
        kprintf("[CFI] Indirect call to invalid target 0x%llx from pid=%u %s\n",
                (unsigned long long)target,
                cur ? cur->pid : 0,
                cur && cur->name ? cur->name : "?");
        return -ENOSYS;
    }
    return 0;
}

/* Enable/disable CFI checking */
static void cfi_set_enabled(int enabled)
{
    cfi_enabled = enabled;
    kprintf("[CFI] %s\n", enabled ? "enabled" : "disabled");
}

static void cfi_init(void)
{
    cfi_enabled = 1;
    spinlock_init(&cfi_lock);
    kprintf("[OK] CFI — Forward-edge control flow integrity (%d slots)\n",
            CFI_MAX_SITES);
}

/* ── Stub: cfi_check ──────────────────────────────────────────────────── */
static int cfi_check(uintptr_t target, const char *func)
{
    (void)target;
    (void)func;
    kprintf("[CFI] cfi_check not yet fully implemented\n");
    return cfi_check_target(target);
}

/* ── Stub: cfi_set_shadow ─────────────────────────────────────────────── */
static int cfi_set_shadow(uintptr_t addr, uintptr_t shadow)
{
    (void)addr;
    (void)shadow;
    kprintf("[CFI] cfi_set_shadow not yet implemented\n");
    return 0;
}

/* ── Stub: cfi_verify ─────────────────────────────────────────────────── */
static int cfi_verify(uintptr_t target, uintptr_t expected)
{
    (void)target;
    (void)expected;
    kprintf("[CFI] cfi_verify not yet implemented\n");
    return 0;
}
