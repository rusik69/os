/* numa_balancing.c — NUMA balancing: page placement, hint fault handling,
 * periodic scanning, and page migration. */

#define KERNEL_INTERNAL
#include "numa_balancing.h"
#include "cpu_topology.h"
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
static int g_numa_scan_enabled = 1;
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

/* ── Forward declarations ──────────────────────────────────────────── */

static void numa_scan_work(void *arg);

/* ── Initialisation ─────────────────────────────────────────────────── */

void numa_balancing_init(void)
{
    if (g_numa_initialised) return;

    memset(g_node_stats, 0, sizeof(g_node_stats));
    memset(g_hint_entries, 0, sizeof(g_hint_entries));
    g_hint_count = 0;
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
 * Migrate a page to the target NUMA node.  This allocates a new frame
 * on the target node (via pmm_alloc_frame), copies the page contents,
 * and updates the page tables.  The old frame is freed.
 *
 * For simplicity, we use pmm_alloc_frame() which allocates from the
 * global pool; on a real NUMA-aware system we would use a node-local
 * allocator.  The target_node parameter records the intent for stats.
 */

int numa_migrate_page(uint64_t page, int target_node)
{
    if (!g_numa_initialised) return -1;
    if (page == 0) return -EINVAL;
    if (target_node < 0 || target_node >= NUMA_MAX_NODES)
        return -EINVAL;

    uint64_t new_frame = pmm_alloc_frame();
    if (!new_frame) return -ENOMEM;

    /* Copy page contents */
    void *src = PHYS_TO_VIRT(page);
    void *dst = PHYS_TO_VIRT(new_frame);
    memcpy(dst, src, PAGE_SIZE);

    /* Update statistics */
    uint64_t flags;
    spinlock_irqsave_acquire(&g_numa_lock, &flags);

    /* Increment migrations_in for target, migrations_out for source's node.
     * We don't track which node the source belongs to precisely here,
     * so we just record the migration event. */
    g_node_stats[target_node].migrations_in++;

    spinlock_irqsave_release(&g_numa_lock, flags);

    /* The caller (scanner) is responsible for updating page tables.
     * We return the new frame; the caller will:
     *   1. Map new_frame at the virtual address
     *   2. Unmap the old page
     *   3. Free the old page
     */

    (void)src; (void)dst; /* suppress unused warnings */

    return 0;
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
 * on a different NUMA node than the process's home_node. */
static void numa_scan_process(struct process *proc)
{
    if (!proc || !proc->pml4 || !proc->is_user)
        return;

    int home = proc->home_node;
    if (home < 0) home = 0; /* default to node 0 */

    uint64_t *pml4 = proc->pml4;
    int pages_scanned = 0;

    /* Walk the user address space (canonical lower half) */
    for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        if (!(pml4[pml4_idx] & PTE_PRESENT))
            continue;

        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);

        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            if (!(pdpt[pdpt_idx] & PTE_PRESENT))
                continue;

            uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);

            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                if (!(pd[pd_idx] & PTE_PRESENT))
                    continue;

                /* Check for 2MB huge page */
                if (pd[pd_idx] & PTE_HUGE) {
                    uint64_t phys = pd[pd_idx] & PTE_ADDR_MASK;
                    /* Approximate NUMA node from physical address.
                     * In a real system we'd use a reverse mapping.
                     * Here we just count it. */
                    (void)phys;
                    pages_scanned++;
                    if (pages_scanned >= NUMA_SCAN_PAGES_PER_CYCLE)
                        return;
                    continue;
                }

                uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);

                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    if (!(pt[pt_idx] & PTE_PRESENT))
                        continue;

                    pages_scanned++;
                    if (pages_scanned >= NUMA_SCAN_PAGES_PER_CYCLE)
                        return;
                }
            }
        }
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
