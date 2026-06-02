#ifndef OOM_H
#define OOM_H

#include "types.h"

/* OOM (Out-Of-Memory) killer — frees memory by killing the worst-behaving process
 * when the page allocator is exhausted.
 *
 * Interface:
 *   oom_kill() — called by pmm_alloc_frame when no frames are available.
 *                 Selects a victim, kills it, and reclaims its frames.
 *   oom_kill_victim() — kills the highest-scored process.
 *   oom_score_process(pid) — returns the oom_score for a given pid (or -1 on error).
 *
 * Selection policy (Item 28 — enhanced RSS-aware scoring):
 *   Score = rss_pages * 10
 *         + dirty_pages * 3
 *         + clean_pages * 1
 *         + reclaimable_bonus (if >70% reclaimable)
 *         + kids * 5
 *         + suspended/background bonuses
 *         - user_process_penalty
 *         + priority * 3
 *         + nice_adjustment
 *         - age_discount (for long-running processes)
 *         + oom_score_adj
 *
 *   The highest-scoring process that is not PID 1, the idle process, or
 *   the current process is killed.
 */

/* Initialize the OOM subsystem */
void oom_init(void);

/* Attempt to reclaim at least 'needed_pages' by killing a process.
 * Returns 1 if a process was killed, 0 if no acceptable victim found. */
int oom_kill(uint64_t needed_pages);

/* Kill the highest-scored process. Returns 1 if a process was killed, 0 otherwise.
 * Skips the current process and init (PID 0/1). */
int oom_kill_victim(void);

/* Compute the OOM score for a given process (higher = more likely to be killed).
 * Factors: actual RSS (via page table walk), dirty/clean page ratio,
 * shared/reclaimable pages, process tree, nice/priority, age. */
int64_t oom_score_process(uint32_t pid);

/* Display OOM status (for /sys or shell) */
void oom_print_status(void);

/* Total number of processes killed by OOM since boot */
extern uint64_t oom_kill_count;

/* OOM score adjustment API — allow adjusting how likely a process is to be killed.
 * Positive adjustment + n  →  more likely to be killed.
 * Negative adjustment − n  →  less likely to be killed.
 * Range: -16 .. +15 (clamped) */
void oom_set_score_adj(int pid, int16_t adjustment);
int16_t oom_get_score_adj(int pid);

/* OOM Reaper kthread — periodically kills the highest-scored process when memory is low */
int oom_reaper_init(void);
extern int oom_reaper_running;

#endif
