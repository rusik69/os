#include "fault.h"
#include "idt.h"
#include "vmm.h"
#include "process.h"
#include "printf.h"
#include "nx_enforce.h"
#include "io.h"
#include "panic.h"
#include "kdump.h"
#include "smp.h"
#include "mce.h"
#include "signal.h"
#include "kprobes.h"
#include "string.h"
#include "pmm.h"
#include "userfaultfd.h"

/* Add vmm.h inclusion for vm_pgfault counter - already present via vmm.h */

/* PTE flags needed for execute-only page-table walking in the
 * page-fault handler.  These match the definitions in vmm.c but
 * are replicated here because vmm.c's internal constants are not
 * exposed via vmm.h.  VMM_FLAG_* equivalents (from vmm.h) are used
 * for the software-defined bits. */
#define PF_PTE_PRESENT   (1ULL << 0)
#define PF_PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PF_PTE_HUGE      (1ULL << 7)

static inline uint64_t read_cr2(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr0(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr4(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr4, %0" : "=r"(val));
    return val;
}

/* Page fault tracing flag */
int page_fault_trace = 0;

void page_fault_trace_enable(int enable) {
    page_fault_trace = enable;
}

/* Kernel stack depth check: verify RSP is within the current process's kernel stack */
int check_kernel_stack_depth(void) {
    struct process *proc = process_get_current();
    if (!proc) return 1; /* no process context, assume OK */
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    /* Kernel stack is between kernel_stack and stack_top */
    if (rsp < proc->kernel_stack || rsp >= proc->stack_top) {
        kprintf("*** KERNEL STACK OVERFLOW DETECTED *** pid=%u name=%s "
                "rsp=0x%llx stack_base=0x%llx stack_top=0x%llx\n",
                proc->pid, proc->name ? proc->name : "?",
                rsp, proc->kernel_stack, proc->stack_top);
        return 0;
    }
    return 1;
}

/*
 * Page fault handler (ISR 14).
 *
 * error_code bits:
 *   bit 0 = PRESENT   (0 = not-present, 1 = protection fault)
 *   bit 1 = WRITE     (0 = read, 1 = write)
 *   bit 2 = USER      (0 = kernel, 1 = user-mode access)
 *
 * CR2 holds the faulting virtual address.
 */
static void page_fault_handler(struct interrupt_frame *frame) {
    uint64_t cr2 = read_cr2();
    uint64_t err = frame->error_code;

    /* Update vmstat counters */
    vm_pgfault++;

    /* Distinguish major vs minor page faults:
     *   Major fault  = the page was swapped out or otherwise required I/O
     *   Minor fault  = the page was already in memory but needed PTE fixup
     *                   (COW, lazy allocation, NUMA migration, etc.)
     *
     * We detect a major fault by walking the page table: if the PTE
     * is non-present but has the swap marker bit (bit 1 set, present=0),
     * the page was swapped out and must be read back from the swap device.
     */
    {
        int is_major = 0;
        struct process *cur_fault = process_get_current();
        if (cur_fault && cur_fault->pml4) {
            uint64_t *pml4_f = cur_fault->pml4;
            int pml4_idx = (cr2 >> 39) & 0x1FF;
            int pdpt_idx = (cr2 >> 30) & 0x1FF;
            int pd_idx   = (cr2 >> 21) & 0x1FF;
            int pt_idx   = (cr2 >> 12) & 0x1FF;

            if ((pml4_f[pml4_idx] & PF_PTE_PRESENT)) {
                uint64_t *pdpt_f = (uint64_t *)PHYS_TO_VIRT(pml4_f[pml4_idx] & PF_PTE_ADDR_MASK);
                if ((pdpt_f[pdpt_idx] & PF_PTE_PRESENT)) {
                    uint64_t *pd_f = (uint64_t *)PHYS_TO_VIRT(pdpt_f[pdpt_idx] & PF_PTE_ADDR_MASK);
                    if ((pd_f[pd_idx] & PF_PTE_PRESENT)) {
                        if (pd_f[pd_idx] & PF_PTE_HUGE) {
                            /* 2 MB huge page — always in memory, not swapped */
                            is_major = 0;
                        } else {
                            uint64_t *pt_f = (uint64_t *)PHYS_TO_VIRT(pd_f[pd_idx] & PF_PTE_ADDR_MASK);
                            uint64_t pte = pt_f[pt_idx];
                            /* A swap entry is a non-present PTE with the
                             * software-defined swap marker bit set (bit 1). */
                            if (!(pte & PF_PTE_PRESENT) && (pte & (1ULL << 1)))
                                is_major = 1;
                        }
                    }
                }
            }
        }
        if (is_major)
            vm_pgmajfault++;
        /* Otherwise it's a minor fault — already counted via vm_pgfault */
    }

#ifdef CONFIG_DEBUG_PAGEALLOC
    /* ── Paranoid runtime debug checks ──────────────────────────── */
    {
        struct process *dbg_proc = process_get_current();

        /* Recursive fault detection: check if we're already handling */
        static int pf_recursion_depth = 0;
        if (pf_recursion_depth > 0) {
            kprintf("*** RECURSIVE PAGE FAULT DETECTED *** "
                    "depth=%d addr=0x%llx err=0x%llx\n",
                    pf_recursion_depth, cr2, err);
            if (pf_recursion_depth > 2) {
                panic("RECURSIVE PAGE FAULT (likely double-fault imminent)");
            }
        }
        pf_recursion_depth++;

        /* Verify kernel pointers are within valid range */
        if (!(err & (1ULL << 2))) {
            /* Kernel-mode fault: validate RIP and RSP */
            if (frame->rip < 0xFFFF800000000000ULL ||
                frame->rip >= 0xFFFFFFFFFFFFFFFFULL) {
                kprintf("*** WARNING: kernel fault with invalid RIP=0x%llx ***\n",
                        (unsigned long long)frame->rip);
            }
            if (frame->rsp < 0xFFFF800000000000ULL ||
                frame->rsp >= 0xFFFFFFFFFFFFFFFFULL) {
                kprintf("*** WARNING: kernel fault with invalid RSP=0x%llx ***\n",
                        (unsigned long long)frame->rsp);
            }
        }

        /* Check if this looks like a stack guard page access */
        if (dbg_proc && !(err & (1ULL << 2))) {
            uint64_t guard_page = dbg_proc->guard_page;
            if (guard_page && (cr2 & ~(uint64_t)0xFFF) == (guard_page & ~(uint64_t)0xFFF)) {
                kprintf("*** STACK GUARD PAGE ACCESS *** process=%s pid=%u "
                        "addr=0x%llx rip=0x%llx\n",
                        dbg_proc->name ? dbg_proc->name : "?",
                        (unsigned int)dbg_proc->pid,
                        (unsigned long long)cr2,
                        (unsigned long long)frame->rip);
                /* Deeper check: if RSP is at or below the guard page,
                 * this is a confirmed stack overflow */
                uint64_t rsp;
                __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
                if (rsp < dbg_proc->kernel_stack) {
                    kprintf("*** CONFIRMED KERNEL STACK OVERFLOW *** "
                            "RSP=0x%llx below stack base=0x%llx\n",
                            (unsigned long long)rsp,
                            (unsigned long long)dbg_proc->kernel_stack);
                }
            }
        }

        pf_recursion_depth--;
    }
#endif /* CONFIG_DEBUG_PAGEALLOC */

    /* Page fault tracing */
    if (page_fault_trace) {
        struct process *proc = process_get_current();
        kprintf("[PFTRACE] addr=0x%llx err=0x%llx (%s %s %s %s) rip=0x%llx pid=%u name=%s\n",
                cr2, err,
                (err & 1) ? "prot" : "np",
                (err & 2) ? "wr" : "rd",
                (err & 4) ? "usr" : "sup",
                (err & 16) ? "if" : "",
                frame->rip,
                proc ? (unsigned int)proc->pid : 0,
                proc && proc->name ? proc->name : "?");
    }

    /* Check for NX violation (instruction fetch on a non-executable page) */
    if ((err & (1ULL << 4)) && (err & 1ULL)) {
        if (nx_enforce_check_fault(cr2, err, frame)) {
            /* NX violation was handled (kernel: panic, user: SIGSEGV) */
            return;
        }
    }

    /* Check for execute-only violation: a read/write access (not an
     * instruction fetch) to a page tagged with VMM_FLAG_EXECONLY.
     *
     * On x86-64, PTE PRESENT always implies readability — there is no
     * separate read-enable bit.  We therefore approximate execute-only
     * semantics by tagging the page with a software bit (PTE_EXECONLY,
     * bit 11 of the PTE) and enforcing it here in software:
     *
     *   - If the faulting address has PTE_EXECONLY set AND
     *   - The access is NOT an instruction fetch (err bit 4 = 0)
     *
     * Then this is a read (or write) to an execute-only page → SIGSEGV.
     *
     * The instruction fetch case (err bit 4 = 1) is allowed — that's
     * the whole point of execute-only pages: code can be fetched but
     * not read or written. */
    if ((err & (1ULL << 4)) == 0 && (err & 1ULL)) {
        /* Read/write access to a present page — check EXECONLY flag */
        struct process *proc = process_get_current();
        if (proc && proc->pml4) {
            uint64_t *pml4 = proc->pml4;
            /* Walk the page table to check PTE_EXECONLY */
            int pml4_idx = (cr2 >> 39) & 0x1FF;
            int pdpt_idx = (cr2 >> 30) & 0x1FF;
            int pd_idx   = (cr2 >> 21) & 0x1FF;
            int pt_idx   = (cr2 >> 12) & 0x1FF;

            if ((pml4[pml4_idx] & PF_PTE_PRESENT)) {
                uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PF_PTE_ADDR_MASK);
                if ((pdpt[pdpt_idx] & PF_PTE_PRESENT)) {
                    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PF_PTE_ADDR_MASK);
                    if ((pd[pd_idx] & PF_PTE_PRESENT)) {
                        if (pd[pd_idx] & PF_PTE_HUGE) {
                            /* 2MB huge page — check PDE for EXECONLY */
                            if (pd[pd_idx] & VMM_FLAG_EXECONLY) {
                                kprintf("[exec-only] SIGSEGV pid=%u addr=0x%llx "
                                        "execute-only page read%s at rip=0x%llx\n",
                                        (unsigned int)proc->pid, cr2,
                                        (err & 2) ? "/write" : "",
                                        frame->rip);
                                /* Populate siginfo before terminating */
                                struct siginfo sinfo;
                                memset(&sinfo, 0, sizeof(sinfo));
                                sinfo.si_signo = SIGSEGV;
                                sinfo.si_code = SEGV_ACCERR;
                                sinfo.si_addr = (void *)(uintptr_t)cr2;
                                sinfo.si_pid = proc->pid;
                                sinfo.si_uid = proc->uid;
                                signal_send_info(proc->pid, SIGSEGV, &sinfo);
                                process_exit_code(11);
                                /* Not reached */
                            }
                        } else {
                            uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PF_PTE_ADDR_MASK);
                            if ((pt[pt_idx] & PF_PTE_PRESENT) && (pt[pt_idx] & VMM_FLAG_EXECONLY)) {
                                kprintf("[exec-only] SIGSEGV pid=%u addr=0x%llx "
                                        "execute-only page read%s at rip=0x%llx\n",
                                        (unsigned int)proc->pid, cr2,
                                        (err & 2) ? "/write" : "",
                                        frame->rip);
                                /* Populate siginfo before terminating */
                                {
                                    struct siginfo sinfo;
                                    memset(&sinfo, 0, sizeof(sinfo));
                                    sinfo.si_signo = SIGSEGV;
                                    sinfo.si_code = SEGV_ACCERR;
                                    sinfo.si_addr = (void *)(uintptr_t)cr2;
                                    sinfo.si_pid = proc->pid;
                                    sinfo.si_uid = proc->uid;
                                    signal_send_info(proc->pid, SIGSEGV, &sinfo);
                                }
                                process_exit_code(11);
                                /* Not reached */
                            }
                        }
                    }
                }
            }
        }
    }

    /* Kernel-mode fault: panic with register dump */
    if (!(err & (1ULL << 2))) {
        kprintf("\n*** KERNEL PAGE FAULT ***\n");

        /* Check for kernel stack overflow via guard page access */
        struct process *pt = process_get_table();
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (pt[i].guard_page &&
                (cr2 & ~(uint64_t)0xFFF) == (pt[i].guard_page & ~(uint64_t)0xFFF)) {
                kprintf("*** KERNEL STACK OVERFLOW DETECTED! Process: %s (pid=%u) ***\n",
                        pt[i].name ? pt[i].name : "?",
                        (unsigned int)pt[i].pid);
                break;
            }
        }
        kprintf("CR2=0x%llx  error=0x%llx  (PF: %s %s %s)\n", cr2, err,
                (err & 1) ? "prot" : "np",
                (err & 2) ? "wr" : "rd",
                (err & 4) ? "usr" : "sup");
        kprintf("RIP=0x%llx  RSP=0x%llx  RBP=0x%llx\n",
                frame->rip, frame->rsp, frame->rbp);
        kprintf("RAX=0x%llx  RBX=0x%llx  RCX=0x%llx  RDX=0x%llx\n",
                frame->rax, frame->rbx, frame->rcx, frame->rdx);
        kprintf("RSI=0x%llx  RDI=0x%llx  R8=0x%llx   R9=0x%llx\n",
                frame->rsi, frame->rdi, frame->r8, frame->r9);
        kprintf("R10=0x%llx  R11=0x%llx  R12=0x%llx  R13=0x%llx\n",
                frame->r10, frame->r11, frame->r12, frame->r13);
        kprintf("R14=0x%llx  R15=0x%llx\n", frame->r14, frame->r15);
        kprintf("CS=0x%llx  SS=0x%llx  RFLAGS=0x%llx\n",
                frame->cs, frame->ss, frame->rflags);
        arch_print_backtrace();
        panic("KERNEL PAGE FAULT at RIP=0x%llx CR2=0x%llx error=0x%llx",
              (unsigned long long)frame->rip,
              (unsigned long long)cr2,
              (unsigned long long)err);
    }

    /* User-mode write fault — check for COW */
    if ((err & (1ULL << 1))) {
        struct process *proc = process_get_current();
        if (proc && proc->pml4 && vmm_handle_cow_fault(proc->pml4, cr2)) {
            if (proc) proc->minflt++;
            return; /* handled */
        }
    }

    /* ── User stack auto-grow (RLIMIT_STACK) ────────────────────────── */
    /* A user-mode write to an unmapped page below the current stack bottom
     * may indicate stack expansion.  If the total stack size stays within
     * rlim_cur[RLIMIT_STACK], map the faulting page and retry. */
    if ((err & (1ULL << 2)) && !(err & 1)) {
        /* User-mode, not-present fault */
        struct process *proc = process_get_current();
        if (proc && proc->user_stack_bottom > 0 && proc->user_stack_top > 0) {
            uint64_t fault_page = cr2 & ~(uint64_t)(PAGE_SIZE - 1);
            /* Fault must be below current bottom (growth downward) */
            if (fault_page < proc->user_stack_bottom &&
                fault_page + PAGE_SIZE >= proc->user_stack_bottom) {
                uint64_t new_sz  = proc->user_stack_top - fault_page;
                /* Check RLIMIT_STACK (index 6) — RLIM_INFINITY means unlimited */
                uint64_t stack_limit = proc->rlim_cur[6]; /* RLIMIT_STACK */
                if (stack_limit == (uint64_t)-1 || new_sz <= stack_limit) {
                    /* Stack expansion is allowed — map one page */
                    uint64_t paddr = pmm_alloc_frame();
                    if (paddr) {
                        memset(PHYS_TO_VIRT(paddr), 0, PAGE_SIZE);
                        if (vmm_map_user_page(proc->pml4, fault_page, paddr,
                                              VMM_FLAG_PRESENT | VMM_FLAG_WRITE |
                                              VMM_FLAG_USER | VMM_FLAG_NOEXEC) == 0) {
                            proc->user_stack_bottom = fault_page;
                            proc->minflt++;
                            kprintf("[stack-grow] pid=%u stack 0x%llx -> 0x%llx (used=%llu limit=%llu)\n",
                                    (unsigned int)proc->pid,
                                    (unsigned long long)proc->user_stack_top,
                                    (unsigned long long)fault_page,
                                    (unsigned long long)new_sz,
                                    (unsigned long long)stack_limit);
                            return; /* retry faulting instruction */
                        }
                        pmm_free_frame(paddr);
                    }
                }
                /* If we reach here, expansion was denied (exceeded limit or OOM).
                 * Fall through to SIGSEGV — the fault is effectively a stack overflow. */
            }
        }
    }

    /* ── Userfaultfd: check if this fault address is registered ────── */
    /* For user-mode faults, check if any userfaultfd context has a
     * registered range covering the fault address.  If so, queue a
     * page fault event (or send SIGBUS) instead of mapping the page. */
    if ((err & (1ULL << 2))) {  /* user-mode fault */
        int write_flag = (err & (1ULL << 1)) ? 1 : 0;
        int uffd_fd = userfaultfd_find_for_addr(cr2, NULL);
        if (uffd_fd >= 0) {
            int is_minor = 0;
            int uffd_ret = userfaultfd_handle_fault(uffd_fd, cr2, write_flag, is_minor);
            if (uffd_ret == 0) {
                /* Handled — event queued or SIGBUS sent.  Don't map a page;
                 * userspace will handle it via the uffd reader or SIGBUS. */
                return;
            }
            /* uffd_ret == 1 means not handled by uffd (e.g. SIGBUS not set
             * and event queue full) — fall through to SIGSEGV. */
        }
    }

    /* Unhandled user fault: kill the process with SIGSEGV (code 11) */
    struct process *proc = process_get_current();
    kprintf("[fault] SIGSEGV pid=%u addr=0x%llx err=0x%llx rip=0x%llx\n",
            proc ? (unsigned int)proc->pid : 0, cr2, err, frame->rip);

    /* Populate siginfo_t with fault address, trap number, and error code
     * before terminating.  This enables signalfd listeners to receive
     * the full fault details. */
    if (proc) {
        struct siginfo sinfo;
        memset(&sinfo, 0, sizeof(sinfo));
        sinfo.si_signo = SIGSEGV;
        sinfo.si_code  = (err & 1) ? SEGV_ACCERR : SEGV_MAPERR;
        sinfo.si_addr  = (void *)(uintptr_t)cr2;
        sinfo.si_pid   = proc->pid;
        sinfo.si_uid   = proc->uid;
        signal_send_info(proc->pid, SIGSEGV, &sinfo);
    }

    process_exit_code(11); /* SIGSEGV = 11 — does not return */
}

