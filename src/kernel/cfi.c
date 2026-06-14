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
static int cfi_target_count;
static spinlock_t cfi_lock;
static int cfi_enabled;

/* Register a valid indirect call target */
int cfi_register_target(uintptr_t addr, const char *name)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&cfi_lock, &irq_flags);

    if (cfi_target_count >= CFI_MAX_SITES) {
        spinlock_irqsave_release(&cfi_lock, irq_flags);
        return -1;
    }

    cfi_targets[cfi_target_count].addr = addr;
    cfi_targets[cfi_target_count].func_name = name;
    cfi_targets[cfi_target_count].registered = 1;
    cfi_target_count++;

    spinlock_irqsave_release(&cfi_lock, irq_flags);
    return 0;
}

/* Check if an address is a valid indirect call target */
int cfi_check_target(uintptr_t addr)
{
    if (!cfi_enabled)
        return 0; /* CFI disabled, allow all */

    for (int i = 0; i < cfi_target_count; i++) {
        if (cfi_targets[i].registered && cfi_targets[i].addr == addr)
            return 1; /* valid target */
    }

    return 0; /* not found */
}

/* Indirect call check — called from instrumented code via asm stub */
int __attribute__((used)) __cfi_check(uintptr_t target)
{
    if (!cfi_check_target(target)) {
        struct process *cur = process_get_current();
        kprintf("[CFI] Indirect call to invalid target 0x%llx from pid=%u %s\n",
                (unsigned long long)target,
                cur ? cur->pid : 0,
                cur && cur->name ? cur->name : "?");
        return -1;
    }
    return 0;
}

/* Enable/disable CFI checking */
void cfi_set_enabled(int enabled)
{
    cfi_enabled = enabled;
    kprintf("[CFI] %s\n", enabled ? "enabled" : "disabled");
}

void cfi_init(void)
{
    cfi_enabled = 1;
    spinlock_init(&cfi_lock);
    kprintf("[OK] CFI — Forward-edge control flow integrity (%d slots)\n",
            CFI_MAX_SITES);
}
