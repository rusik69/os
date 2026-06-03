/*
 * nx_enforce.c — NX-bit enforcement for kernel and user pages
 *
 * Provides runtime verification that:
 *   - EFER.NXE is enabled
 *   - Only kernel .text pages are executable
 *   - All other kernel pages (.rodata, .data, .bss, heap, stack) have NX=1
 *   - The page-fault handler identifies NX violations and acts on them
 *
 * This is a defence-in-depth mechanism to catch stray execution attempts
 * (e.g., corrupt function pointers, ROP gadgets, shellcode injection) early.
 */

#include "nx_enforce.h"
#include "cpu.h"
#include "cpu_features.h"
#include "printf.h"
#include "vmm.h"
#include "process.h"
#include "fault.h" /* for arch_print_backtrace, interrupt_frame */
#include "idt.h"   /* for struct interrupt_frame */
#include "types.h" /* for PHYS_TO_VIRT */
#include "panic.h"
#include "signal.h"
#include "string.h"

/* ── Linker section boundaries ──────────────────────────────────────── */
/* These are defined in linker.ld and resolved at link time.
 * We declare them as arrays-of-char so we can take their address,
 * then cast to uint64_t for numeric comparison. */
extern char _text_start[], _text_end[];     /* kernel executable code */
extern char _rodata_start[], _rodata_end[]; /* read-only data */
extern char _data_start[], _data_end[];     /* read-write data */
extern char _bss_start[], _bss_end[];       /* zero-initialised data */
extern char _lbss_start[], _lbss_end[];     /* large BSS (if present) */
extern char _kernel_end[];                  /* end of all kernel sections */

/* PTE flag */
#define PTE_NX       (1ULL << 63)
#define PTE_PRESENT  (1ULL << 0)
#define PTE_HUGE     (1ULL << 7)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* Forward declarations of kernel page-table root (from vmm.c) */
extern uint64_t *kernel_pml4;
extern int nx_enabled; /* from vmm.c NX detection */

/* ── State ──────────────────────────────────────────────────────────── */

static int nx_self_active = 0; /* has nx_enforce_init succeeded? */

/*
 * Registered executable regions.
 * By default, this contains just the kernel .text section.
 * The module loader, JIT compiler, or other subsystems can register
 * additional regions via nx_enforce_register_region().
 */
static struct exec_region {
    uint64_t start;
    uint64_t end;
} exec_regions[NX_ENFORCE_MAX_REGIONS];

static int nr_exec_regions = 0;

/* ── Initialisation ─────────────────────────────────────────────────── */

int nx_enforce_init(void) {
    int rax, rbx, rcx, rdx;

    /* Check CPUID leaf 0x80000001 for NX support (EDX bit 20) */
    __asm__ volatile("cpuid"
                     : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx)
                     : "a"(0x80000001));

    if (!(rdx & (1 << 20))) {
        kprintf("[nx] NX (No-Execute) not supported by CPU — skipping\n");
        return -1;
    }

    /* Enable NXE in IA32_EFER MSR (bit 11) */
    uint64_t efer = read_msr(0xC0000080);
    if (!(efer & EFER_NXE)) {
        efer |= EFER_NXE;
        write_msr(0xC0000080, efer);
        kprintf("[nx] EFER.NXE enabled\n");
    }

    nx_self_active = 1;

    /* Register the kernel .text section as the primary executable region */
    uint64_t text_start = (uint64_t)(uintptr_t)_text_start;
    uint64_t text_end   = (uint64_t)(uintptr_t)_text_end;
    nx_enforce_register_region(text_start, text_end);

    kprintf("[nx] Kernel .text : 0x%llx – 0x%llx  (%llu KB)\n",
            text_start, text_end, (text_end - text_start) / 1024);
    kprintf("[nx] Kernel .rodata: 0x%llx – 0x%llx\n",
            (uint64_t)(uintptr_t)_rodata_start,
            (uint64_t)(uintptr_t)_rodata_end);
    kprintf("[nx] Kernel .data  : 0x%llx – 0x%llx\n",
            (uint64_t)(uintptr_t)_data_start,
            (uint64_t)(uintptr_t)_data_end);
    kprintf("[nx] Kernel .bss   : 0x%llx – 0x%llx\n",
            (uint64_t)(uintptr_t)_bss_start,
            (uint64_t)(uintptr_t)_bss_end);
    kprintf("[nx] NX enforcement initialised successfully\n");

    return 0;
}

