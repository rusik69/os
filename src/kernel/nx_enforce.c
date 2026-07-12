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
#include "pmm.h"         /* pmm_alloc_frame for huge-page splitting */

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

/* Software-defined PTE bits (not in vmm.h) */
/* (none needed — hardware PTE bits now come from vmm.h) */

/* 2MB huge-page size */
#define HUGE_PAGE_SIZE (2ULL * 1024 * 1024)

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

    if (!(rdx & (1U << 20))) {
        kprintf("[NX] NX (No-Execute) not supported by CPU — skipping\n");
        return -1;
    }

    /* Enable NXE in IA32_EFER MSR (bit 11) */
    uint64_t efer = read_msr(0xC0000080);
    if (!(efer & EFER_NXE)) {
        efer |= EFER_NXE;
        write_msr(0xC0000080, efer);
        kprintf("[NX] EFER.NXE enabled\n");
    }

    nx_self_active = 1;

    /* Register the kernel .text section as the primary executable region */
    uint64_t text_start = (uint64_t)(uintptr_t)_text_start;
    uint64_t text_end   = (uint64_t)(uintptr_t)_text_end;
    nx_enforce_register_region(text_start, text_end);

    kprintf("[NX] Kernel .text : 0x%llx – 0x%llx  (%llu KB)\n",
            text_start, text_end, (text_end - text_start) / 1024);
    kprintf("[NX] Kernel .rodata: 0x%llx – 0x%llx\n",
            (uint64_t)(uintptr_t)_rodata_start,
            (uint64_t)(uintptr_t)_rodata_end);
    kprintf("[NX] Kernel .data  : 0x%llx – 0x%llx\n",
            (uint64_t)(uintptr_t)_data_start,
            (uint64_t)(uintptr_t)_data_end);
    kprintf("[NX] Kernel .bss   : 0x%llx – 0x%llx\n",
            (uint64_t)(uintptr_t)_bss_start,
            (uint64_t)(uintptr_t)_bss_end);
    kprintf("[NX] NX enforcement initialised successfully\n");

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
        kprintf("[NX] WARNING: exec region table full (max %d)\n",
                NX_ENFORCE_MAX_REGIONS);
        return -1;
    }
    exec_regions[nr_exec_regions].start = start;
    exec_regions[nr_exec_regions].end   = end;
    nr_exec_regions++;
    kprintf("[NX] Registered exec region [%d]: 0x%llx – 0x%llx\n",
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
    kprintf("[NX] Registered executable regions (%d):\n", nr_exec_regions);
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
 * Returns 0 on success (correct), 1 on violation (logged).
 * When *suppress is non-zero, individual violation messages are skipped
 * (only the count is accumulated) so boot-time serial floods are avoided. */
static int check_pte(uint64_t vaddr, uint64_t pte, int level, int *suppress) {
    if (!(pte & PTE_PRESENT)) return 0;
    /* Skip user-accessible pages — they belong to user processes and
     * are audited separately (e.g. vDSO/vsyscall is intentionally
     * executable and not part of the kernel .text region). */
    if (pte & PTE_USER) return 0;

    int has_nx  = (pte & PTE_NX) ? 1 : 0;
    int is_exec = !has_nx;
    int should_exec = nx_enforce_is_executable(vaddr);

    if (is_exec && !should_exec) {
        if (!suppress || *suppress > 0) {
            kprintf("[NX] AUDIT: %s page 0x%llx (L%d) is EXECUTABLE but is in %s"
                    " — missing NX!\n",
                    region_name(vaddr), vaddr, level, region_name(vaddr));
            if (suppress && *suppress > 0) (*suppress)--;
        }
        return 1;
    }

    if (!is_exec && should_exec) {
        kprintf("[NX] AUDIT: .text page 0x%llx (L%d) has NX set but"
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
    /* Suppress individual violation messages after the first few to avoid
     * flooding the serial console at boot (the protection pass immediately
     * after the audit will fix them all). */
    int suppress = 20;
    uint64_t *pml4 = kernel_pml4;
    if (!pml4) {
        kprintf("[NX] audit: kernel_pml4 is NULL — cannot audit\n");
        return -1;
    }

    for (int pml4_idx = 0; pml4_idx < 512; pml4_idx++) {
        if (!(pml4[pml4_idx] & PTE_PRESENT))
            continue;

        /* Sign-extend the 48-bit virtual address to 64-bit canonical form.
         * PML4 indices 0-255 are user-space (bit 47 = 0 → upper bits 0),
         * indices 256-511 are kernel-space (bit 47 = 1 → upper bits 1). */
        uint64_t pml4_vbase = (uint64_t)pml4_idx << 39;
        if (pml4_idx >= 256)
            pml4_vbase |= 0xFFFF000000000000ULL;
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);

        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            if (!(pdpt[pdpt_idx] & PTE_PRESENT))
                continue;

            uint64_t pdpt_vbase = pml4_vbase | ((uint64_t)pdpt_idx << 30);

            /* 1 GB huge page? */
            if (pdpt[pdpt_idx] & PTE_HUGE) {
                violations += check_pte(pdpt_vbase, pdpt[pdpt_idx], 2, &suppress);
                continue;
            }

            uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);

            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                if (!(pd[pd_idx] & PTE_PRESENT))
                    continue;

                uint64_t pd_vbase = pdpt_vbase | ((uint64_t)pd_idx << 21);

                /* 2 MB huge page? */
                if (pd[pd_idx] & PTE_HUGE) {
                    violations += check_pte(pd_vbase, pd[pd_idx], 1, &suppress);
                    continue;
                }

                uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);

                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    if (!(pt[pt_idx] & PTE_PRESENT))
                        continue;

                    uint64_t pt_vbase = pd_vbase | ((uint64_t)pt_idx << 12);
                    violations += check_pte(pt_vbase, pt[pt_idx], 0, &suppress);
                }
            }
        }
    }

    if (violations == 0) {
        kprintf("[NX] audit: OK — all kernel pages have correct NX settings\n");
    } else {
        kprintf("[NX] audit: %d NX violation(s) found in kernel page tables!"
                " (showing first %d)\n",
                violations, 1);
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
                    kprintf("[NX] user-audit: executable 2MB page at 0x%llx"
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
            kprintf("[NX] user-audit: executable user page at 0x%llx"
                    " not in exec region\n", vaddr);
            violations++;
        }
    }

    return violations;
}

