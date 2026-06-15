/* thp.c — Transparent Huge Pages tracking + khugepaged daemon */

#include "thp.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "process.h"
#include "scheduler.h"
#include "timer.h"
#include "spinlock.h"

#define THP_MAX_PAGES 64

struct thp_entry {
    uint64_t virt_addr;    /* Virtual address of the huge page */
    uint64_t phys_addr;    /* Physical address of the huge page */
    int state;             /* THP_RAW, THP_SPLIT, or THP_PARTIAL */
    int present;
};

static struct thp_entry thp_entries[THP_MAX_PAGES];
static int thp_entry_count = 0;
static int thp_enabled = 0;
static uint64_t thp_total = 0;
static uint64_t thp_merged = 0;
static uint64_t thp_split = 0;

void thp_init(void) {
    memset(thp_entries, 0, sizeof(thp_entries));
    thp_entry_count = 0;
    thp_enabled = 1;
    thp_total = 0;
    thp_merged = 0;
    thp_split = 0;
    kprintf("[mem] THP (Transparent Huge Pages) tracking initialized\n");
}

void thp_set_enabled(int enabled) {
    thp_enabled = enabled;
    kprintf("[mem] THP %s\n", enabled ? "enabled" : "disabled");
}

int thp_is_enabled(void) {
    return thp_enabled;
}

int thp_track_hugepage(uint64_t virt_addr, uint64_t phys_addr) {
    if (!thp_enabled) return -1;
    if (thp_entry_count >= THP_MAX_PAGES) return -1;

    /* Check for duplicate */
    for (int i = 0; i < thp_entry_count; i++) {
        if (thp_entries[i].virt_addr == virt_addr && thp_entries[i].present)
            return 0; /* Already tracked */
    }

    struct thp_entry *entry = &thp_entries[thp_entry_count++];
    entry->virt_addr = virt_addr;
    entry->phys_addr = phys_addr;
    entry->state = THP_RAW;
    entry->present = 1;
    thp_total++;
    return 0;
}

void thp_untrack_hugepage(uint64_t virt_addr) {
    for (int i = 0; i < thp_entry_count; i++) {
        if (thp_entries[i].virt_addr == virt_addr && thp_entries[i].present) {
            thp_entries[i].present = 0;
            thp_total--;
            return;
        }
    }
}

int thp_split_hugepage(uint64_t virt_addr) {
    for (int i = 0; i < thp_entry_count; i++) {
        if (thp_entries[i].virt_addr == virt_addr && thp_entries[i].present) {
            if (thp_entries[i].state == THP_RAW) {
                thp_entries[i].state = THP_SPLIT;
                thp_split++;
                return 512; /* 2 MB / 4 KB = 512 pages */
            }
            return 0;
        }
    }
    return -1;
}

uint64_t thp_get_total_pages(void)  { return thp_total; }
uint64_t thp_get_merged_pages(void) { return thp_merged; }
uint64_t thp_get_split_pages(void)  { return thp_split; }

/* ═══════════════════════════════════════════════════════════════════════
 *  khugepaged — background 4K → 2MB huge page promotion daemon
 *
 *  Scans user process address spaces for contiguous 4K page ranges that
 *  can be coalesced into 2MB huge pages.  Uses a kernel thread that
 *  sleeps for a configurable interval between scan cycles.
 *
 *  Eligibility criteria for a 2MB-aligned range:
 *    1. All 512 PTEs are PRESENT
 *    2. All PTEs have identical protection flags (RWX, USER, etc.)
 *    3. Physical pages are contiguous (P[i+1] == P[i] + 4096)
 *    4. No PTE is a COW page (VMM_FLAG_COW) or lazy-zero page
 *    5. No PTE has refcount > 1 (shared via fork)
 *    6. The range is not already a huge page
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── Page table entry flag definitions (from vmm.c, duplicated for
 *    convenience to avoid including vmm.c internals) ──────────────── */
#define KHUGE_PTE_PRESENT  (1ULL << 0)
#define KHUGE_PTE_WRITE    (1ULL << 1)
#define KHUGE_PTE_USER     (1ULL << 2)
#define KHUGE_PTE_HUGE     (1ULL << 7)
#define KHUGE_PTE_COW      (1ULL << 9)   /* Software bit: COW */
#define KHUGE_PTE_LAZY     (1ULL << 10)  /* Software bit: lazy/demand */
#define KHUGE_PTE_NX       (1ULL << 63)
#define KHUGE_PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL
#define KHUGE_PDE_ADDR_MASK  0x000FFFFFFFE00000ULL  /* For 2MB pages */

/* User virtual address space limit (from vmm.h) */
#define KHUGE_USER_VADDR_MAX 0x0000800000000000ULL

