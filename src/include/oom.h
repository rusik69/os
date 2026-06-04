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
 * Selection policy (Item 28 — enhanced RSS+swap+limit-aware scoring):
 *   Base = rss_pages + swap_pages
 *   Score = base * 10                           ← raw footprint
 *         + memory_pressure_bonus                ← (rss / max(rlimit_rss,1)) * 50
 *         + dirty_pages * 3                      ← dirty pages are harder to reclaim
 *         + clean_pages                          ← clean pages reclaimable
 *         + reclaimable_bonus (if >70%)          ← low-hanging fruit: shared+cache pages
 *         + freed_estimate_bonus                 ← estimated pages freed by killing: rss + swap
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
 *
 *   Key enhancements over the basic RSS-only model:
 *     1. swap_pages: counts swapped-out pages per process so swap-heavy
 *        processes are correctly scored.
 *     2. Memory limit ratio: (rss + swap) / RLIMIT_RSS generates higher
 *        scores for processes consuming a large fraction of their limit.
 *     3. Estimated freed pages: scoring includes how many pages the system
 *        would actually recover (RSS + swap + clean cache).
 *     4. Low-hanging cache pages: processes with a high proportion of
 *        clean cache pages are better victims since those pages are
 *        immediately reclaimable without I/O.
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
 * shared/reclaimable pages, swap usage, memory limit ratio,
 * estimated freed pages, process tree, nice/priority, age. */
int64_t oom_score_process(uint32_t pid);

/* Estimate how many physical pages would be freed if this process were killed.
 * Includes RSS + swap_pages + clean page cache pages belonging to the process. */
uint64_t oom_estimate_freed_pages(uint32_t pid);

/* Display OOM status (for /sys or shell) */
void oom_print_status(void);

/* Set/get OOM score adjustment for a process (via /proc/PID/oom_score_adj) */
void oom_set_score_adj(int pid, int16_t adjustment);
int16_t oom_get_score_adj(int pid);

/* OOM reaper kthread control */
int oom_reaper_init(void);
extern int oom_reaper_running;
extern uint64_t oom_kill_count;

#endif /* OOM_H */