int nx_enforce_is_active(void) {
    return nx_self_active && nx_enabled;
}

/* ── Executable region tracking ─────────────────────────────────────── */

int nx_enforce_register_region(uint64_t start, uint64_t end) {
    if (!nx_self_active) return -1;
    if (start >= end) return -1;
    if (nr_exec_regions >= NX_ENFORCE_MAX_REGIONS) {
        kprintf("[nx] WARNING: exec region table full (max %d)\n",
                NX_ENFORCE_MAX_REGIONS);
        return -1;
    }
    exec_regions[nr_exec_regions].start = start;
    exec_regions[nr_exec_regions].end   = end;
    nr_exec_regions++;
    kprintf("[nx] Registered exec region [%d]: 0x%llx – 0x%llx\n",
            nr_exec_regions - 1, start, end);
    return 0;
}

int nx_enforce_is_executable(uint64_t vaddr) {
    if (!nx_self_active) return 1; /* allow everything if not active */
    for (int i = 0; i < nr_exec_regions; i++) {
        if (vaddr >= exec_regions[i].start && vaddr < exec_regions[i].end)
            return 1;
    }
    return 0;
}

void nx_enforce_print_regions(void) {
    kprintf("[nx] Registered executable regions (%d):\n", nr_exec_regions);
    for (int i = 0; i < nr_exec_regions; i++) {
        kprintf("  [%d] 0x%llx – 0x%llx  (%llu KB)\n",
                i, exec_regions[i].start, exec_regions[i].end,
                (exec_regions[i].end - exec_regions[i].start) / 1024);
    }
}

/* ── Internal helpers ───────────────────────────────────────────────── */

/* Return a human-readable classification for a kernel virtual address. */
static const char *region_name(uint64_t vaddr) {
    if (vaddr >= (uint64_t)(uintptr_t)_text_start &&
        vaddr <  (uint64_t)(uintptr_t)_text_end)
        return ".text";
    if (vaddr >= (uint64_t)(uintptr_t)_rodata_start &&
        vaddr <  (uint64_t)(uintptr_t)_rodata_end)
        return ".rodata";
    if (vaddr >= (uint64_t)(uintptr_t)_data_start &&
        vaddr <  (uint64_t)(uintptr_t)_data_end)
        return ".data";
    if (vaddr >= (uint64_t)(uintptr_t)_bss_start &&
        vaddr <  (uint64_t)(uintptr_t)_bss_end)
        return ".bss";
    if (vaddr >= (uint64_t)(uintptr_t)_lbss_start &&
        vaddr <  (uint64_t)(uintptr_t)_lbss_end)
        return ".lbss";
    if (vaddr >= 0xFFFF800000000000ULL)
        return "kernel (unknown)";
    return "user";
}

/* Check whether a single PTE has the correct NX attribute.
 * Returns 0 on success (correct), 1 on violation (logged). */
static int check_pte(uint64_t vaddr, uint64_t pte, int level) {
    if (!(pte & PTE_PRESENT)) return 0;

    int has_nx  = (pte & PTE_NX) ? 1 : 0;
    int is_exec = !has_nx;
    int should_exec = nx_enforce_is_executable(vaddr);

    if (is_exec && !should_exec) {
        kprintf("[nx] AUDIT: %s page 0x%llx (L%d) is EXECUTABLE but is in %s"
                " — missing NX!\n",
                region_name(vaddr), vaddr, level, region_name(vaddr));
        return 1;
    }

    if (!is_exec && should_exec) {
        kprintf("[nx] AUDIT: .text page 0x%llx (L%d) has NX set but"
                " should be executable!\n",
                vaddr, level);
        return 1;
    }

    return 0;
}

/* ── Kernel page-table walker ───────────────────────────────────────── */

/*
 * Walk the kernel PML4 and audit every leaf page-table entry.
 * This covers the kernel identity map (PML4[0]) and the high-half
 * map (PML4[256..511]).
 */