/* ── Configuration ───────────────────────────────────────────────── */
static volatile int khugepaged_enabled = 0;
static volatile int khugepaged_sleep_ms = 1000;  /* 1 second default interval */
static struct process *khugepaged_thread = NULL;

/* ── Statistics ──────────────────────────────────────────────────── */
static uint64_t khugepaged_pages_scanned = 0;
static uint64_t khugepaged_pages_promoted = 0;
static uint64_t khugepaged_pages_failed = 0;

/* ── Lock for concurrent access to configuration/statistics ──────── */
static spinlock_t khugepaged_lock;

/* ── Public configuration API ────────────────────────────────────── */

void khugepaged_set_enabled(int enabled) {
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&khugepaged_lock, &irq_flags);
    khugepaged_enabled = enabled;
    spinlock_irqsave_release(&khugepaged_lock, irq_flags);
}

int khugepaged_is_enabled(void) {
    return khugepaged_enabled;
}

void khugepaged_set_sleep_ms(int ms) {
    if (ms < 10) ms = 10;   /* minimum 10 ms */
    if (ms > 60000) ms = 60000;  /* maximum 60 seconds */
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&khugepaged_lock, &irq_flags);
    khugepaged_sleep_ms = ms;
    spinlock_irqsave_release(&khugepaged_lock, irq_flags);
}

int khugepaged_get_sleep_ms(void) {
    return khugepaged_sleep_ms;
}

uint64_t khugepaged_get_scanned(void)   { return khugepaged_pages_scanned; }
uint64_t khugepaged_get_promoted(void)  { return khugepaged_pages_promoted; }
uint64_t khugepaged_get_failed(void)    { return khugepaged_pages_failed; }

/* ── Page-table walk helpers ─────────────────────────────────────── */

/*
 * Check whether a 2MB-aligned virtual address range has all 512 PTEs
 * that meet the eligibility criteria for promotion to a huge page.
 *
 * @pml4:   Process page table root (PML4)
 * @virt:   2MB-aligned virtual address to examine
 * @out_phys: [out] If eligible, set to the physical address of the
 *            first 4K page (guaranteed contiguous across all 512)
 * @out_flags: [out] Common PTE flags for the range
 *
 * Returns 1 if eligible, 0 if not (or on error).
 */
static int khugepaged_check_range(uint64_t *pml4, uint64_t virt,
                                   uint64_t *out_phys, uint64_t *out_flags)
{
    int pml4_idx = (int)((virt >> 39) & 0x1FF);
    int pdpt_idx = (int)((virt >> 30) & 0x1FF);
    int pd_idx   = (int)((virt >> 21) & 0x1FF);

    /* ── Walk to the page-directory entry ── */
    if (!(pml4[pml4_idx] & KHUGE_PTE_PRESENT))
        return 0;

    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & KHUGE_PTE_ADDR_MASK);
    if (!(pdpt[pdpt_idx] & KHUGE_PTE_PRESENT))
        return 0;

    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & KHUGE_PTE_ADDR_MASK);

    /* If already a huge page, nothing to do */
    if (pd[pd_idx] & KHUGE_PTE_HUGE)
        return 0;

    /* PD entry must point to a page table */
    if (!(pd[pd_idx] & KHUGE_PTE_PRESENT))
        return 0;

    uint64_t pt_phys = pd[pd_idx] & KHUGE_PTE_ADDR_MASK;
    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pt_phys);

    /* ── Examine all 512 PTEs ── */
    uint64_t flags = 0;
    uint64_t first_phys = 0;
    int flags_set = 0;

    for (int i = 0; i < 512; i++) {
        uint64_t pte = pt[i];

        /* Must be present */
        if (!(pte & KHUGE_PTE_PRESENT))
            return 0;

        /* Must not be a lazy/demand page (shared zero page) */
        if (pte & KHUGE_PTE_LAZY)
            return 0;

        /* Must not be COW — these pages are copy-on-write and promoting
         * them would break the sharing.  COW pages have refcount > 1. */
        if (pte & KHUGE_PTE_COW)
            return 0;

        uint64_t page_phys = pte & KHUGE_PTE_ADDR_MASK;

        /* Must not be the shared zero page */
        if (page_phys == 0)  /* Zero page is at a known phys, but 0 is safe filter */
            return 0;

        /* Refcount must be exactly 1 (not shared via fork) */
        if (pmm_refcount(page_phys) != 1)
            return 0;

        if (!flags_set) {
            /* First PTE: record flags and physical address */
            flags = pte & ~KHUGE_PTE_ADDR_MASK;
            first_phys = page_phys;
            flags_set = 1;
        } else {
            /* All PTEs must have identical protection flags */
            if ((pte & ~KHUGE_PTE_ADDR_MASK) != flags)
                return 0;

            /* Physical pages must be contiguous */
            if (page_phys != first_phys + (uint64_t)i * PAGE_SIZE)
                return 0;
        }
    }

    *out_phys  = first_phys;
    *out_flags = flags;
    return 1;
}

