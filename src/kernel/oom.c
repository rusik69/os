#define KERNEL_INTERNAL
#include "types.h"
#include "oom.h"
#include "process.h"
#include "scheduler.h"
#include "printf.h"
#include "pmm.h"
#include "vmm.h"
#include "signal.h"
#include "timer.h"
#include "syscall.h"
#include "dcache.h"
#include "page_cache.h"
#include "process_rlimit.h"
#include "vfs.h"
#include "workqueue.h"
#include "string.h"
#include "spinlock.h"

/* OOM score adjustment table (by PID) — extended range for OOM_SCORE_ADJ_MIN = -1000 */
#define OOM_ADJ_MIN  (-1000)
#define OOM_ADJ_MAX  1000

static int16_t oom_score_adj_table[PROCESS_MAX];

void oom_set_score_adj(int pid, int16_t adjustment) {
    if (pid < 0 || pid >= PROCESS_MAX) return;
    if (adjustment < OOM_ADJ_MIN) adjustment = OOM_ADJ_MIN;
    if (adjustment > OOM_ADJ_MAX) adjustment = OOM_ADJ_MAX;
    oom_score_adj_table[pid] = adjustment;
}

int16_t oom_get_score_adj(int pid) {
    if (pid < 0 || pid >= PROCESS_MAX) return 0;
    return oom_score_adj_table[pid];
}

uint64_t oom_kill_count = 0;

/*
 * ── Count clean page cache pages accessible by this process ──────────
 *
 * Iterates the process's open file descriptors and sums the page cache
 * pages for each inode that are clean (not dirty).  These pages are
 * "low-hanging fruit" — killing the process frees them immediately
 * without needing to write back dirty data first.
 */
static uint64_t oom_count_clean_cache_pages(struct process *p)
{
    if (!p || !p->is_user)
        return 0;

    uint64_t total_clean = 0;
    uint64_t seen_inos[16];
    int n_seen = 0;

    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!p->fd_table[i].used)
            continue;

        /* stat the path to get inode number */
        struct vfs_stat st;
        if (vfs_stat(p->fd_table[i].path, &st) < 0 || st.ino == 0)
            continue;

        /* Deduplicate inodes (multiple fds may point to same file) */
        int already = 0;
        for (int j = 0; j < n_seen; j++) {
            if (seen_inos[j] == st.ino) {
                already = 1;
                break;
            }
        }
        if (already) continue;
        if (n_seen < 16)
            seen_inos[n_seen++] = st.ino;

        /* Count clean (non-dirty) page cache pages for this inode */
        total_clean += page_cache_count_clean(st.ino);
    }

    return total_clean;
}

/*
 * ── OOM scoring ──────────────────────────────────────────────────────
 *
 * Score a process based on how much memory it uses and how valuable it is.
 * Higher score = more likely to be killed.
 *
 * Scoring factors (higher = more likely to die):
 *   1. Actual resident pages (RSS) from page table walk       ─  weight ×10
 *   2. swap_pages (swapped-out pages)                          ─  weight ×8
 *   3. Memory limit ratio (rss+swap)/RLIMIT_RSS               ─  ×50 scaling
 *   4. Writability ratio (writable pages)                      ─  writable pages are harder to reclaim
 *   5. Clean page cache pages (instant-reclaim)                ─  bonus for easy reclamation
 *   6. Shared/reclaimable (COW/LAZY) page bonus                ─  low-hanging fruit
 *   7. Estimated total freed pages                             ─  bonus
 *   8. Children count                                          ─  +5 each
 *   9. Suspended/background processes                          ─  +5 bonus
 *  10. Low priority / high nice value                          ─  +priority*3
 *  11. Long-running processes get age discount                 ─  -1 per 10s after 60s
 *  12. Kernel threads (no user pml4) score lower               ─  they don't free user pages
 *  13. OOM score adjustment from /proc interface               ─  user-controlled
 */
