/* numa_balancing.c — NUMA balancing: page placement, hint fault handling,
 * periodic scanning, and page migration. */

#define KERNEL_INTERNAL
#include "numa_balancing.h"
#include "cpu_topology.h"
#include "numa_mem.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "scheduler.h"
#include "timer.h"
#include "spinlock.h"
#include "debugfs.h"

/* ── Global state ──────────────────────────────────────────────────── */

static struct numa_node_stats g_node_stats[NUMA_MAX_NODES];
static spinlock_t g_numa_lock;

/* Scanner control */
static int __read_mostly g_numa_scan_enabled = 1;
static int g_numa_initialised = 0;

/* Hint fault tracking ring buffer (per-node, simple array) */
struct numa_hint_entry {
    uint64_t addr;         /* faulting virtual address */
    int      node;         /* NUMA node that accessed it */
    uint64_t tick;         /* timer tick when hint was recorded */
    int      in_use;
};

static struct numa_hint_entry g_hint_entries[NUMA_HINT_MAX];
static int g_hint_count = 0;

/* ── Migration cool-down: prevent bouncing ────────────────────────────
 *
 * When a page is migrated between NUMA nodes, we record the event.
 * If the scanner encounters the same physical page again within the
 * cool-down window, it skips migration.  This prevents "bouncing" —
 * repeatedly migrating a page back and forth for tasks that genuinely
 * access memory across multiple nodes (hot-move bouncing tasks).
 *
 * Tracking physical addresses rather than virtual addresses lets us
 * catch the same page even after virtual address reuse (COW, fork).
 */
#define NUMA_COOLDOWN_ENTRIES  32
#define NUMA_COOLDOWN_MS       (NUMA_SCAN_PERIOD_MS * 4)  /* 4 scan cycles */

struct numa_cooldown_entry {
    uint64_t phys;          /* physical address of migrated page */
    uint64_t migrate_tick;  /* timer tick when migration occurred */
};

static struct numa_cooldown_entry g_cooldown[NUMA_COOLDOWN_ENTRIES];
static int g_cooldown_count;
static uint64_t g_cooldown_ticks; /* cached cooldown window in timer ticks */

/* ── Forward declarations ──────────────────────────────────────────── */

static void numa_scan_work(void *arg);

/* ── Physical-address-to-NUMA-node helper ──────────────────────────── */
static int phys_to_node_id(uint64_t phys)
{
    for (int n = 0; n < NUMA_MAX_NODES; n++) {
        if (phys >= numa_node_memory_start[n] && phys < numa_node_memory_end[n])
            return n;
    }
    return -1;
}

/* ── Migration cool-down helpers ──────────────────────────────────── */

/* Check if a page is on cool-down (recently migrated). */
static int numa_on_cooldown(uint64_t phys, uint64_t now_tick)
{
    for (int i = 0; i < NUMA_COOLDOWN_ENTRIES; i++) {
        if (g_cooldown[i].phys == phys) {
            if (now_tick - g_cooldown[i].migrate_tick < g_cooldown_ticks)
                return 1; /* still on cool-down */
            /* Cool-down expired — clear the entry */
            g_cooldown[i].phys = 0;
            g_cooldown_count--;
            return 0;
        }
    }
    return 0;
}

/* Record a page migration in the cool-down tracker.
 * Must be called with g_numa_lock held (or in single-threaded context). */
static void numa_record_cooldown(uint64_t phys)
{
    uint64_t now_tick = timer_get_ticks();
    int oldest_idx = 0;
    uint64_t oldest_tick = now_tick;

    for (int i = 0; i < NUMA_COOLDOWN_ENTRIES; i++) {
        if (g_cooldown[i].phys == phys) {
            /* Update existing entry (same page migrated again) */
            g_cooldown[i].migrate_tick = now_tick;
            return;
        }
        if (g_cooldown[i].phys == 0) {
            /* Found empty slot */
            g_cooldown[i].phys = phys;
            g_cooldown[i].migrate_tick = now_tick;
            g_cooldown_count++;
            return;
        }
        /* Track oldest for replacement if array is full */
        if (g_cooldown[i].migrate_tick < oldest_tick) {
            oldest_tick = g_cooldown[i].migrate_tick;
            oldest_idx = i;
        }
    }

    /* Array full — replace oldest entry */
    g_cooldown[oldest_idx].phys = phys;
    g_cooldown[oldest_idx].migrate_tick = now_tick;
}