/*
 * Promote a 2MB-aligned 4K page range to a single 2MB huge page.
 *
 * Steps:
 *   1. Allocate 512 contiguous physical frames for the new huge page
 *   2. Copy data from the 512 existing 4K pages into the new 2MB page
 *   3. Install a huge-page PDE (with HUGE bit) in the page directory
 *   4. Free the original 4K page table and its pages
 *
 * @pml4:   Process page table root
 * @virt:   2MB-aligned virtual address
 * @old_phys: Physical address of the first existing 4K page
 * @flags:  Common PTE protection flags
 *
 * Returns 0 on success, -1 on failure.
 */
static int khugepaged_promote_range(uint64_t *pml4, uint64_t virt,
                                     uint64_t old_phys, uint64_t flags)
{
    int pd_idx = (int)((virt >> 21) & 0x1FF);

    /* ── Allocate 512 contiguous frames for the huge page ── */
    uint64_t huge_phys = (uint64_t)pmm_alloc_frames(512);
    if (!huge_phys) {
        /* Contiguous allocation failed — cannot promote */
        return -1;
    }

    /* ── Copy data from old 4K pages to new 2MB page ── */
    for (int i = 0; i < 512; i++) {
        uint64_t src_phys = old_phys + (uint64_t)i * PAGE_SIZE;
        uint64_t dst_phys = huge_phys + (uint64_t)i * PAGE_SIZE;
        memcpy((void *)PHYS_TO_VIRT(dst_phys),
               (void *)PHYS_TO_VIRT(src_phys), PAGE_SIZE);
    }

    /* ── Build the huge page PDE ──
     * The PDE physical address mask for 2MB pages is 0x000FFFFFFFE00000.
     * We keep the low 9 bits of protection flags (bits 8:0) plus the
     * HUGE bit and PRESENT bit.  The NX bit (bit 63) is kept as well. */
    uint64_t pde = (huge_phys & KHUGE_PDE_ADDR_MASK)
                   | (flags & 0x1FF)          /* low 9 flag bits */
                   | KHUGE_PTE_HUGE
                   | KHUGE_PTE_PRESENT;

    /* Preserve the NX bit from the original flags */
    if (flags & KHUGE_PTE_NX)
        pde |= KHUGE_PTE_NX;

    /* ── Locate the PD entry for this range ── */
    int pml4_idx = (int)((virt >> 39) & 0x1FF);
    int pdpt_idx = (int)((virt >> 30) & 0x1FF);

    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & KHUGE_PTE_ADDR_MASK);
    uint64_t *pd   = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & KHUGE_PTE_ADDR_MASK);

    /* ── Free the old page-table and its 4K pages ──
     * We must free the page-table page itself AND unref the data pages.
     * The PD currently points to the old PT; we capture its physical
     * address before overwriting the PDE. */
    uint64_t old_pt_phys = pd[pd_idx] & KHUGE_PTE_ADDR_MASK;

    /* Unref (and potentially free) all 512 original 4K data pages */
    for (int i = 0; i < 512; i++) {
        uint64_t page_phys = old_phys + (uint64_t)i * PAGE_SIZE;
        pmm_unref_frame(page_phys);
    }

    /* Free the page-table page itself (512 PTEs × 8 bytes = 4096 bytes) */
    pmm_free_frame(old_pt_phys);

    /* ── Install the huge page PDE ── */
    pd[pd_idx] = pde;

    /* ── Flush TLB for this virtual address range ──
     * We use invlpg for the start — hardware TLB will invalidate
     * the entire 2MB range as the PDE changed from 4K to 2MB. */
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");

    /* ── Track in THP subsystem ── */
    if (thp_enabled) {
        thp_track_hugepage(virt, huge_phys);
    }
    vm_hugepages++;
    khugepaged_pages_promoted++;

    return 0;
}

/*
 * Scan a single user process for promotable 4K page ranges.
 *
 * Walks the user address space in 2MB increments.  For each 2MB-aligned
 * chunk that has a present PD entry pointing to a PT (not a huge page),
 * checks whether all 512 PTEs can be coalesced.
 */