int64_t oom_score_process(uint32_t pid) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    int64_t score = 0;

    /* ── Resident memory (actual page table walk) ───────────────── */
    uint64_t rss_pages = 0;
    uint64_t dirty_pages = 0;   /* actually: writable pages */
    uint64_t shared_pages = 0;  /* COW + lazy + zero-page pages */

    if (p->is_user && p->pml4) {
        rss_pages = vmm_count_user_pages(p->pml4, &dirty_pages, &shared_pages);
    }

    uint64_t clean_pages = (rss_pages > dirty_pages) ? (rss_pages - dirty_pages) : 0;

    /* ── Swap usage ──────────────────────────────────────────────── */
    uint64_t swap_pages = p->swap_pages;

    /* ── Raw memory footprint (RSS + swap) ──────────────────────── */
    uint64_t total_pages = rss_pages + swap_pages;

    /* ── Total virtual address space (including non-resident mappings) ── */
    /* Add total_vm * 0.3: count every mapped (even if not resident) page
     * at 30% weight. A process with large mmap'd files or sparse mappings
     * gets a proportional score bump. */
    uint64_t total_vm_pages = 0;
    if (p->is_user && p->pml4) {
        total_vm_pages = vmm_count_user_pages_range(p->pml4, 0, USER_VADDR_MAX, NULL, NULL);
    }
    score += (int64_t)(total_vm_pages * 30 / 100);  /* total_vm * 0.3 */

    /* Base score: raw footprint */
    score += (int64_t)(total_pages) * 10;

    /* Writable/dirty pages contribute extra (they may require swap-out) */
    score += (int64_t)dirty_pages * 3;

    /* Clean pages (non-writable) are somewhat reclaimable */
    score += (int64_t)clean_pages;

    /* ── Memory pressure ratio: score proportional to usage/limit ── */
    uint64_t rss_limit = p->rlim_cur[RLIMIT_RSS];
    if (rss_limit == 0 || rss_limit == ~0ULL)
        rss_limit = p->rlim_cur[RLIMIT_AS];
    if (rss_limit > PAGE_SIZE && total_pages > 0 && rss_limit != ~0ULL) {
        uint64_t limit_pages = rss_limit / PAGE_SIZE;
        if (limit_pages > 0) {
            /* Ratio: (total_pages * 100) / limit_pages gives a percentage.
             * Scale that by 50 to produce the bonus. */
            uint64_t ratio_x100 = (total_pages * 100) / limit_pages;
            if (ratio_x100 > 100) ratio_x100 = 100;
            if (ratio_x100 > 10)  /* only apply if >10% of limit used */
                score += (int64_t)(ratio_x100 * 50) / 100;
        }
    }

    /* ── Low-hanging memory: shared/COW/lazy pages are easily reclaimed ── */
    if (rss_pages > 0) {
        uint64_t reclaimable = shared_pages + clean_pages;
        /* Processes with >70% reclaimable pages get a bonus */
        if ((reclaimable * 100) / rss_pages > 70)
            score += (int64_t)reclaimable * 2;
    }

    /* ── Clean page cache pages (instant-reclaim) ────────────────── */
    uint64_t clean_cache = oom_count_clean_cache_pages(p);
    if (clean_cache > 0) {
        /* Each clean cache page is a bonus — killing this process
         * would free these pages with zero I/O cost */
        score += (int64_t)(clean_cache / 4);  /* scaled down, cache may be shared */
    }

    /* ── Estimated total freed pages ─────────────────────────────── */
    uint64_t estimated_freed = rss_pages + swap_pages + clean_cache;
    if (estimated_freed > 0) {
        int64_t freed_bonus = (int64_t)(estimated_freed / 10);
        if (freed_bonus > 500) freed_bonus = 500;
        score += freed_bonus;
    }

    /* Kernel threads: they have no user memory to free,
     * so score them lower unless they're huge consumers. */
    if (!p->is_user || !p->pml4) {
        if (score > 50) score = 50;
    }

    /* ── Process tree weight ────────────────────────────────────── */
    int kids = 0;
    struct process *tp = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (tp[i].state != PROCESS_UNUSED && tp[i].parent_pid == p->pid)
            kids++;
    }
    score += kids * 5;

    /* ── State-based adjustments ─────────────────────────────────── */
    if (p->is_suspended) score += 10;     /* suspended = good victim */
    if (p->is_background) score += 5;     /* background = better */
    if (p->is_user)       score -= 10;    /* user processes are more valuable than kernel */

    /* ── Priority / nice ─────────────────────────────────────────── */
    score += p->priority * 3;             /* lower priority = better victim */

    /* Nice value: higher nice = more likely to be killed */
    int nice_adj = p->nice - NICE_DEFAULT;
    if (nice_adj > 0) score += nice_adj * 2;

    /* ── Age discount: long-running processes are more important ─── */
    uint64_t age_ticks = timer_get_ticks() - p->start_time_tick;
    if (age_ticks > TIMER_FREQ * 60)  /* older than 60 seconds */
        score -= (int64_t)(age_ticks / (TIMER_FREQ * 10)); /* -1 per 10s */

    /* ── Apply OOM score adjustment ──────────────────────────────── */
    score += oom_get_score_adj((int)p->pid);

    return score;
}