/* ── Initialisation ─────────────────────────────────────────────────── */

void numa_balancing_init(void)
{
    if (g_numa_initialised) return;

    memset(g_node_stats, 0, sizeof(g_node_stats));
    memset(g_hint_entries, 0, sizeof(g_hint_entries));
    memset(g_cooldown, 0, sizeof(g_cooldown));
    g_hint_count = 0;
    g_cooldown_count = 0;
    g_cooldown_ticks = (uint64_t)NUMA_COOLDOWN_MS / (1000 / TIMER_FREQ);
    if (g_cooldown_ticks < 1) g_cooldown_ticks = 1;
    spinlock_init(&g_numa_lock);

    g_numa_initialised = 1;

    /* Create scanner kthread that runs periodically */
    struct process *scanner = kthread_create(numa_scan_work, NULL,
                                             "numa_scan");
    if (scanner) {
        kprintf("[numa] NUMA balancing scanner kthread started (pid=%u)\n",
                (unsigned int)scanner->pid);
    } else {
        kprintf("[numa] WARNING: failed to start NUMA scanner kthread\n");
    }

    /* Register debugfs file */
    debugfs_create_file("numa_stats", numa_stats_read);

    kprintf("[numa] NUMA balancing initialised (%d nodes detected, "
            "scan period=%d ms, pages/cycle=%d)\n",
            numa_node_count > 0 ? numa_node_count : 1,
            NUMA_SCAN_PERIOD_MS, NUMA_SCAN_PAGES_PER_CYCLE);
}

/* ── Hint fault handler ───────────────────────────────────────────────
 *
 * Called from the page fault handler when a remote NUMA access is
 * detected.  Records the hint in a ring buffer for the scanner to
 * process later (migration is deferred to avoid overhead in the
 * fault path).
 */

void numa_hint_fault(uint64_t addr, int node)
{
    if (!g_numa_initialised) return;

    /* Clamp node to valid range */
    if (node < 0 || node >= NUMA_MAX_NODES) {
        if (numa_node_count > 0 && node >= numa_node_count)
            return;
        if (node < 0 || node >= NUMA_MAX_NODES)
            return;
    }

    uint64_t flags;
    spinlock_irqsave_acquire(&g_numa_lock, &flags);

    /* Increment remote access counter */
    g_node_stats[node].remote_accesses++;

    /* Record hint in ring buffer (FIFO replacement if full) */
    int idx = -1;

    /* First try to find an empty slot */
    for (int i = 0; i < NUMA_HINT_MAX; i++) {
        if (!g_hint_entries[i].in_use) {
            idx = i;
            break;
        }
    }

    /* If full, overwrite the oldest entry (round-robin) */
    if (idx < 0) {
        static int g_hint_next = 0;
        idx = g_hint_next;
        g_hint_next = (g_hint_next + 1) % NUMA_HINT_MAX;
    }

    g_hint_entries[idx].addr   = addr & ~(uint64_t)(PAGE_SIZE - 1);
    g_hint_entries[idx].node   = node;
    g_hint_entries[idx].tick   = timer_get_ticks();
    g_hint_entries[idx].in_use = 1;

    spinlock_irqsave_release(&g_numa_lock, flags);
}

/* ── Page migration ───────────────────────────────────────────────────
 *
 * Migrate a page to the target NUMA node.  Allocates a new frame on
 * the target node (via pmm_alloc_frame_on_node), copies the page
 * contents, records the migration in the cool-down tracker (to
 * prevent bouncing), and returns the new physical frame address.
 *
 * The caller is responsible for updating the page table entry to
 * point to the returned physical frame and for freeing the old page.
 *
 * Returns physical address of the new frame on success, 0 on failure.
 */

