// SPDX-License-Identifier: GPL-2.0-only
/*
 * execshield.c — W^X enforcement
 *
 * Provides:
 * - Mprotect W^X enforcement (cannot create writable+executable mappings)
 * - READ_IMPLIES_EXEC personality handling
 * - PT_GNU_STACK processing for ELF binaries
 */
#include "types.h"
#include "printf.h"
#include "string.h"
#include "process.h"
#include "vmm.h"
#include "elf.h"

/* ELF segment flags (not defined in elf.h) */
#define PF_X        1
#define PF_W        2
#define PF_R        4

/* PT_GNU_STACK — program header type for stack executability */
#define PT_GNU_STACK 0x6474e551

/* ── W^X enforcement ──────────────────────────────────────────────── */

/* Check if a memory protection combination violates W^X */
int execshield_check_wx(uint64_t prot_flags)
{
    /* Extract writable and executable bits from protection flags */
    int writable = (prot_flags & PROT_WRITE) ? 1 : 0;
    int executable = (prot_flags & PROT_EXEC) ? 1 : 0;

    /* W+X is forbidden */
    if (writable && executable)
        return -1; /* W^X violation */

    return 0;
}

/* ── READ_IMPLIES_EXEC ────────────────────────────────────────────── */

/* Determine whether a process should get READ_IMPLIES_EXEC based on
 * ELF header flags (e.g., PT_GNU_STACK absence) */
int execshield_read_implies_exec(const struct elf64_header *ehdr,
                                  const struct elf64_phdr *phdrs,
                                  int phdr_count)
{
    /* If no PT_GNU_STACK, legacy behavior implies exec on read */
    int gnu_stack_found = 0;

    (void)ehdr;

    for (int i = 0; i < phdr_count; i++) {
        if (phdrs[i].p_type == PT_GNU_STACK) {
            gnu_stack_found = 1;
            /* If stack is executable, enable READ_IMPLIES_EXEC */
            if (phdrs[i].p_flags & PF_X)
                return 1;
            break;
        }
    }

    /* No PT_GNU_STACK: legacy binary, enable READ_IMPLIES_EXEC */
    if (!gnu_stack_found)
        return 1;

    return 0;
}

/* Apply READ_IMPLIES_EXEC personality to a process */
void execshield_apply_personality(struct process *proc, int read_implies_exec)
{
    if (!proc)
        return;

    if (read_implies_exec) {
        proc->personality |= 0x40000; /* READ_IMPLIES_EXEC */
    } else {
        proc->personality &= ~0x40000;
    }
}

/* ── mprotect enforcement ─────────────────────────────────────────── */

/* Hook for mprotect syscall: enforce W^X */
int execshield_mprotect_check(uint64_t addr, uint64_t len, uint64_t prot,
                               struct process *proc)
{
    (void)addr;
    (void)len;

    /* Skip enforcement for kernel threads */
    if (!proc || !proc->is_user)
        return 0;

    /* Check if W^X would be violated */
    if (execshield_check_wx(prot) < 0) {
        kprintf("[EXECSHIELD] W^X denied: pid=%u addr=0x%llx len=%llu prot=0x%llx\n",
                proc->pid, (unsigned long long)addr,
                (unsigned long long)len, (unsigned long long)prot);
        return -1;
    }

    /* If READ_IMPLIES_EXEC is set, X implies R; ensure W is not set with X */
    if (proc->personality & 0x40000) {
        /* Allow R+X but still deny W+X */
        /* Already checked above */
    }

    return 0;
}

/* ── Init ─────────────────────────────────────────────────────────── */

void execshield_init(void)
{
    kprintf("[OK] Exec Shield (W^X enforcement + READ_IMPLIES_EXEC + PT_GNU_STACK)\n");
}