/*
 * Estimate how many physical pages would be freed if this process were killed.
 * This is used for victim selection and logging.
 */
uint64_t oom_estimate_freed_pages(uint32_t pid)
{
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return 0;

    uint64_t rss_pages = 0;
    if (p->is_user && p->pml4)
        rss_pages = vmm_count_user_pages(p->pml4, NULL, NULL);

    uint64_t swap_pages = p->swap_pages;
    uint64_t clean_cache = oom_count_clean_cache_pages(p);

    return rss_pages + swap_pages + clean_cache;
}

static int is_oom_safe(struct process *p) {
    if (!p || p->state == PROCESS_UNUSED) return 1;
    /* Never kill PID 1 (init), idle (0), or the current process */
    if (p->pid == 1 || p->pid == 0) return 1;
    struct process *cur = process_get_current();
    if (cur && p->pid == cur->pid) return 1;
    /* Protect kernel threads — they have no user memory to free */
    if (!p->is_user) return 1;
    /* Protect processes with OOM_SCORE_ADJ_MIN = -1000 (immune) */
    if (oom_get_score_adj((int)p->pid) <= OOM_SCORE_ADJ_MIN + 1) return 1;
    return 0;
}

/* ── OOM Reaper forward declarations ────────────────────────────────── */
#define OOM_VICTIM_MAX 16
static struct {
    uint32_t pid;
    int      in_use;
} oom_victim_table[OOM_VICTIM_MAX];
static spinlock_t oom_victim_lock;

static void oom_victim_add(uint32_t pid);
static void oom_victim_remove(uint32_t pid);
static void oom_reap_process(void *arg);

static uint32_t select_victim(void) {
    struct process *table = process_get_table();
    uint32_t victim_pid = 0;
    int64_t best_score = -1;

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED) continue;
        if (is_oom_safe(&table[i])) continue;
        int64_t score = oom_score_process(table[i].pid);
        if (score > best_score) {
            best_score = score;
            victim_pid = table[i].pid;
        }
    }

    return victim_pid;
}