uint64_t numa_migrate_page(uint64_t page, int target_node)
{
    if (!g_numa_initialised) return 0;
    if (page == 0) return 0;
    if (target_node < 0 || target_node >= NUMA_MAX_NODES)
        return 0;

    /* Allocate new frame on the target NUMA node */
    uint64_t new_frame = pmm_alloc_frame_on_node(target_node);
    if (!new_frame)
        return 0;

    /* Copy page contents */
    void *src = PHYS_TO_VIRT(page);
    void *dst = PHYS_TO_VIRT(new_frame);
    memcpy(dst, src, PAGE_SIZE);

    /* Update statistics under lock */
    uint64_t flags;
    spinlock_irqsave_acquire(&g_numa_lock, &flags);
    g_node_stats[target_node].migrations_in++;

    /* Record migration in cool-down tracker to prevent bouncing */
    numa_record_cooldown(page);

    spinlock_irqsave_release(&g_numa_lock, flags);

    return new_frame;
}

/* ── Periodic scanner ─────────────────────────────────────────────────
 *
 * The scanner runs as a kernel thread with a configurable sleep interval.
 * Each cycle it:
 *   1. Drains the hint fault ring buffer
 *   2. Scans a portion of each user process's address space
 *   3. If a page is on a remote node relative to the process's home
 *      node, schedules it for migration
 *
 * Because this is a simple kernel without full reverse-mapping, we
 * approximate: we walk each process's page tables and check if the
 * phys page's NUMA node differs from the process's home node.
 */

static void numa_process_hint_entries(void)
{
    uint64_t flags;
    spinlock_irqsave_acquire(&g_numa_lock, &flags);

    /* Process each pending hint — in a full implementation we would
     * use these to trigger actual page migration.  For now we just
     * count them. */
    for (int i = 0; i < NUMA_HINT_MAX; i++) {
        if (g_hint_entries[i].in_use) {
            g_hint_entries[i].in_use = 0;
        }
    }

    spinlock_irqsave_release(&g_numa_lock, flags);
}

/* Scan one process's address space for remote NUMA pages.
 * Walks the user page table and checks if any present pages are
 * on a different NUMA node than the process's home_node.
 * Migrates remote pages to the home node, with cool-down protection
 * to prevent bouncing tasks from being hot-moved repeatedly. */
static void numa_scan_process(struct process *proc)
{
    if (!proc || !proc->pml4 || !proc->is_user)
        return;

    int home = proc->home_node;
    if (home < 0) home = 0; /* default to node 0 */

    uint64_t *pml4 = proc->pml4;
    int pages_scanned = 0;
    int pages_remote = 0;
    int pages_migrated = 0;

    /* Walk the user address space (canonical lower half) */
    for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        if (!(pml4[pml4_idx] & PTE_PRESENT))
            continue;

        uint64_t pdpt_phys = pml4[pml4_idx] & PTE_ADDR_MASK;
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pdpt_phys);

        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            if (!(pdpt[pdpt_idx] & PTE_PRESENT))
                continue;

            uint64_t pd_phys = pdpt[pdpt_idx] & PTE_ADDR_MASK;
            uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pd_phys);

            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                if (!(pd[pd_idx] & PTE_PRESENT))
                    continue;

                /* Check for 2MB huge page — skip migration for now
                 * (huge page migration is handled by a separate path) */
                if (pd[pd_idx] & PTE_HUGE) {
                    pages_scanned++;
                    if (pages_scanned >= NUMA_SCAN_PAGES_PER_CYCLE)
                        goto done;
                    continue;
                }

                uint64_t pt_phys = pd[pd_idx] & PTE_ADDR_MASK;
                uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pt_phys);

                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    if (!(pt[pt_idx] & PTE_PRESENT))
                        continue;

                    uint64_t phys = pt[pt_idx] & PTE_ADDR_MASK;
                    pages_scanned++;

                    /* Check if this page is on a remote NUMA node */
                    int page_node = phys_to_node_id(phys);
                    if (page_node >= 0 && page_node != home) {
                        pages_remote++;

                        /* Check cool-down to prevent bouncing */
                        uint64_t now_tick = timer_get_ticks();
                        uint64_t flags;
                        spinlock_irqsave_acquire(&g_numa_lock, &flags);
                        int cooldown = numa_on_cooldown(phys, now_tick);
                        spinlock_irqsave_release(&g_numa_lock, flags);

                        if (cooldown) {
                            /* Page was recently migrated — skip to avoid bounce */
                            if (pages_scanned >= NUMA_SCAN_PAGES_PER_CYCLE)
                                goto done;
                            continue;
                        }

                        /* Migrate the page to the process's home node */
                        uint64_t new_frame = numa_migrate_page(phys, home);
                        if (new_frame) {
                            /* Compute the virtual address for this PTE */
                            uint64_t virt = ((uint64_t)pml4_idx << 39) |
                                            ((uint64_t)pdpt_idx << 30) |
                                            ((uint64_t)pd_idx << 21) |
                                            ((uint64_t)pt_idx << 12);

                            /* Preserve original PTE flags, replace physical address */
                            uint64_t pte_flags = pt[pt_idx] & ~PTE_ADDR_MASK;
                            pt[pt_idx] = new_frame | pte_flags;

                            /* Flush the TLB entry for this virtual address */
                            __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");

                            /* Free the old frame */
                            pmm_free_frame(phys);

                            pages_migrated++;
                        }
                    }

                    if (pages_scanned >= NUMA_SCAN_PAGES_PER_CYCLE)
                        goto done;
                }
            }
        }
    }