/* ── Double-fault handler (#DF, vector 8) ────────────────────────── */
/* Runs on a dedicated IST stack (IST1), safe even when the regular kernel
 * stack has overflowed.  The IST switch happens BEFORE the CPU pushes any
 * handler state, so we have a guaranteed-good stack regardless of what
 * happened to the interrupted context's stack.
 *
 * Common causes of double faults:
 *   1. Kernel stack overflow — a #PF occurs during a push/mov on a full
 *      kernel stack, and the CPU's attempt to push the #PF error code
 *      causes a second #PF → #DF.
 *   2. Segment error recovery — a #GP/#NP/#SS/#TS handler itself causes
 *      a fault (e.g. the handler's code segment is invalid).
 *   3. IRET to a bad segment — IRET loads SS/CS from the stack and if
 *      those selectors are invalid, a #GP occurs; if #GP handler faults
 *      too, that's #DF.
 *
 * On x86-64 the error code pushed by the CPU for #DF is always 0
 * (reserved), so we must infer the root cause from the saved register
 * state.
 */
static void double_fault_handler(struct interrupt_frame *frame) {
    uint64_t cr0, cr2, cr3, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

    /*
     * Capture the fault state to the kdump region BEFORE printing —
     * the printing / serial I/O might itself fault, and we want the
     * registers preserved regardless.
     */
    {
        char msg[96];
        struct process *proc = process_get_current();
        int n = snprintf(msg, sizeof(msg),
            "DOUBLE FAULT at RIP=0x%lx CR2=0x%lx cpu=%u pid=%u",
            (unsigned long)frame->rip, (unsigned long)cr2,
            (unsigned int)smp_get_cpu_id(), proc ? (unsigned int)proc->pid : 0);
        if (n < 0 || (size_t)n >= sizeof(msg)) {
            /* Truncation is acceptable — msg is best-effort here */
        }
        msg[sizeof(msg) - 1] = '\0';
        kdump_capture(msg, frame->rip);
    }

    kprintf("\n*** DOUBLE FAULT (#DF) ***\n");
    kprintf("Error code: %lu  (always 0 on x86-64 — cause inferred from state)\n",
            (unsigned long)frame->error_code);
    kprintf("RIP: 0x%lx  RSP: 0x%lx  RBP: 0x%lx\n",
            (unsigned long)frame->rip, (unsigned long)frame->rsp,
            (unsigned long)frame->rbp);
    kprintf("RAX: 0x%lx  RBX: 0x%lx  RCX: 0x%lx  RDX: 0x%lx\n",
            (unsigned long)frame->rax, (unsigned long)frame->rbx,
            (unsigned long)frame->rcx, (unsigned long)frame->rdx);
    kprintf("RSI: 0x%lx  RDI: 0x%lx  R8: 0x%lx   R9: 0x%lx\n",
            (unsigned long)frame->rsi, (unsigned long)frame->rdi,
            (unsigned long)frame->r8, (unsigned long)frame->r9);
    kprintf("R10: 0x%lx  R11: 0x%lx  R12: 0x%lx  R13: 0x%lx\n",
            (unsigned long)frame->r10, (unsigned long)frame->r11,
            (unsigned long)frame->r12, (unsigned long)frame->r13);
    kprintf("R14: 0x%lx  R15: 0x%lx\n",
            (unsigned long)frame->r14, (unsigned long)frame->r15);
    kprintf("CS: 0x%lx  SS: 0x%lx  RFLAGS: 0x%lx\n",
            (unsigned long)frame->cs, (unsigned long)frame->ss,
            (unsigned long)frame->rflags);
    kprintf("CR0: 0x%lx  CR2: 0x%lx  CR3: 0x%lx  CR4: 0x%lx\n",
            (unsigned long)cr0, (unsigned long)cr2,
            (unsigned long)cr3, (unsigned long)cr4);

    /* ── Cause analysis ─────────────────────────────────────────── */
    struct process *proc = process_get_current();

    /*
     * Case 1: CR2 != 0 — the first fault was almost certainly a page
     * fault (#PF, vector 14), and the #PF handler's attempt to push
     * the error code / frame onto a full kernel stack caused the #DF.
     * This is the classic "kernel stack overflow" scenario.
     */
    if (cr2 != 0) {
        kprintf("*** LIKELY CAUSE: Nested page fault (kernel stack overflow?) ***\n");
        kprintf("    Faulting address (CR2): 0x%lx\n", (unsigned long)cr2);
        if (proc && proc->guard_page &&
            (cr2 & ~0xFFFULL) == (proc->guard_page & ~0xFFFULL)) {
            kprintf("    *** CONFIRMED: Guard page hit — kernel stack overflow "
                    "for process %s (pid=%u) ***\n",
                    proc->name ? proc->name : "?", (unsigned int)proc->pid);
        }
        /* Walk the original (faulting) stack if it looks reasonable */
        uint64_t orig_rsp = frame->rsp;
        if (orig_rsp >= 0xFFFF800000000000ULL) {
            kprintf("    Original RSP=0x%lx (stack likely overflowed to guard)\n",
                    (unsigned long)orig_rsp);
        }
    }

    /*
     * Case 2: CR2 == 0 but the CS selector in the saved frame looks
     * suspicious — suggests a #GP recovery failure (e.g. IRET to a
     * bad segment, or a segment-load instruction that faulted and the
     * #GP handler itself crashed).
     */
    else if ((frame->cs & 0xFFFF) == 0 ||
             (frame->ss & 0xFFFF) == 0 ||
             (frame->cs & 0xFFFF) > 0x18) {
        kprintf("*** LIKELY CAUSE: Segment error (#GP/#NP/#SS/#TS) — "
                "recovery failure ***\n");
        kprintf("    Suspicious CS=0x%lx or SS=0x%lx in saved frame\n",
                (unsigned long)(frame->cs & 0xFFFF),
                (unsigned long)(frame->ss & 0xFFFF));
    }

    /*
     * Case 3: Unusual RFLAGS values — could indicate a corrupted
     * interrupt return.
     */
    else if ((frame->rflags & 0x200) == 0) {
        /* IF is cleared — means we were in an interrupt handler */
        kprintf("*** LIKELY CAUSE: Fault within interrupt handler "
                "(IF=0 in saved RFLAGS) ***\n");
    }

    /*
     * Case 4: General — could be a hardware issue, corrupted stack,
     * or a rare corner case.
     */
    else {
        kprintf("*** LIKELY CAUSE: Unknown — see RIP/CR2 for context ***\n");
    }

    /* ── Stack trace ────────────────────────────────────────────── */
    if (proc) {
        kprintf("Process: %s (pid=%u, state=%u)\n",
                proc->name ? proc->name : "?", (unsigned int)proc->pid,
                (uint32_t)proc->state);
    }

    /* Print backtrace (walks the frame pointer chain on the IST stack) */
    arch_print_backtrace();

    /* Try to produce a secondary trace from the ORIGINAL (faulting) RSP
     * if it looks like a valid kernel stack — helpful when the fault
     * was a stack overflow and the frame pointers on the original stack
     * are still intact. */
    uint64_t orig_rsp = frame->rsp;
    if (orig_rsp >= 0xFFFF800000000000ULL && orig_rsp < 0xFFFFFFFFFFFFFFFFULL) {
        kprintf("Original-stack backtrace (via saved RSP=0x%lx):\n",
                (unsigned long)orig_rsp);
        uint64_t *stack = (uint64_t *)orig_rsp;
        int limit = (proc && proc->stack_top)
                    ? (int)((proc->stack_top - orig_rsp) / sizeof(uint64_t))
                    : 64;
        if (limit > 64) limit = 64;
        if (limit < 1)   limit = 1;
        for (int i = 0; i < limit; i++) {
            uint64_t val = stack[i];
            if (val >= 0xFFFF800000000000ULL && val < 0xFFFFFFFFFFFFFFFFULL) {
                kprintf("  [%d] 0x%lx\n", i, (unsigned long)val);
            }
        }
    }

    /* Delegate to panic() for full dump + kdump capture + timeout reset */
    panic("DOUBLE FAULT (#DF) — unrecoverable\n"
          "  RIP=0x%lx  CR2=0x%lx",
          (unsigned long)frame->rip, (unsigned long)cr2);
}