int nx_enforce_audit_kernel(void) {
    if (!nx_self_active) return 0;

    int violations = 0;
    uint64_t *pml4 = kernel_pml4;
    if (!pml4) {
        kprintf("[nx] audit: kernel_pml4 is NULL — cannot audit\n");
        return -1;
    }

    for (int pml4_idx = 0; pml4_idx < 512; pml4_idx++) {
        if (!(pml4[pml4_idx] & PTE_PRESENT))
            continue;

        uint64_t pml4_vbase = (uint64_t)pml4_idx << 39;
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);

        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            if (!(pdpt[pdpt_idx] & PTE_PRESENT))
                continue;

            uint64_t pdpt_vbase = pml4_vbase | ((uint64_t)pdpt_idx << 30);

            /* 1 GB huge page? */
            if (pdpt[pdpt_idx] & PTE_HUGE) {
                violations += check_pte(pdpt_vbase, pdpt[pdpt_idx], 2);
                continue;
            }

            uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);

            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                if (!(pd[pd_idx] & PTE_PRESENT))
                    continue;

                uint64_t pd_vbase = pdpt_vbase | ((uint64_t)pd_idx << 21);

                /* 2 MB huge page? */
                if (pd[pd_idx] & PTE_HUGE) {
                    violations += check_pte(pd_vbase, pd[pd_idx], 1);
                    continue;
                }

                uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);

                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    if (!(pt[pt_idx] & PTE_PRESENT))
                        continue;

                    uint64_t pt_vbase = pd_vbase | ((uint64_t)pt_idx << 12);
                    violations += check_pte(pt_vbase, pt[pt_idx], 0);
                }
            }
        }
    }

    if (violations == 0) {
        kprintf("[nx] audit: OK — all kernel pages have correct NX settings\n");
    } else {
        kprintf("[nx] audit: %d NX violation(s) found in kernel page tables!\n",
                violations);
    }

    return violations;
}

/* ── User-range audit ───────────────────────────────────────────────── */

int nx_enforce_audit_user_range(uint64_t *pml4, uint64_t start, uint64_t end) {
    if (!nx_self_active || !pml4) return 0;
    int violations = 0;

    for (uint64_t vaddr = start & ~(uint64_t)0xFFF;
         vaddr < end; vaddr += 0x1000) {
        if (vaddr >= 0xFFFFFFFFFFFFF000ULL) break; /* safety */

        int pml4_idx = (int)((vaddr >> 39) & 0x1FF);
        int pdpt_idx = (int)((vaddr >> 30) & 0x1FF);
        int pd_idx   = (int)((vaddr >> 21) & 0x1FF);
        int pt_idx   = (int)((vaddr >> 12) & 0x1FF);

        if (!(pml4[pml4_idx] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);
        if (!(pdpt[pdpt_idx] & PTE_PRESENT)) continue;
        uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);
        if (!(pd[pd_idx] & PTE_PRESENT)) continue;

        if (pd[pd_idx] & PTE_HUGE) {
            /* 2MB page — check NX at PDE level */
            if (!(pd[pd_idx] & PTE_NX)) {
                /* executable huge page in user space: only valid for
                 * regions registered as executable */
                if (!nx_enforce_is_executable(vaddr)) {
                    kprintf("[nx] user-audit: executable 2MB page at 0x%llx"
                            " not in exec region\n", vaddr);
                    violations++;
                }
            }
            continue;
        }

        uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);
        if (!(pt[pt_idx] & PTE_PRESENT)) continue;

        uint64_t pte = pt[pt_idx];
        if (!(pte & PTE_NX) && !nx_enforce_is_executable(vaddr)) {
            /* executable user page outside known exec region */
            kprintf("[nx] user-audit: executable user page at 0x%llx"
                    " not in exec region\n", vaddr);
            violations++;
        }
    }

    return violations;
}

/* ── Page-fault integration ─────────────────────────────────────────── */

/*
 * Called from the page-fault handler when error-code bits indicate
 * an instruction fetch (bit 4) and a protection violation (bit 0).
 *
 * Returns 1 if the fault was an NX violation (and handled), 0 otherwise.
 */