/* ── Kernel section permission protection (Item 176) ────────────────── */

/*
 * Split a 2MB huge page into 512 × 4KB pages so we can set different
 * permissions on individual pages within a 2MB region.
 *
 * @pml4      Kernel PML4 (virtual address).
 * @pd_vaddr  Virtual address of the PDE covering the huge page.
 * @pd_phys   Physical address base of the huge page.
 * @pd_flags  Current PDE flags (used to set consistent permissions).
 * Returns 0 on success, -1 on failure.
 */
static int split_2mb_huge_page(uint64_t *pml4, uint64_t pd_vaddr,
                                uint64_t pd_phys, uint64_t pd_flags)
{
    int pml4_idx = (pd_vaddr >> 39) & 0x1FF;
    int pdpt_idx = (pd_vaddr >> 30) & 0x1FF;
    int pd_idx   = (pd_vaddr >> 21) & 0x1FF;

    if (!(pml4[pml4_idx] & PTE_PRESENT)) return -1;
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return -1;
    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);
    if (!(pd[pd_idx] & PTE_PRESENT)) return -1;
    if (!(pd[pd_idx] & PTE_HUGE)) return 0; /* not a huge page — nothing to do */

    /* Allocate a 4KB page table page */
    uint64_t pt_phys = pmm_alloc_frame();
    if (!pt_phys) {
        kprintf("[NX] FAILED to allocate page-table page for huge-page split at 0x%llx\n",
                pd_vaddr);
        return -1;
    }
    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pt_phys);
    memset(pt, 0, PAGE_SIZE);

    /* Fill the page table: map each 4KB sub-page with the huge page's flags,
     * but strip the PTE_HUGE flag (this is a leaf PTE now) and preserve the
     * accessed/dirty bits as zero since these will be set by hardware. */
    uint64_t base_flags = (pd_flags & ~(PTE_HUGE | PTE_ACCESSED | PTE_DIRTY)) | PTE_PRESENT;
    for (int i = 0; i < 512; i++) {
        pt[i] = (pd_phys + (uint64_t)i * PAGE_SIZE) | base_flags;
    }

    /* Point the PDE to the new page table (clear the huge bit) */
    pd[pd_idx] = pt_phys | (pd_flags & ~(PTE_HUGE | PTE_ADDR_MASK)) | PTE_PRESENT;

    /* Flush the TLB for the affected 2MB region */
    for (uint64_t v = pd_vaddr; v < pd_vaddr + HUGE_PAGE_SIZE; v += PAGE_SIZE) {
        __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
    }

    return 0;
}