static void khugepaged_scan_process(struct process *proc)
{
    uint64_t *pml4 = proc->pml4;
    if (!pml4)
        return;

    /* Scan the full user address space in 2MB steps */
    for (uint64_t virt = 0;
         virt < KHUGE_USER_VADDR_MAX;
         virt += THP_HPAGE_SIZE) {

        /* Quick filter: check if the PML4/PDPT/PD entries exist */
        int pml4_idx = (int)((virt >> 39) & 0x1FF);
        int pdpt_idx = (int)((virt >> 30) & 0x1FF);
        int pd_idx   = (int)((virt >> 21) & 0x1FF);

        if (!(pml4[pml4_idx] & KHUGE_PTE_PRESENT)) {
            /* Skip the entire 512GB PML4 range */
            virt += (512ULL * 1024 * 1024 * 1024) - THP_HPAGE_SIZE;
            continue;
        }

        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & KHUGE_PTE_ADDR_MASK);
        if (!(pdpt[pdpt_idx] & KHUGE_PTE_PRESENT)) {
            /* Skip the entire 1GB PDPT range */
            virt += (512ULL * 1024 * 1024 * 1024 / 512) - THP_HPAGE_SIZE;
            continue;
        }

        uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & KHUGE_PTE_ADDR_MASK);

        /* If already a huge page, skip */
        if (pd[pd_idx] & KHUGE_PTE_HUGE)
            continue;

        /* PD entry must be present to have a PT below it */
        if (!(pd[pd_idx] & KHUGE_PTE_PRESENT))
            continue;

        /* ── Found a candidate 2MB range with a PT — check eligibility ── */
        uint64_t range_phys = 0;
        uint64_t range_flags = 0;

        if (!khugepaged_check_range(pml4, virt, &range_phys, &range_flags))
            continue;

        khugepaged_pages_scanned++;

        /* ── Attempt promotion ── */
        if (khugepaged_promote_range(pml4, virt, range_phys, range_flags) < 0) {
            khugepaged_pages_failed++;
        }
    }
}

/* ── Daemon entry point ──────────────────────────────────────────── */

static void khugepaged_daemon(void *arg)
{
    (void)arg;

    kprintf("[khugepaged] daemon started (scan interval=%d ms)\n",
            khugepaged_sleep_ms);

    while (khugepaged_enabled) {
        uint64_t scan_start = timer_get_ticks();
        uint64_t pages_before = khugepaged_pages_scanned;

        /* Scan all user processes */
        struct process *table = process_get_table();
        for (int i = 0; i < PROCESS_MAX; i++) {
            struct process *proc = &table[i];

            /* Skip unused processes, kernel threads, and processes
             * without their own page tables */
            if (proc->state == PROCESS_UNUSED)
                continue;
            if (!proc->is_user)
                continue;
            if (!proc->pml4)
                continue;

            khugepaged_scan_process(proc);
        }

        uint64_t scanned_this_cycle = khugepaged_pages_scanned - pages_before;
        uint64_t elapsed_ticks = timer_get_ticks() - scan_start;

        /* Convert sleep interval to timer ticks.
         * TIMER_FREQ = 100 ticks/sec, so ms → ticks = ms * 100 / 1000 */
        uint64_t sleep_ticks = (uint64_t)khugepaged_sleep_ms * TIMER_FREQ / 1000;

        /* If scan took longer than the interval, don't sleep */
        if (elapsed_ticks < sleep_ticks) {
            process_sleep_ticks(sleep_ticks - elapsed_ticks);
        } else {
            /* Yield to avoid busy-waiting */
            scheduler_yield();
        }

        /* Periodic progress log (quiet unless something happened) */
        if (scanned_this_cycle > 0) {
            kprintf("[khugepaged] scanned %llu ranges, promoted %llu total, "
                    "failed %llu total\n",
                    (unsigned long long)scanned_this_cycle,
                    (unsigned long long)khugepaged_pages_promoted,
                    (unsigned long long)khugepaged_pages_failed);
        }
    }

    kprintf("[khugepaged] daemon stopped\n");

    /* Loop forever (the thread should not exit; it's stopped by setting
     * khugepaged_enabled=0 and may be restarted).  We just idle. */
    for (;;) {
        scheduler_yield();
    }
}

/* ── Initialization ──────────────────────────────────────────────── */

void khugepaged_start(void)
{
    if (khugepaged_thread) {
        kprintf("[khugepaged] already running\n");
        return;
    }

    spinlock_init(&khugepaged_lock);
    khugepaged_enabled = 1;

    khugepaged_thread = kthread_create(khugepaged_daemon, NULL,
                                        "khugepaged");
    if (khugepaged_thread) {
        kprintf("[OK] khugepaged daemon created\n");
    } else {
        khugepaged_enabled = 0;
        kprintf("[!!] khugepaged: failed to create daemon thread\n");
    }
}
#include "module.h"
module_init(thp_init);
