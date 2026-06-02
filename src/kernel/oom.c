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

/* OOM score adjustment table (by PID) */
#define OOM_ADJ_MIN  (-16)
#define OOM_ADJ_MAX  15

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
 * ── OOM scoring ──────────────────────────────────────────────────────
 *
 * Score a process based on how much memory it uses and how valuable it is.
 * Higher score = more likely to be killed.
 *
 * Scoring factors (higher = more likely to die):
 *   1. Actual resident pages (RSS) from page table walk       ─  weight ×10
 *   2. Dirty-to-total page ratio                               ─  dirty pages are harder to reclaim
 *   3. Low-hanging cache pages (shared/COW/lazy) are bonus    ─  killing frees these easily
 *   4. Children count                                          ─  +5 each (takes out descendants)
 *   5. Suspended/background processes                          ─  +5 bonus
 *   6. Low priority / high nice value                          ─  +priority*3
 *   7. Long-running processes get age discount                 ─  -1 per 10s after 60s
 *   8. Kernel threads (no user pml4) score lower               ─  they don't free user pages
 *   9. OOM score adjustment from /proc interface               ─  user-controlled
 */
int64_t oom_score_process(uint32_t pid) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    int64_t score = 0;

    /* ── Resident memory (actual page table walk) ───────────────── */
    uint64_t rss_pages = 0;
    uint64_t dirty_pages = 0;
    uint64_t shared_pages = 0;

    if (p->is_user && p->pml4) {
        rss_pages = vmm_count_user_pages(p->pml4, &dirty_pages, &shared_pages);
    }

    /* RSS weight: each resident page contributes 10 points.
     * Dirty pages contribute extra (they have to be written to swap).
     * Shared/COW pages contribute less (they may be shared). */
    score += (int64_t)rss_pages * 10;
    score += (int64_t)dirty_pages * 3;           /* dirty pages are harder to reclaim */
    score += (int64_t)(rss_pages - dirty_pages);  /* clean pages are somewhat reclaimable */

    /* Low-hanging memory: shared pages are easily reclaimed (COW/lazy/zero).
     * Processes with many shared pages are better victims. */
    if (rss_pages > 0) {
        uint64_t reclaimable = shared_pages + (rss_pages - dirty_pages);
        /* Processes with >70% reclaimable pages get a bonus */
        if ((reclaimable * 100) / rss_pages > 70)
            score += reclaimable * 2;
    }

    /* Kernel threads: they have no user memory to free,
     * so score them lower unless they're huge consumers. */
    if (!p->is_user || !p->pml4) {
        /* Still some pages may be accounted through kernel allocations,
         * but far less valuable as victims. Cap their score. */
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

static int is_oom_safe(struct process *p) {
    if (!p || p->state == PROCESS_UNUSED) return 1;
    /* Never kill PID 1 (init), idle (0), or the current process */
    if (p->pid == 1 || p->pid == 0) return 1;
    struct process *cur = process_get_current();
    if (cur && p->pid == cur->pid) return 1;
    return 0;
}

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

    uint32_t victim_pid = select_victim();
    if (victim_pid == 0) {
        kprintf("[OOM] No suitable victim found!\n");
        return 0;
    }

    struct process *victim = process_get_by_pid(victim_pid);
    uint64_t rss = 0;
    if (victim && victim->is_user && victim->pml4)
        rss = vmm_count_user_pages(victim->pml4, NULL, NULL);

    kprintf("[OOM] Killing pid=%u \"%s\" (score=%lld, rss=%llu pages, %s, prio=%u)\n",
            victim_pid, victim ? victim->name : "?",
            (long long)(victim ? oom_score_process(victim_pid) : -1),
            (unsigned long long)rss,
            (victim && victim->is_user) ? "user" : "kernel",
            victim ? (uint32_t)victim->priority : 0);

    /* Send SIGKILL — signal handler in the target process will cause exit */
    signal_send(victim_pid, SIGKILL);

    oom_kill_count++;

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
    if (victim && victim->is_user && victim->pml4)
        rss = vmm_count_user_pages(victim->pml4, NULL, NULL);

    kprintf("[OOM] Killing pid=%u \"%s\" (score=%lld, rss=%llu pages)\n",
            victim_pid, victim ? victim->name : "?",
            (long long)(victim ? oom_score_process(victim_pid) : -1),
            (unsigned long long)rss);

    signal_send(victim_pid, SIGKILL);
    oom_kill_count++;
    scheduler_yield();
    return 1;
}

void oom_init(void) {
    oom_kill_count = 0;
    kprintf("[OK] OOM killer initialized with enhanced RSS-aware scoring\n");
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
    kprintf("Top OOM candidates (pid/name/score/rss):\n");
    struct process *table = process_get_table();
    /* Collect top 5 */
    struct { uint32_t pid; int64_t score; uint64_t rss; } top[5];
    int top_count = 0;

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED) continue;
        if (is_oom_safe(&table[i])) continue;
        if (table[i].pid == 0) continue;

        int64_t score = oom_score_process(table[i].pid);
        uint64_t rss = 0;
        if (table[i].is_user && table[i].pml4)
            rss = vmm_count_user_pages(table[i].pml4, NULL, NULL);

        /* Insert into sorted top list */
        int j;
        for (j = top_count - 1; j >= 0; j--) {
            if (score <= top[j].score) break;
            if (j < 4) {
                top[j + 1] = top[j];
            }
        }
        if (j < 4) {
            top[j + 1].pid   = table[i].pid;
            top[j + 1].score = score;
            top[j + 1].rss   = rss;
            if (top_count < 5) top_count++;
        }
    }

    for (int i = 0; i < top_count; i++) {
        struct process *p = process_get_by_pid(top[i].pid);
        kprintf("  %5u  %-20s  score=%5lld  rss=%5llu pages\n",
                top[i].pid,
                p && p->name ? p->name : "?",
                (long long)top[i].score,
                (unsigned long long)top[i].rss);
    }
}

/* ── OOM Reaper kthread ─────────────────────────────────────────────── */

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

    struct process *reaper = kthread_create(oom_reaper_task, NULL, "oom_reaper");
    if (!reaper) {
        kprintf("[OOM] Failed to create reaper kthread\n");
        return -1;
    }
    return 0;
}