/* ── NMI handler (vector 2) ─────────────────────────────────────── */
/* Runs on dedicated IST stack. NMI is non-maskable and can indicate
 * hardware issues (ECC errors, watchdog, etc.) */
static void nmi_handler(struct interrupt_frame *frame) {
    kprintf("\n*** NMI — Non-Maskable Interrupt ***\n");
    kprintf("RIP: 0x%lx  RSP: 0x%lx\n",
            (unsigned long)frame->rip, (unsigned long)frame->rsp);

    /* Attempt to check for NMI source via port 0x61 (PC-style) */
    uint8_t nmi_status = inb(0x61);
    if (nmi_status & 0x80) {
        kprintf("NMI source: RAM parity error (port 0x61 bit 7 set)\n");
    } else if (nmi_status & 0x40) {
        kprintf("NMI source: Channel check (port 0x61 bit 6 set)\n");
    } else {
        kprintf("NMI source: unknown (0x61=0x%02x)\n", (unsigned int)nmi_status);
    }

    /* Log via dmesg for post-mortem analysis */
    kprintf("*** NMI received — continuing ***\n");
    /* NMI is often recoverable; return and continue execution */
}

/* ── General Protection Fault handler (vector 13) ──────────────── */
static void gp_fault_handler(struct interrupt_frame *frame) {
    kprintf("\n*** GENERAL PROTECTION FAULT (#13) ***\n");
    kprintf("Error code: 0x%lx\n", (unsigned long)frame->error_code);
    kprintf("RIP: 0x%lx  RSP: 0x%lx\n",
            (unsigned long)frame->rip, (unsigned long)frame->rsp);
    kprintf("RAX: 0x%lx  RBX: 0x%lx  RCX: 0x%lx  RDX: 0x%lx\n",
            (unsigned long)frame->rax, (unsigned long)frame->rbx,
            (unsigned long)frame->rcx, (unsigned long)frame->rdx);
    kprintf("RSI: 0x%lx  RDI: 0x%lx  R8:  0x%lx  R9:  0x%lx\n",
            (unsigned long)frame->rsi, (unsigned long)frame->rdi,
            (unsigned long)frame->r8, (unsigned long)frame->r9);
    kprintf("R10: 0x%lx  R11: 0x%lx  R12: 0x%lx  R13: 0x%lx\n",
            (unsigned long)frame->r10, (unsigned long)frame->r11,
            (unsigned long)frame->r12, (unsigned long)frame->r13);
    kprintf("R14: 0x%lx  R15: 0x%lx\n",
            (unsigned long)frame->r14, (unsigned long)frame->r15);
    kprintf("CS: 0x%lx  SS: 0x%lx  RFLAGS: 0x%lx\n",
            (unsigned long)frame->cs, (unsigned long)frame->ss,
            (unsigned long)frame->rflags);
    cli();
    for (;;) hlt();
}