int oom_kill(uint64_t needed_pages) {
    (void)needed_pages;

    /* Before killing a process, try reclaiming the dentry cache — it's cheap. */
    int evicted = dcache_shrink(DCACHE_SIZE);
    if (evicted > 0) {
        kprintf("[OOM] Freed %d dentry cache entries, retrying allocation...\n", evicted);
        return 1;  /* caller may retry allocation */
    }

    uint32_t victim_pid = select_victim();
    if (victim_pid == 0) {
        kprintf("[OOM] No suitable victim found!\n");
        return 0;
    }

    struct process *victim = process_get_by_pid(victim_pid);
    uint64_t rss = 0;
    uint64_t swap = 0;
    uint64_t clean_cache = 0;
    uint64_t estimated_freed = 0;
    if (victim) {
        if (victim->is_user && victim->pml4)
            rss = vmm_count_user_pages(victim->pml4, NULL, NULL);
        swap = victim->swap_pages;
        clean_cache = oom_count_clean_cache_pages(victim);
        estimated_freed = rss + swap + clean_cache;
    }

    kprintf("[OOM] Killing pid=%u \"%s\" (score=%lld, rss=%llu, swap=%llu, "
            "cache=%llu, freed_est=%llu, %s, prio=%u)\n",
            victim_pid, victim ? victim->name : "?",
            (long long)(victim ? oom_score_process(victim_pid) : -1),
            (unsigned long long)rss,
            (unsigned long long)swap,
            (unsigned long long)clean_cache,
            (unsigned long long)estimated_freed,
            (victim && victim->is_user) ? "user" : "kernel",
            victim ? (uint32_t)victim->priority : 0);

    /* Send SIGKILL — signal handler in the target process will cause exit */
    signal_send(victim_pid, SIGKILL);

    oom_kill_count++;

    /* Register the victim for async reaping and queue workqueue item.
     * The reaper will walk the victim's page tables and free pages
     * immediately, without waiting for the victim process to run and
     * exit naturally.  This allows the OOM-triggering allocation to
     * retry sooner. */
    oom_victim_add(victim_pid);
    workqueue_schedule(oom_reap_process, (void *)(uintptr_t)victim_pid);

    /* Brief yield so the killed process can exit and free memory */
    scheduler_yield();
    return 1;
}

/* Kill the highest-scored process. Returns 1 if a process was killed, 0 otherwise. */
int oom_kill_victim(void) {
    uint32_t victim_pid = select_victim();
    if (victim_pid == 0) {
        return 0;
    }

    struct process *victim = process_get_by_pid(victim_pid);
    uint64_t rss = 0;
    uint64_t swap = 0;
    uint64_t clean_cache = 0;
    uint64_t estimated_freed = 0;
    if (victim) {
        if (victim->is_user && victim->pml4)
            rss = vmm_count_user_pages(victim->pml4, NULL, NULL);
        swap = victim->swap_pages;
        clean_cache = oom_count_clean_cache_pages(victim);
        estimated_freed = rss + swap + clean_cache;
    }

    kprintf("[OOM] Killing pid=%u \"%s\" (score=%lld, rss=%llu, swap=%llu, "
            "cache=%llu, freed_est=%llu)\n",
            victim_pid, victim ? victim->name : "?",
            (long long)(victim ? oom_score_process(victim_pid) : -1),
            (unsigned long long)rss,
            (unsigned long long)swap,
            (unsigned long long)clean_cache,
            (unsigned long long)estimated_freed);

    signal_send(victim_pid, SIGKILL);
    oom_kill_count++;

    /* Queue async reaper to reclaim victim's memory immediately */
    oom_victim_add(victim_pid);
    workqueue_schedule(oom_reap_process, (void *)(uintptr_t)victim_pid);

    scheduler_yield();
    return 1;
}

void oom_init(void) {
    oom_kill_count = 0;
    kprintf("[OK] OOM killer initialized — RSS+swap+limit+cache-aware scoring (Item 28)\n");
}