int nx_enforce_check_fault(uint64_t cr2, uint64_t err,
                           struct interrupt_frame *frame) {
    if (!nx_self_active) return 0;

    /*
     * A true NX violation has:
     *   - err bit 0 = 1  (page was present, it's a protection fault)
     *   - err bit 4 = 1  (instruction fetch)
     *
     * Without bit 4 we can't distinguish NX from other protection faults.
     */
    if (!(err & (1ULL << 4))) return 0; /* not an instruction fetch */
    if (!(err & 1ULL)) return 0;        /* not-present, not NX */

    /* This is an NX violation */
    int user_fault = (err & (1ULL << 2)) ? 1 : 0;

    kprintf("\n*** NX VIOLATION ***\n");
    kprintf("  Fault address: 0x%llx\n", cr2);
    kprintf("  RIP:           0x%llx\n", frame->rip);
    kprintf("  Mode:          %s\n", user_fault ? "user" : "kernel");
    kprintf("  Region:        0x%llx is in %s\n",
            cr2, region_name(cr2));

    /* Print current process info if available */
    struct process *proc = process_get_current();
    if (proc) {
        kprintf("  Process:       %s (pid=%u)\n",
                proc->name ? proc->name : "?",
                (unsigned int)proc->pid);
    }

    if (user_fault) {
        /* Deliver SIGSEGV to the offending process */
        kprintf("[nx] Delivering SIGSEGV to pid=%u for NX violation\n",
                proc ? (unsigned int)proc->pid : 0);
        kprintf("  Full register state:\n");
        kprintf("  RAX=0x%llx  RBX=0x%llx  RCX=0x%llx  RDX=0x%llx\n",
                frame->rax, frame->rbx, frame->rcx, frame->rdx);
        kprintf("  RSI=0x%llx  RDI=0x%llx  R8=0x%llx   R9=0x%llx\n",
                frame->rsi, frame->rdi, frame->r8, frame->r9);
        kprintf("  R10=0x%llx  R11=0x%llx  R12=0x%llx  R13=0x%llx\n",
                frame->r10, frame->r11, frame->r12, frame->r13);
        kprintf("  R14=0x%llx  R15=0x%llx\n",
                frame->r14, frame->r15);
        kprintf("  RSP=0x%llx  RBP=0x%llx\n", frame->rsp, frame->rbp);

        /* Populate siginfo_t before terminating — provides fault address
         * and error code for signalfd listeners or user-space handlers. */
        if (proc) {
            struct siginfo sinfo;
            memset(&sinfo, 0, sizeof(sinfo));
            sinfo.si_signo = SIGSEGV;
            sinfo.si_code  = SEGV_ACCERR;  /* protection fault (NX) */
            sinfo.si_addr  = (void *)(uintptr_t)cr2;
            sinfo.si_pid   = proc->pid;
            sinfo.si_uid   = proc->uid;
            signal_send_info(proc->pid, SIGSEGV, &sinfo);
        }

        /* process_exit_code(11) kills the process with SIGSEGV */
        process_exit_code(11);
        return 1; /* does not return, but signal handler might */
    }

    /* Kernel-mode NX violation — panic with full state */
    {
        uint64_t cr0, cr2_val, cr3, cr4;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2_val));
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        kprintf("  CR0=0x%llx  CR2=0x%llx  CR3=0x%llx  CR4=0x%llx\n",
                (unsigned long long)cr0, (unsigned long long)cr2_val,
                (unsigned long long)cr3, (unsigned long long)cr4);
    }
    kprintf("  RAX=0x%llx  RBX=0x%llx  RCX=0x%llx  RDX=0x%llx\n",
            (unsigned long long)frame->rax, (unsigned long long)frame->rbx,
            (unsigned long long)frame->rcx, (unsigned long long)frame->rdx);
    kprintf("  RSI=0x%llx  RDI=0x%llx  R8=0x%llx   R9=0x%llx\n",
            (unsigned long long)frame->rsi, (unsigned long long)frame->rdi,
            (unsigned long long)frame->r8, (unsigned long long)frame->r9);
    kprintf("  R10=0x%llx  R11=0x%llx  R12=0x%llx  R13=0x%llx\n",
            (unsigned long long)frame->r10, (unsigned long long)frame->r11,
            (unsigned long long)frame->r12, (unsigned long long)frame->r13);
    kprintf("  R14=0x%llx  R15=0x%llx\n",
            (unsigned long long)frame->r14, (unsigned long long)frame->r15);
    kprintf("  RSP=0x%llx  RBP=0x%llx\n",
            (unsigned long long)frame->rsp, (unsigned long long)frame->rbp);
    kprintf("  CS=0x%llx  SS=0x%llx  RFLAGS=0x%llx\n",
            (unsigned long long)frame->cs, (unsigned long long)frame->ss,
            (unsigned long long)frame->rflags);

    arch_print_backtrace();

    panic("NX VIOLATION in kernel mode at RIP=0x%llx (fault addr=0x%llx)",
          (unsigned long long)frame->rip,
          (unsigned long long)cr2);
}