/* ── Machine Check Exception handler (vector 18) ───────────────── */
/* Delegates to the production-quality implementation in mce.c.
 * The handler is registered via idt_register_handler() below. */
/* (mce_handler lives in mce.c, declared in mce.h) */

void __init fault_init(void) {
    idt_register_handler(14, page_fault_handler);
    idt_register_handler(8, double_fault_handler);
    idt_register_handler(13, gp_fault_handler);
    idt_register_handler(2, nmi_handler);
    idt_register_handler(18, mce_handler);
    /* Kprobes: INT3 (#BP, vector 3) and single-step (#DB, vector 1) */
    idt_register_handler(3, kprobe_int3_handler);
    idt_register_handler(1, kprobe_debug_handler);
}

/* ── Frame-pointer-based backtrace ──────────────────────────── */

/* Print a backtrace by walking the RBP-linked frame pointer chain.
 * Each frame: [rbp+0] = previous rbp, [rbp+8] = return address.
 */
void arch_print_backtrace(void) {
    uint64_t rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));

    /* Determine stack bounds from current process if available */
    uint64_t stack_low = 0, stack_high = ~0ULL;
    struct process *proc = process_get_current();
    if (proc) {
        stack_low  = proc->kernel_stack;
        stack_high = proc->stack_top;
    }

    kprintf("Backtrace:\n");
    for (int i = 0; i < 32; i++) {
        /* Validate frame pointer is in reasonable kernel range */
        if (rbp < 0xFFFF800000000000ULL || rbp >= 0xFFFFFFFFFFFFFFFFULL)
            break;
        /* Validate within kernel stack if we know bounds */
        if (stack_low && (rbp < stack_low || rbp >= stack_high))
            break;
        /* Check alignment */
        if (rbp & 0xF)
            break;

        uint64_t *frame = (uint64_t *)rbp;
        uint64_t ret_addr = frame[1];
        if (ret_addr == 0)
            break;

        kprintf("  [%02d] 0x%llx\n", i, ret_addr);

        rbp = frame[0];
    }
}