void oom_print_status(void) {
    uint64_t total_frames = pmm_get_total_frames();
    uint64_t used_frames = pmm_get_used_frames();
    uint64_t free_pct = total_frames > 0 ? ((total_frames - used_frames) * 100) / total_frames : 0;

    kprintf("OOM kills: %llu\n", (unsigned long long)oom_kill_count);
    kprintf("Memory: %llu used / %llu total frames (%llu%% free)\n",
            (unsigned long long)used_frames, (unsigned long long)total_frames,
            (unsigned long long)free_pct);

    /* Show top OOM scorers */
    kprintf("Top OOM candidates (pid/name/score/rss/swap/freed_est):\n");
    struct process *table = process_get_table();
    /* Collect top 5 */
    struct { uint32_t pid; int64_t score; uint64_t rss; uint64_t swap; uint64_t freed_est; } top[5];
    int top_count = 0;

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED) continue;
        if (is_oom_safe(&table[i])) continue;
        if (table[i].pid == 0) continue;

        int64_t score = oom_score_process(table[i].pid);
        uint64_t freed = oom_estimate_freed_pages(table[i].pid);

        /* Insert into sorted top list */
        int j;
        for (j = top_count - 1; j >= 0; j--) {
            if (score <= top[j].score) break;
            if (j < 4) {
                top[j + 1] = top[j];
            }
        }
        if (j < 4) {
            top[j + 1].pid       = table[i].pid;
            top[j + 1].score     = score;
            top[j + 1].rss       = 0;
            top[j + 1].swap      = 0;
            top[j + 1].freed_est = freed;
            if (table[i].is_user && table[i].pml4) {
                top[j + 1].rss  = vmm_count_user_pages(table[i].pml4, NULL, NULL);
                top[j + 1].swap = table[i].swap_pages;
            }
            if (top_count < 5) top_count++;
        }
    }

    for (int i = 0; i < top_count; i++) {
        struct process *p = process_get_by_pid(top[i].pid);
        kprintf("  %5u  %-20s  score=%5lld  rss=%5llu  swap=%5llu  free=%5llu\n",
                top[i].pid,
                p && p->name ? p->name : "?",
                (long long)top[i].score,
                (unsigned long long)top[i].rss,
                (unsigned long long)top[i].swap,
                (unsigned long long)top[i].freed_est);
    }
}

/* ── OOM Reaper — Async victim memory reclamation ───────────────────── */

/*
 * Track which PIDs have been marked as OOM victims so the asynchronous
 * reaper work item can safely reclaim their memory.  A PID is inserted
 * when oom_kill sends SIGKILL and removed after reaping completes.
 *
 * Variables (oom_victim_table, oom_victim_lock) and function forward
 * declarations are above select_victim().  Only the implementations
 * live here to avoid "defined but not used" warnings.
 */

/* Add a PID to the victim table (for async reaping) */
static void oom_victim_add(uint32_t pid) {
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&oom_victim_lock, &irq_flags);
    for (int i = 0; i < OOM_VICTIM_MAX; i++) {
        if (!oom_victim_table[i].in_use) {
            oom_victim_table[i].pid = pid;
            oom_victim_table[i].in_use = 1;
            break;
        }
    }
    spinlock_irqsave_release(&oom_victim_lock, irq_flags);
}

/* Remove a PID from the victim table after reaping completes */
static void oom_victim_remove(uint32_t pid) {
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&oom_victim_lock, &irq_flags);
    for (int i = 0; i < OOM_VICTIM_MAX; i++) {
        if (oom_victim_table[i].in_use && oom_victim_table[i].pid == pid) {
            oom_victim_table[i].in_use = 0;
            break;
        }
    }
    spinlock_irqsave_release(&oom_victim_lock, irq_flags);
}

/*
 * oom_reap_process — Asynchronously reclaim a killed process's memory.
 *
 * Called from a workqueue context after OOM sends SIGKILL.  Actively
 * walks the victim's user page tables and frees all physical pages,
 * bypassing the normal process-exit path so that memory becomes available
 * immediately — even if the victim process is stuck (e.g. in D state
 * waiting for I/O) and hasn't had a chance to run yet.
 *
 * Safety:
 *   - The victim's pml4 pointer is atomically set to NULL so no other
 *     kernel code will try to walk it concurrently.
 *   - If the victim is currently running on another CPU, the worst case
 *     is that it continues executing for a few instructions, then faults
 *     when it touches userspace — the fault handler sends SIGKILL.
 *   - After freeing, the dentry cache is also shrunk.
 */
