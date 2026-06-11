#ifndef KPTI_H
#define KPTI_H

#include "types.h"

/*
 * kpti.h — Kernel Page-Table Isolation (KPTI)
 *
 * KPTI separates user-mode execution from kernel page tables.
 * In user mode, the active page table (user_pml4) only maps:
 *   - User-space memory (PML4 entries 0..255)
 *   - A small trampoline page for syscall entry/exit
 * In kernel mode (syscalls, interrupts), the full page table is used.
 *
 * SYSCALL entry trampoline (at KPTI_TRAMPOLINE_VADDR) handles the
 * CR3 switch.  It's mapped supervisor-only in both page tables.
 */

/* ── Trampoline virtual address ────────────────────────────────────
 * Chosen just below USER_VADDR_MAX (0x0000800000000000).
 * This is in the canonical user half, so it can be managed with
 * vmm_map_user_page() after clearing the USER bit for supervisor-only.
 */
#define KPTI_TRAMPOLINE_VADDR  0x00007FFFFFFE0000ULL
#define KPTI_TRAMPOLINE_SIZE   0x1000ULL  /* one 4K page */

/* Layout of the single trampoline page:
 *   +0x000  code:  syscall_entry_trampoline (128 bytes)
 *   +0x080  code:  syscall_exit_trampoline  (128 bytes)
 *   +0x100  data:  CR3 values, saved registers (256 bytes)
 */
#define KPTI_TRAMP_OFF_ENTRY      0x000
#define KPTI_TRAMP_OFF_EXIT       0x080
#define KPTI_TRAMP_OFF_CR3_KERN   0x100
#define KPTI_TRAMP_OFF_CR3_USER   0x108
#define KPTI_TRAMP_OFF_SAVE_RSP   0x110
#define KPTI_TRAMP_OFF_SAVE_RIP   0x118
#define KPTI_TRAMP_OFF_SAVE_RFL   0x120
#define KPTI_TRAMP_OFF_EXIT_RIP   0x128

/* ── Per-process KPTI state ──────────────────────────────────────── */
struct kpti_state {
    uint64_t *kernel_pml4;   /* Full (kernel + user) PML4 — used in kernel mode */
    uint64_t *user_pml4;     /* User-only PML4 — used in user mode */
    uint64_t  kernel_cr3;    /* Cached CR3 value for kernel_pml4 */
    uint64_t  user_cr3;      /* Cached CR3 value for user_pml4 */
};

/* ── API ─────────────────────────────────────────────────────────── */

/* Initialize KPTI: allocate trampoline, set up mappings, patch LSTAR MSR. */
void kpti_init(void);

/* Create user-only PML4 for a process.
 * Copies user entries (0..255) from kernel_pml4.
 * Adds the supervisor-only trampoline mapping.
 * Updates proc->kpti_state. */
int kpti_setup_process(struct process *proc);

/* Free user PML4 for a process. */
void kpti_teardown_process(struct process *proc);

/* Called by syscall trampoline assembly — fill CR3 values. */
void kpti_trampoline_patch_cr3(int cpu, uint64_t kernel_cr3, uint64_t user_cr3);

/* Check if KPTI is active. */
int kpti_is_active(void);

#endif /* KPTI_H */