/*
 * Walk the kernel page tables and apply fine-grained permissions:
 *   - .rodata:  clear write bit (read-only)
 *   - .data:    set NX bit    (no execute)
 *   - .bss:     set NX bit    (no execute)
 *
 * 2MB huge pages that span across section boundaries are split into 4KB
 * pages first so each section gets correct per-page permissions.
 *
 * Must be called after all early init code that modifies kernel sections
 * (e.g. module loader init, syscall table patching) is complete.
 */
int nx_enforce_protect_kernel_sections(void)
{
    if (!nx_self_active) {
        kprintf("[NX] section protection: NX not active — skipping\n");
        return -1;
    }

    uint64_t *pml4 = kernel_pml4;
    if (!pml4) {
        kprintf("[NX] section protection: kernel_pml4 is NULL\n");
        return -1;
    }

    uint64_t rodata_start = (uint64_t)(uintptr_t)_rodata_start;
    uint64_t rodata_end   = (uint64_t)(uintptr_t)_rodata_end;
    uint64_t data_start   = (uint64_t)(uintptr_t)_data_start;
    uint64_t data_end     = (uint64_t)(uintptr_t)_data_end;
    uint64_t bss_start    = (uint64_t)(uintptr_t)_bss_start;
    uint64_t bss_end      = (uint64_t)(uintptr_t)_bss_end;

    int ro_modified = 0;
    int nx_modified = 0;
    int splits      = 0;

    for (int pml4_idx = 0; pml4_idx < 512; pml4_idx++) {
        if (!(pml4[pml4_idx] & PTE_PRESENT)) continue;

        /* Sign-extend the 48-bit virtual address to 64-bit canonical form. */
        uint64_t pml4_vbase = (uint64_t)pml4_idx << 39;
        if (pml4_idx >= 256)
            pml4_vbase |= 0xFFFF000000000000ULL;
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);

        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            if (!(pdpt[pdpt_idx] & PTE_PRESENT)) continue;

            uint64_t pdpt_vbase = pml4_vbase | ((uint64_t)pdpt_idx << 30);

            /* 1 GB pages — skip (kernel sections are in low 2MB). */
            if (pdpt[pdpt_idx] & PTE_HUGE) continue;

            uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);

            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                if (!(pd[pd_idx] & PTE_PRESENT)) continue;

                uint64_t pd_vbase = pdpt_vbase | ((uint64_t)pd_idx << 21);
                uint64_t pd_flags = pd[pd_idx];
                int       is_2mb  = !!(pd_flags & PTE_HUGE);

                /* Determine which section(s) this 2MB region overlaps */
                uint64_t region_end = pd_vbase + HUGE_PAGE_SIZE;

                int overlaps_rodata = pd_vbase < rodata_end && region_end > rodata_start;
                int overlaps_data   = pd_vbase < data_end   && region_end > data_start;
                int overlaps_bss    = pd_vbase < bss_end    && region_end > bss_start;
                int overlaps_text   = pd_vbase < (uint64_t)(uintptr_t)_text_end &&
                                      region_end > (uint64_t)(uintptr_t)_text_start;

                /* If this 2MB page spans multiple sections, split it */
                int section_count = overlaps_rodata + overlaps_data +
                                    overlaps_bss + overlaps_text;
                if (is_2mb && section_count > 1) {
                    uint64_t phys = pd_flags & PTE_ADDR_MASK;
                    if (split_2mb_huge_page(pml4, pd_vbase, phys, pd_flags) < 0) {
                        kprintf("[NX] WARNING: failed to split 2MB page at 0x%llx\n",
                                pd_vbase);
                        continue;
                    }
                    splits++;
                    /* Re-read the PDE (now a non-huge pointer to a page table) */
                    pd[pd_idx] = (pd[pd_idx] & ~PTE_HUGE);
                    pd_flags = pd[pd_idx];
                    is_2mb = 0;
                }

                if (is_2mb) {
                    /* Whole 2MB region belongs to one section */
                    if (overlaps_rodata && (pd_flags & PTE_WRITE)) {
                        pd[pd_idx] = pd_flags & ~(uint64_t)PTE_WRITE;
                        ro_modified++;
                    } else if ((overlaps_data || overlaps_bss) && !(pd_flags & PTE_NX)) {
                        pd[pd_idx] = pd_flags | PTE_NX;
                        nx_modified++;
                    } else if (!overlaps_text && pd_vbase >= 0xFFFF800000000000ULL && !(pd_flags & PTE_NX)) {
                        pd[pd_idx] = pd_flags | PTE_NX;
                        nx_modified++;
                    }
                    continue;
                }

                /* 4KB page-level: walk the page table and apply per-page permissions */
                uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);

                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    if (!(pt[pt_idx] & PTE_PRESENT)) continue;

                    uint64_t vaddr = pd_vbase | ((uint64_t)pt_idx << 12);
                    uint64_t pte   = pt[pt_idx];

                    /* .rodata → clear write bit */
                    if (vaddr >= rodata_start && vaddr < rodata_end) {
                        if (pte & PTE_WRITE) {
                            pt[pt_idx] = pte & ~(uint64_t)PTE_WRITE;
                            ro_modified++;
                            __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
                        }
                    }
                    /* .data and .bss → set NX bit */
                    else if ((vaddr >= data_start && vaddr < data_end) ||
                             (vaddr >= bss_start  && vaddr < bss_end)) {
                        if (!(pte & PTE_NX)) {
                            pt[pt_idx] = pte | PTE_NX;
                            nx_modified++;
                            __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
                        }
                    }
                    /* .text → skip (keep executable) */
                    else if (vaddr >= (uint64_t)(uintptr_t)_text_start &&
                             vaddr <  (uint64_t)(uintptr_t)_text_end) {
                        /* leave executable */
                    }
                    /* All other kernel pages (heap, MMIO, .ksymtab, .modinfo, etc.) → set NX */
                    else if (vaddr >= 0xFFFF800000000000ULL && !(pte & PTE_NX)) {
                        pt[pt_idx] = pte | PTE_NX;
                        nx_modified++;
                        __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
                    }
                }
            }
        }
    }

    kprintf("[NX] section protection complete: %d pages set read-only, "
            "%d pages set NX, %d huge-page split(s)\n",
            ro_modified, nx_modified, splits);

    return 0;
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
        kprintf("[NX] Delivering SIGSEGV to pid=%u for NX violation\n",
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
            signal_send_info(proc->pid, SIGSEGV, &sinfo, 0);
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

/* ── Stub: nx_enforce ─────────────────────────────── */
int nx_enforce(void *task)
{
    (void)task;
    kprintf("[NX] nx_enforce: not yet implemented\n");
    return 0;
}
/* ── Stub: nx_check_addr ─────────────────────────────── */
static int nx_check_addr(uint64_t addr)
{
    (void)addr;
    kprintf("[NX] nx_check_addr: not yet implemented\n");
    return 0;
}
/* ── Stub: nx_set_prot ─────────────────────────────── */
static int nx_set_prot(uint64_t addr, size_t len, int prot)
{
    (void)addr;
    (void)len;
    (void)prot;
    kprintf("[NX] nx_set_prot: not yet implemented\n");
    return 0;
}