/* ── kpanic — print a formatted message then halt ─────────────────── */

__printf(1, 2)
void kpanic(const char *fmt, ...) {
    __builtin_va_list ap;
    __asm__ volatile("cli");
    kprintf("\n*** KERNEL PANIC ***\n");
    __builtin_va_start(ap, fmt);
    vkprintf(fmt, ap);
    __builtin_va_end(ap);
    kprintf("\n");
    kprintf("CR0=0x%llx  CR2=0x%llx  CR3=0x%llx  CR4=0x%llx\n",
            read_cr0(), read_cr2(), read_cr3(), read_cr4());
    arch_print_backtrace();
    for (;;) __asm__ volatile("hlt");
    __builtin_unreachable();
}

/* ── Per-task stack usage ─────────────────────────────────────── */

uint64_t task_stack_usage(struct process *p) {
    if (!p || p->kernel_stack == 0 || p->stack_top == 0) return 0;
    /* Read current RSP */
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    /* If we're not running in this process's context, try to estimate */
    struct process *cur = process_get_current();
    if (cur != p) {
        /* Return last known watermark */
        return p->stack_watermark;
    }
    /* Stack usage = current stack top - current RSP */
    uint64_t used = p->stack_top - rsp;
    /* Update watermark (lowest RSP = deepest stack usage) */
    if (rsp < p->stack_watermark || p->stack_watermark == 0)
        p->stack_watermark = rsp;
    return used;
}

/* Update stack watermark on context switch - called from scheduler */
void task_update_stack_watermark(struct process *p) {
    if (!p) return;
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    if (rsp < p->stack_watermark || p->stack_watermark == 0)
        p->stack_watermark = rsp;
}

/* ── Stub: fault_handle_page_fault ─────────────────────────────── */
int fault_handle_page_fault(uint64_t addr, uint64_t error_code)
{
    (void)addr;
    (void)error_code;
    kprintf("[fault] fault_handle_page_fault: not yet implemented\n");
    return 0;
}
/* ── Stub: fault_handle_gpf ─────────────────────────────── */
int fault_handle_gpf(uint64_t error_code)
{
    (void)error_code;
    kprintf("[fault] fault_handle_gpf: not yet implemented\n");
    return 0;
}
/* ── Stub: fault_handle_df ─────────────────────────────── */
int fault_handle_df(uint64_t error_code)
{
    (void)error_code;
    kprintf("[fault] fault_handle_df: not yet implemented\n");
    return 0;
}