done:
    if (pages_remote > 0 || pages_migrated > 0) {
        kprintf("[numa] pid %d: scanned %d pages, %d remote, %d migrated to node %d\n",
                (int)proc->pid, pages_scanned, pages_remote, pages_migrated, home);
    }
}

static void numa_scan_all_processes(void)
{
    struct process *table = process_get_table();
    if (!table) return;

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state != PROCESS_UNUSED &&
            table[i].is_user && table[i].pml4) {
            numa_scan_process(&table[i]);
        }
    }
}

/* Scanner kernel thread entry point */
static void numa_scan_work(void *arg)
{
    (void)arg;

    kprintf("[numa] Scanner thread started\n");

    while (g_numa_scan_enabled) {
        /* Drain hint entries */
        numa_process_hint_entries();

        /* Scan processes */
        numa_scan_all_processes();

        /* Sleep for one scan period */
        process_sleep_ticks(NUMA_SCAN_PERIOD_MS / (1000 / TIMER_FREQ));
    }
}

/* ── Control functions ──────────────────────────────────────────────── */

void numa_scan_set_enabled(int enabled)
{
    g_numa_scan_enabled = enabled;
}

int numa_scan_is_enabled(void)
{
    return g_numa_scan_enabled;
}

/* ── Stats access ───────────────────────────────────────────────────── */

const struct numa_node_stats *numa_get_node_stats(int node)
{
    if (node < 0 || node >= NUMA_MAX_NODES)
        return NULL;
    return &g_node_stats[node];
}

/* ── Debugfs callback ────────────────────────────────────────────────── */

void numa_stats_read(char *buf, int *len)
{
    int pos = 0;
    int max = 2048; /* buffer size, should be enough */

    int nodes = numa_node_count > 0 ? numa_node_count : 1;

    for (int n = 0; n < nodes; n++) {
        struct numa_node_stats *st = &g_node_stats[n];
        int remaining = max - pos;
        if (remaining <= 0) break;

        pos += snprintf(buf + pos, (size_t)(remaining > 0 ? remaining : 0),
                        "Node %d:\n"
                        "  local_allocations:  %llu\n"
                        "  remote_accesses:    %llu\n"
                        "  migrations_in:      %llu\n"
                        "  migrations_out:     %llu\n"
                        "  pages_placed:       %llu\n",
                        n,
                        (unsigned long long)st->local_allocations,
                        (unsigned long long)st->remote_accesses,
                        (unsigned long long)st->migrations_in,
                        (unsigned long long)st->migrations_out,
                        (unsigned long long)st->pages_placed);
    }

    *len = pos;
}

/* ── Stub: numa_balance ─────────────────────────────── */
static int numa_balance(void *task)
{
    (void)task;
    kprintf("[numa] numa_balance: not yet implemented\n");
    return 0;
}
/* ── Stub: numa_migrate ─────────────────────────────── */
static int numa_migrate(void *task, int src_node, int dst_node)
{
    (void)task;
    (void)src_node;
    (void)dst_node;
    kprintf("[numa] numa_migrate: not yet implemented\n");
    return 0;
}
/* ── Stub: numa_promote ─────────────────────────────── */
static int numa_promote(void *page)
{
    (void)page;
    kprintf("[numa] numa_promote: not yet implemented\n");
    return 0;
}
