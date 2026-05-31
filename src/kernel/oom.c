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

/* Number of pages (4K) per process we track as "estimated resident" */
static uint64_t estimate_resident_pages(struct process *p) {
    if (!p) return 0;
    /* rough estimate based on kernel stack allocation + known user mappings */
    uint64_t pages = KERNEL_STACK_TOTAL_PAGES;
    if (p->is_user && p->pml4) {
        /* estimate user pages: 64KB stack + code region */
        pages += (USER_STACK_SIZE / PAGE_SIZE);
        pages += 32; /* code, heap, bss */
    }
    return pages;
}

int64_t oom_score_process(uint32_t pid) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    int64_t score = 0;

    /* Resident size estimate */
    uint64_t resident = estimate_resident_pages(p);
    score += (int64_t)resident * 10;

    /* Heap usage (from brk-based allocations) */
    score += (int64_t)(p->utime_ticks + p->stime_ticks) / 100;

    /* Children increase score (avoid killing parents with many kids) */
    int kids = 0;
    struct process *tp = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (tp[i].state != PROCESS_UNUSED && tp[i].parent_pid == p->pid)
            kids++;
    }
    score += kids * 5;

    /* Penalties and bonuses */
    if (p->is_suspended) score += 5;     /* suspended processes are good victims */
    if (p->is_user)       score -= 10;    /* user processes are more valuable */
    if (p->priority > 0)  score += p->priority * 3;  /* low priority = better victim */

    /* Long-running processes get a slight bonus (may be important) */
    uint64_t age_ticks = timer_get_ticks() - p->start_time_tick;
    if (age_ticks > TIMER_FREQ * 60)  /* older than 60 seconds */
        score -= (int64_t)(age_ticks / (TIMER_FREQ * 10)); /* -1 per 10s */

    /* Apply OOM score adjustment */
    score += oom_get_score_adj((int)p->pid);

    return score;
}

static int is_oom_safe(struct process *p) {
    if (!p || p->state == PROCESS_UNUSED) return 1;
    /* Never kill PID 1 (init), idle, or kernel-that-just-started */
    if (p->pid == 1 || p->pid == 0) return 1;
    /* Don't kill the current process if it's the last resort */
    struct process *cur = process_get_current();
    if (cur && p->pid == cur->pid) return 1;
    return 0;
}

int oom_kill(uint64_t needed_pages) {
    (void)needed_pages;

    struct process *table = process_get_table();
    uint32_t victim_pid = 0;
    int64_t best_score = -1;

    /* Score all processes and pick the worst offender */
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED) continue;
        if (is_oom_safe(&table[i])) continue;
        int64_t score = oom_score_process(table[i].pid);
        if (score > best_score) {
            best_score = score;
            victim_pid = table[i].pid;
        }
    }

    if (victim_pid == 0) {
        kprintf("[OOM] No suitable victim found!\n");
        return 0;
    }

    struct process *victim = process_get_by_pid(victim_pid);
    kprintf("[OOM] Killing pid=%u \"%s\" (score=%lld, %s, prio=%u)\n",
            victim_pid, victim ? victim->name : "?",
            (long long)best_score,
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

    if (victim_pid == 0) {
        return 0;
    }

    struct process *victim = process_get_by_pid(victim_pid);
    kprintf("[OOM] Killing pid=%u \"%s\" (score=%lld)\n",
            victim_pid, victim ? victim->name : "?",
            (long long)best_score);

    signal_send(victim_pid, SIGKILL);
    oom_kill_count++;
    scheduler_yield();
    return 1;
}

void oom_init(void) {
    oom_kill_count = 0;
    kprintf("[OK] OOM killer initialized\n");
}

void oom_print_status(void) {
    kprintf("OOM kills: %llu\n", (unsigned long long)oom_kill_count);
}