static void oom_reap_process(void *arg)
{
    uint32_t victim_pid = (uint32_t)(uintptr_t)arg;

    struct process *victim = process_get_by_pid(victim_pid);
    if (!victim || victim->state == PROCESS_UNUSED) {
        oom_victim_remove(victim_pid);
        return;
    }

    /* Atomically steal the pml4 pointer so no one else walks it */
    uint64_t *pml4_to_free = NULL;
    if (victim->is_user && victim->pml4) {
        uint64_t irq_flags;
        spinlock_irqsave_acquire(&oom_victim_lock, &irq_flags);
        if (victim->pml4) {
            pml4_to_free = victim->pml4;
            victim->pml4 = NULL;   /* prevent concurrent access */
        }
        spinlock_irqsave_release(&oom_victim_lock, irq_flags);
    }

    uint64_t freed_pages = 0;

    if (pml4_to_free) {
        /* Walk and free all user pages via PMM refcount.
         * vmm_destroy_user_pml4 handles 4KB, 2MB huge pages, and
         * skips the shared zero page. */
        /* Count pages first for the log message */
        uint64_t rss = vmm_count_user_pages(pml4_to_free, NULL, NULL);
        freed_pages = rss;

        /* Free all page-table pages and leaf frames */
        vmm_destroy_user_pml4(pml4_to_free);

        kprintf("[OOM] Reaper: reclaimed %llu pages from pid=%u \"%s\"\n",
                (unsigned long long)freed_pages,
                victim_pid,
                victim->name ? victim->name : "?");
    }

    /* If the victim is already a zombie, clean it up completely.
     * Otherwise mark it as reaped; process_exit_code will finish
     * the job when the victim finally runs. */
    if (victim->state == PROCESS_ZOMBIE && !victim->pml4) {
        /* process_cleanup has already been called for zombies,
         * but if we freed the pml4 above we're done. */
    } else if (victim->state != PROCESS_UNUSED) {
        /* The victim hasn't exited yet.  Setting pml4 = NULL ensures
         * that when it eventually runs, any userspace access will
         * page-fault immediately, which signals SIGKILL again.
         * The process_exit path will skip vmm_destroy_user_pml4
         * because pml4 is already NULL. */
    }

    /* Also shrink dentry cache to reclaim additional memory */
    int evicted = dcache_shrink(DCACHE_SIZE / 2);
    if (evicted > 0) {
        kprintf("[OOM] Reaper: also freed %d dentry cache entries\n", evicted);
    }

    oom_victim_remove(victim_pid);
}

int oom_reaper_running = 0;

static void oom_reaper_task(void *arg) {
    (void)arg;
    kprintf("[OOM] Reaper kthread started\n");
    oom_reaper_running = 1;

    while (oom_reaper_running) {
        /* Check memory pressure every 10 ticks (~100ms at 100Hz) */
        uint64_t total = pmm_get_total_frames();
        uint64_t used = pmm_get_used_frames();
        uint64_t free_pct = total > 0 ? ((total - used) * 100) / total : 0;

        /* If free memory is below 5%, try to kill a victim */
        if (free_pct < 5) {
            kprintf("[OOM] Reaper: low memory (free=%llu%%), shrinking dcache first\n",
                    (unsigned long long)free_pct);
            dcache_shrink(DCACHE_SIZE / 2);
            kprintf("[OOM] Reaper: low memory (free=%llu%%), killing victim\n",
                    (unsigned long long)free_pct);
            oom_kill_victim();
        }

        /* Sleep for 10 ticks */
        process_sleep_ticks(10);
    }

    oom_reaper_running = 0;
}

int oom_reaper_init(void) {
    if (oom_reaper_running) return -1;

    /* Initialise victim table */
    memset(oom_victim_table, 0, sizeof(oom_victim_table));
    spinlock_init(&oom_victim_lock);

    struct process *reaper = kthread_create(oom_reaper_task, NULL, "oom_reaper");
    if (!reaper) {
        kprintf("[OOM] Failed to create reaper kthread\n");
        return -1;
    }
    return 0;
}
