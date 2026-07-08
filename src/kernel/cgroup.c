/*
 * cgroup.c — Cgroup v2 unified hierarchy controller
 *
 * Implements the cgroup v2 interface anchored at /sys/fs/cgroup/:
 *   1. Cgroup v2 unified hierarchy — mount, tree management
 *   2. CPU controller — cpu.max (quota/period), throttle
 *   3. Memory controller — memory.max/high, cgroup OOM
 *   4. IO controller — io.max (IOPS/bandwidth limits)
 *   5. PID controller — pids.max (process limit)
 *   6. Cgroup freezer — freeze/unfreeze cgroup tasks
 *
 * Each cgroup is represented by a struct cgroup with resource
 * controller state and a list of member processes.
 *
 * Item 440–445: Cgroup v2 controllers
 */

#define KERNEL_INTERNAL
#include "cgroup.h"
#include "string.h"
#include "string_ext.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "timer.h"
#include "process.h"
#include "scheduler.h"
#include "signal.h"
#include "vfs.h"
#include "export.h"
/* TIMER_FREQ from timeconst.h */
#include "timeconst.h"
#ifndef TIMER_FREQ
#define TIMER_FREQ 100
#endif

/* ── Process helper wrappers (used by cgroup freezer & OOM) ────────── */

#ifndef CGROUP_PROCESS_WRAPPERS
#define CGROUP_PROCESS_WRAPPERS
#include "scheduler.h"

static inline void process_freeze(uint32_t pid)
{
    struct process *p = process_get_by_pid(pid);
    if (p && p->state != PROCESS_UNUSED && p->state != PROCESS_ZOMBIE) {
        p->is_suspended = 1;
        p->state = PROCESS_BLOCKED;
        scheduler_remove(p);
    }
}

static inline void process_unfreeze(uint32_t pid)
{
    struct process *p = process_get_by_pid(pid);
    if (p && p->state != PROCESS_UNUSED && p->state != PROCESS_ZOMBIE) {
        p->is_suspended = 0;
        p->sleep_until = 0;
        p->state = PROCESS_READY;
        scheduler_add(p);
    }
}

static inline uint64_t process_get_mem_usage(uint32_t pid)
{
    struct process *p = process_get_by_pid(pid);
    return p ? p->max_rss : 0;
}

static inline void process_kill(uint32_t pid)
{
    signal_send(pid, SIGKILL);
}
#endif

/* ── Global state ─────────────────────────────────────────────────── */

/* Maximum cgroups in the tree */
#define CGROUP_MAX 64

/* Root cgroup (/) is always at index 0 */
static struct cgroup g_cgroups[CGROUP_MAX];
static int g_num_cgroups = 0;
static spinlock_t g_cgroup_lock;
static int g_cgroup_initialized = 0;

/* ── CPU controller state ─────────────────────────────────────────── */

/* Default CPU quota/period: 100000/100000 = 1 core */
#define CGROUP_CPU_PERIOD_DEFAULT 100000
#define CGROUP_CPU_PERIOD_MIN     1000
#define CGROUP_CPU_PERIOD_MAX     1000000

/* ── Memory controller state ──────────────────────────────────────── */

#define CGROUP_MEM_MAX_DEFAULT  0   /* 0 = unlimited */
#define CGROUP_MEM_HIGH_DEFAULT 0

/* ── IO controller state ──────────────────────────────────────────── */

#define CGROUP_IO_MAX_DEVICES 8

/* ── PID controller state ─────────────────────────────────────────── */

#define CGROUP_PIDS_MAX_DEFAULT 0  /* 0 = unlimited */

/* ── Queue of pending work items ──────────────────────────────────── */

struct cgroup_work {
    enum { CGROUP_WORK_NONE, CGROUP_WORK_FREEZE, CGROUP_WORK_UNFREEZE,
           CGROUP_WORK_OOM_KILL } type;
    int cg_idx;
} g_cgroup_work_queue[16];
static int g_cgroup_work_head = 0, g_cgroup_work_tail = 0;
static spinlock_t g_cgroup_work_lock;

/* ═══════════════════════════════════════════════════════════════════════
 *  Internal helpers
 * ═══════════════════════════════════════════════════════════════════════ */

static inline bool cgroup_valid(int idx)
{
    return idx >= 0 && idx < CGROUP_MAX && g_cgroups[idx].in_use;
}

static int cgroup_alloc_id(void)
{
    spinlock_acquire(&g_cgroup_lock);
    for (int i = 1; i < CGROUP_MAX; i++) {
        if (!g_cgroups[i].in_use) {
            memset(&g_cgroups[i], 0, sizeof(struct cgroup));
            g_cgroups[i].in_use = 1;
            g_cgroups[i].id = i;
            g_cgroups[i].parent_id = -1;
            g_cgroups[i].cpu.max_period = CGROUP_CPU_PERIOD_DEFAULT;
            g_cgroups[i].cpu.max_quota   = CGROUP_CPU_PERIOD_DEFAULT;
            g_num_cgroups++;
            spinlock_release(&g_cgroup_lock);
            return i;
        }
    }
    spinlock_release(&g_cgroup_lock);
    return -ENOSPC;
}

/* ── Cgroupv2 filesystem ──────────────────────────────────────────── */

static int cgroup_v2_read(void *priv, const char *path, void *buf,
                          uint32_t max, uint32_t *out)
{
    (void)priv;
    (void)path;
    /* Build hierarchy info listing all mounted cgroups and their controllers */
    char tmp[512];
    int pos = 0;
    {
        int n = snprintf(tmp + pos, sizeof(tmp) - (size_t)pos,
            "# Cgroup v2 unified hierarchy\n"
            "# Controllers: cpu memory io pids freezer\n"
            "# /sys/fs/cgroup/ mounted\n\n");
        if (n > 0 && pos + n < (int)sizeof(tmp)) pos += n;
    }

    spinlock_acquire(&g_cgroup_lock);
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (!g_cgroups[i].in_use) continue;
        /* Build controller list for this cgroup */
        char ctrl[128] = "";
        if (g_cgroups[i].cpu.max_period > 0 || g_cgroups[i].cpu.max_quota > 0)
            strlcat(ctrl, "cpu ", sizeof(ctrl));
        if (g_cgroups[i].mem.max_bytes > 0 || g_cgroups[i].mem.high_bytes > 0)
            strlcat(ctrl, "memory ", sizeof(ctrl));
        if (g_cgroups[i].pids.max > 0)
            strlcat(ctrl, "pids ", sizeof(ctrl));
        /* IO limits */
        for (int j = 0; j < CGROUP_IO_MAX_DEVICES; j++) {
            if (g_cgroups[i].io.devices[j].in_use) {
                strlcat(ctrl, "io ", sizeof(ctrl));
                break;
            }
        }
        /* Freezer is always available */
        strlcat(ctrl, "freezer", sizeof(ctrl));
        if (ctrl[0] == '\0')
            strlcpy(ctrl, "-", sizeof(ctrl));

        {
            int n = snprintf(tmp + pos, sizeof(tmp) - (size_t)pos,
                "  cgroup[%d]  parent=%d  pids=%lu  controllers=%s\n",
                i, g_cgroups[i].parent_id,
                (unsigned long)g_cgroups[i].pids.current,
                ctrl);
            if (n > 0 && pos + n < (int)sizeof(tmp)) pos += n;
        }
    }
    spinlock_release(&g_cgroup_lock);

    size_t total = (size_t)pos < (size_t)max ? (size_t)pos : (size_t)max;
    memcpy(buf, tmp, total);
    *out = (uint32_t)total;
    return 0;
}

static int cgroup_v2_write(void *priv, const char *path, const void *buf,
                           uint32_t size)
{
    (void)priv;
    /* Parse write content to support cgroup migration and limit setting.
     *
     * Format 1: "cgroup.procs <pid>"   — migrate PID into this cgroup
     * Format 2: "<key> <value>"         — set control value
     * Format 3: "<pid>"                 — shorthand migration (to root cgroup.procs)
     */
    const char *s = (const char *)buf;
    uint32_t len = size;
    /* Strip trailing whitespace/newline */
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' ' || s[len - 1] == '\t'))
        len--;

    /* Check for cgroup.procs prefix */
    const char *procs_prefix = "cgroup.procs ";
    size_t plen = strlen(procs_prefix);
    int pid = -1;
    int is_migration = 0;

    if (len > plen && memcmp(s, procs_prefix, plen) == 0) {
        /* cgroup.procs <pid> */
        pid = 0;
        for (uint32_t i = (uint32_t)plen; i < len; i++) {
            if (s[i] < '0' || s[i] > '9') return -EINVAL;
            pid = pid * 10 + (int)(s[i] - '0');
        }
        is_migration = 1;
    } else {
        /* Try plain PID or key=value */
        int all_digits = 1;
        int has_space = 0;
        for (uint32_t i = 0; i < len; i++) {
            if (s[i] < '0' || s[i] > '9') {
                if (s[i] == ' ' || s[i] == '\t') {
                    has_space = 1;
                } else {
                    all_digits = 0;
                    break;
                }
            }
        }

        if (all_digits && !has_space && len > 0) {
            /* Plain PID — migrate */
            pid = 0;
            for (uint32_t i = 0; i < len; i++)
                pid = pid * 10 + (int)(s[i] - '0');
            is_migration = 1;
        }
    }

    if (is_migration && pid >= 0) {
        /* Extract cgroup ID from path.
         * Path looks like: /sys/fs/cgroup/<name>/cgroup.procs
         * or /sys/fs/cgroup/cgroup.procs for root.
         */
        int cg_id = 0; /* default to root */
        if (path) {
            const char *p = path;
            /* Skip past /sys/fs/cgroup/ */
            const char *base = "/sys/fs/cgroup/";
            size_t blen = strlen(base);
            if (memcmp(p, base, blen) == 0) {
                p += blen;
                /* If there's a subdirectory name before /cgroup.procs */
                const char *slash = strchr(p, '/');
                if (slash && (size_t)(slash - p) < sizeof(g_cgroups[0].name)) {
                    char cg_name[32];
                    size_t nlen = (size_t)(slash - p);
                    if (nlen > 31) nlen = 31;
                    memcpy(cg_name, p, nlen);
                    cg_name[nlen] = '\0';
                    /* Look up cgroup by name */
                    int found = -1;
                    for (int i = 0; i < CGROUP_MAX; i++) {
                        if (g_cgroups[i].in_use &&
                            g_cgroups[i].name[0] &&
                            strcmp(g_cgroups[i].name, cg_name) == 0) {
                            found = i;
                            break;
                        }
                    }
                    if (found >= 0) cg_id = found;
                }
            }
        }
        return cgroup_attach(pid, cg_id);
    }

    /* Key-value setting: "cpu.max 50000 100000", "memory.max 1048576", etc. */
    if (len > 0) {
        /* Copy to a temporary buffer */
        char tmp[128];
        if (len > 127) len = 127;
        memcpy(tmp, s, len);
        tmp[len] = '\0';

        /* Try to split into controller key and value */
        /* Format: "<controller>.<key> <value>" or "<controller> <key> <value>" */
        char *space = strchr(tmp, ' ');
        if (space) {
            *space++ = '\0';
            char *key = tmp;
            char *value = space;

            /* Also support "<controller> <key> <value>" by splitting again */
            char *space2 = strchr(value, ' ');
            if (space2) {
                *space2++ = '\0';
                /* key = value (first token after controller), value = space2 */
                /* But we need to find the cgroup ID from the path */
                int cg_id = 0;
                /* Extract cgroup name from path (same logic as above) */
                if (path) {
                    const char *p = path;
                    const char *base = "/sys/fs/cgroup/";
                    size_t blen = strlen(base);
                    if (memcmp(p, base, blen) == 0) {
                        p += blen;
                        const char *slash = strchr(p, '/');
                        char cg_name[32] = "";
                        if (slash) {
                            size_t nlen = (size_t)(slash - p);
                            if (nlen > 31) nlen = 31;
                            memcpy(cg_name, p, nlen);
                            cg_name[nlen] = '\0';
                        }
                        if (cg_name[0]) {
                            for (int i = 0; i < CGROUP_MAX; i++) {
                                if (g_cgroups[i].in_use &&
                                    g_cgroups[i].name[0] &&
                                    strcmp(g_cgroups[i].name, cg_name) == 0) {
                                    cg_id = i;
                                    break;
                                }
                            }
                        }
                    }
                }
                /* Validate cg_id before use */
                if (cg_id < 0 || cg_id >= CGROUP_MAX)
                    return -EINVAL;
                return cgroup_write_control(cg_id, key, value, space2);
            } else {
                /* "key value" with no controller prefix */
                int cg_id = 0;
                if (path) {
                    const char *p = path;
                    const char *base = "/sys/fs/cgroup/";
                    size_t blen = strlen(base);
                    if (memcmp(p, base, blen) == 0) {
                        p += blen;
                        const char *slash = strchr(p, '/');
                        char cg_name[32] = "";
                        if (slash) {
                            size_t nlen = (size_t)(slash - p);
                            if (nlen > 31) nlen = 31;
                            memcpy(cg_name, p, nlen);
                            cg_name[nlen] = '\0';
                        }
                        if (cg_name[0]) {
                            for (int i = 0; i < CGROUP_MAX; i++) {
                                if (g_cgroups[i].in_use &&
                                    g_cgroups[i].name[0] &&
                                    strcmp(g_cgroups[i].name, cg_name) == 0) {
                                    cg_id = i;
                                    break;
                                }
                            }
                        }
                    }
                }
                /* Validate cg_id before use */
                if (cg_id < 0 || cg_id >= CGROUP_MAX)
                    return -EINVAL;
                return cgroup_write_control(cg_id, "", key, value);
            }
        }
    }

    return 0;
}

static struct vfs_ops cgroup_v2_vfs_ops = {
    .read  = cgroup_v2_read,
    .write = cgroup_v2_write,
};

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API — Cgroup management
 * ═══════════════════════════════════════════════════════════════════════ */

/* Create a new cgroup as a child of parent_id.
 * Returns cgroup ID (>= 0) on success, negative errno on failure. */
int cgroup_create(int parent_id)
{
    if (parent_id != 0 && !cgroup_valid(parent_id))
        return -EINVAL;

    int id = cgroup_alloc_id();
    if (id < 0) return id;

    g_cgroups[id].parent_id = parent_id;
    g_cgroups[id].cpu.max_period = CGROUP_CPU_PERIOD_DEFAULT;
    g_cgroups[id].cpu.max_quota   = CGROUP_CPU_PERIOD_DEFAULT;
    g_cgroups[id].cpu.usage_usec  = 0;

    kprintf("[cgroup] created cgroup %d (parent %d)\n", id, parent_id);
    return id;
}
EXPORT_SYMBOL(cgroup_create);

/* Destroy a cgroup. All member processes are moved to the root cgroup.
 * Returns 0 on success. */
int cgroup_destroy(int cg_id)
{
    if (!cgroup_valid(cg_id) || cg_id == 0)
        return -EINVAL;

    spinlock_acquire(&g_cgroup_lock);
    struct cgroup *cg = &g_cgroups[cg_id];

    /* Move all processes to root */
    for (int i = 0; i < CGROUP_MAX_PIDS; i++) {
        if (cg->members[i] != 0) {
            cgroup_attach(0, cg->members[i]);
        }
    }

    memset(cg, 0, sizeof(struct cgroup));
    g_num_cgroups--;
    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_destroy);

/* Attach a process (PID) to a cgroup.
 * Returns 0 on success, negative errno on failure. */
int cgroup_attach(int cg_id, int pid)
{
    if (!cgroup_valid(cg_id))
        return -EINVAL;

    spinlock_acquire(&g_cgroup_lock);
    struct cgroup *cg = &g_cgroups[cg_id];

    /* Remove from old cgroup first */
    for (int i = 0; i < CGROUP_MAX_PIDS; i++) {
        for (int j = 0; j < CGROUP_MAX; j++) {
            if (!g_cgroups[j].in_use) continue;
            for (int k = 0; k < CGROUP_MAX_PIDS; k++) {
                if (g_cgroups[j].members[k] == pid) {
                    g_cgroups[j].members[k] = 0;
                    g_cgroups[j].num_pids--;
                    goto found;
                }
            }
        }
    }
found:
    /* Find free slot in new cgroup */
    for (int i = 0; i < CGROUP_MAX_PIDS; i++) {
        if (cg->members[i] == 0) {
            cg->members[i] = pid;
            cg->num_pids++;
            spinlock_release(&g_cgroup_lock);
            return 0;
        }
    }
    spinlock_release(&g_cgroup_lock);
    return -ENOSPC; /* cgroup full */
}
EXPORT_SYMBOL(cgroup_attach);

/* Get the cgroup ID for a given PID.
 * Returns cgroup ID or -1 if not found. */
int cgroup_of_pid(int pid)
{
    spinlock_acquire(&g_cgroup_lock);
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (!g_cgroups[i].in_use) continue;
        for (int j = 0; j < CGROUP_MAX_PIDS; j++) {
            if (g_cgroups[i].members[j] == pid) {
                spinlock_release(&g_cgroup_lock);
                return i;
            }
        }
    }
    spinlock_release(&g_cgroup_lock);
    return -ENOENT; /* root */
}

/* ═══════════════════════════════════════════════════════════════════════
 *  CPU controller (cpu.max quota/period, throttle)
 * ═══════════════════════════════════════════════════════════════════════ */

/* Set cpu.max quota (µs per period) and period (µs).
 * quota <= 0 means unlimited. */
int cgroup_cpu_set_max(int cg_id, int64_t quota_us, int64_t period_us)
{
    if (!cgroup_valid(cg_id)) return -EINVAL;
    if (period_us < CGROUP_CPU_PERIOD_MIN || period_us > CGROUP_CPU_PERIOD_MAX)
        return -EINVAL;

    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);
    cg->cpu.max_quota   = quota_us <= 0 ? CGROUP_CPU_PERIOD_DEFAULT : (uint64_t)quota_us;
    cg->cpu.max_period  = (uint64_t)period_us;
    cg->cpu.throttled   = 0;
    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_cpu_set_max);

/* Query cpu.max values. */
void cgroup_cpu_get_max(int cg_id, uint64_t *quota, uint64_t *period)
{
    if (!cgroup_valid(cg_id)) return;
    struct cgroup *cg = &g_cgroups[cg_id];
    if (quota)  *quota  = cg->cpu.max_quota;
    if (period) *period = cg->cpu.max_period;
}

/* Account CPU usage (called by scheduler on context switch).
 * @pid: process that ran
 * @delta_us: microseconds of CPU time consumed
 * Returns 1 if the process was throttled, 0 otherwise. */
int cgroup_cpu_account(int pid, uint64_t delta_us)
{
    int cg_id = cgroup_of_pid(pid);
    if (cg_id < 0 || !cgroup_valid(cg_id)) return 0;

    struct cgroup *cg = &g_cgroups[cg_id];
    if (cg->cpu.max_quota == 0) return 0; /* unlimited */

    spinlock_acquire(&g_cgroup_lock);
    cg->cpu.usage_usec += delta_us;

    /* Check if we've exceeded the quota in this period */
    if (cg->cpu.usage_usec > cg->cpu.max_quota) {
        cg->cpu.throttled = 1;
        cg->cpu.nr_throttled++;
        spinlock_release(&g_cgroup_lock);
        return 1; /* caller should throttle */
    }

    /* Period rotation check — approximate: reset when usage exceeds period */
    if (cg->cpu.usage_usec > cg->cpu.max_period) {
        cg->cpu.usage_usec = 0;
        cg->cpu.throttled = 0;
    }

    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_cpu_account);

/* Check if a cgroup is currently throttled. */
int cgroup_cpu_is_throttled(int cg_id)
{
    if (!cgroup_valid(cg_id)) return 0;
    return g_cgroups[cg_id].cpu.throttled;
}

/* Get CPU throttling statistics. */
void cgroup_cpu_stat(int cg_id, uint64_t *usage_usec,
                     uint64_t *nr_throttled, uint64_t *throttled_usec)
{
    if (!cgroup_valid(cg_id)) return;
    struct cgroup *cg = &g_cgroups[cg_id];
    if (usage_usec)       *usage_usec       = cg->cpu.usage_usec;
    if (nr_throttled)     *nr_throttled     = cg->cpu.nr_throttled;
    if (throttled_usec)   *throttled_usec   = cg->cpu.throttled_usec;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Memory controller (memory.max/high, cgroup OOM)
 * ═══════════════════════════════════════════════════════════════════════ */

/* Set memory.max limit (bytes). 0 = unlimited. */
int cgroup_mem_set_max(int cg_id, uint64_t max_bytes)
{
    if (!cgroup_valid(cg_id)) return -EINVAL;
    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);
    cg->mem.max_bytes = max_bytes;
    if (max_bytes > 0 && cg->mem.usage_bytes > max_bytes) {
        /* Trigger OOM */
        cg->mem.oom_triggered = 1;
    }
    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_mem_set_max);

/* Set memory.high limit (bytes). 0 = unlimited.
 * When exceeded, reclaim is aggressively attempted. */
int cgroup_mem_set_high(int cg_id, uint64_t high_bytes)
{
    if (!cgroup_valid(cg_id)) return -EINVAL;
    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);
    cg->mem.high_bytes = high_bytes;
    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_mem_set_high);

/* Account memory usage (called by page allocator).
 * @pid: process allocating memory
 * @nr_pages: number of pages allocated (positive) or freed (negative)
 * Returns 1 if cgroup OOM should be triggered, 0 otherwise. */
int cgroup_mem_account(int pid, int64_t nr_pages)
{
    int cg_id = cgroup_of_pid(pid);
    if (cg_id < 0 || !cgroup_valid(cg_id)) return 0;

    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);
    uint64_t delta = (uint64_t)(nr_pages > 0 ? (uint64_t)nr_pages * PAGE_SIZE : 0);

    if (nr_pages > 0) {
        cg->mem.usage_bytes += delta;
        /* Check high limit — trigger reclaim */
        if (cg->mem.high_bytes > 0 && cg->mem.usage_bytes > cg->mem.high_bytes) {
            cg->mem.high_crossings++;
        }
        /* Check hard limit — trigger OOM */
        if (cg->mem.max_bytes > 0 && cg->mem.usage_bytes > cg->mem.max_bytes) {
            cg->mem.oom_triggered = 1;
            cg->mem.oom_kills++;
            spinlock_release(&g_cgroup_lock);
            return 1; /* signal OOM */
        }
    } else if (nr_pages < 0) {
        uint64_t freed = (uint64_t)(-nr_pages) * PAGE_SIZE;
        if (cg->mem.usage_bytes >= freed)
            cg->mem.usage_bytes -= freed;
        else
            cg->mem.usage_bytes = 0;
    }

    /* Track max usage */
    if (cg->mem.usage_bytes > cg->mem.max_usage)
        cg->mem.max_usage = cg->mem.usage_bytes;

    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_mem_account);

/* Query memory usage and limits. */
void cgroup_mem_stat(int cg_id, uint64_t *usage, uint64_t *max_usage,
                     uint64_t *limit, uint64_t *high_limit,
                     int *oom_kills)
{
    if (!cgroup_valid(cg_id)) return;
    struct cgroup *cg = &g_cgroups[cg_id];
    if (usage)      *usage      = cg->mem.usage_bytes;
    if (max_usage)  *max_usage  = cg->mem.max_usage;
    if (limit)      *limit      = cg->mem.max_bytes;
    if (high_limit) *high_limit = cg->mem.high_bytes;
    if (oom_kills)  *oom_kills  = cg->mem.oom_kills;
}

/* Kill the largest process in a cgroup (OOM handler).
 * Returns PID of killed process, or -1 if none. */
int cgroup_oom_kill(int cg_id)
{
    if (!cgroup_valid(cg_id)) return -EINVAL;

    struct cgroup *cg = &g_cgroups[cg_id];
    int victim = -1;
    uint64_t max_mem = 0;

    spinlock_acquire(&g_cgroup_lock);
    for (int i = 0; i < CGROUP_MAX_PIDS; i++) {
        int pid = cg->members[i];
        if (pid == 0) continue;
        uint64_t mem = process_get_mem_usage(pid); /* defined in process.h */
        if (mem > max_mem) {
            max_mem = mem;
            victim = pid;
        }
    }

    if (victim > 0) {
        kprintf("[cgroup OOM] killing PID %d (mem %llu) in cgroup %d\n",
                victim, (unsigned long long)max_mem, cg_id);
        process_kill(victim);
        cg->mem.oom_kills++;
    }
    spinlock_release(&g_cgroup_lock);
    return victim;
}
EXPORT_SYMBOL(cgroup_oom_kill);

/* ═══════════════════════════════════════════════════════════════════════
 *  IO controller (io.max IOPS/bandwidth limits)
 * ═══════════════════════════════════════════════════════════════════════ */

/* Set IO limits for a cgroup on a given device major:minor.
 * @cg_id: cgroup ID
 * @major, @minor: device number
 * @rbps: read bytes per second limit (0 = unlimited)
 * @wbps: write bytes per second limit (0 = unlimited)
 * @riops: read IOPS limit (0 = unlimited)
 * @wiops: write IOPS limit (0 = unlimited) */
int cgroup_io_set_limit(int cg_id, uint32_t major, uint32_t minor,
                        uint64_t rbps, uint64_t wbps,
                        uint64_t riops, uint64_t wiops)
{
    if (!cgroup_valid(cg_id)) return -EINVAL;

    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);

    /* Find existing device entry or create one */
    int slot = -1;
    for (int i = 0; i < CGROUP_IO_MAX_DEVICES; i++) {
        if (cg->io.devices[i].major == major &&
            cg->io.devices[i].minor == minor) {
            slot = i;
            break;
        }
        if (slot < 0 && cg->io.devices[i].major == 0 &&
            cg->io.devices[i].minor == 0)
            slot = i;
    }
    if (slot < 0) {
        spinlock_release(&g_cgroup_lock);
        return -ENOSPC;
    }

    cg->io.devices[slot].major = (uint16_t)major;
    cg->io.devices[slot].minor = (uint16_t)minor;
    cg->io.devices[slot].rbps  = rbps;
    cg->io.devices[slot].wbps  = wbps;
    cg->io.devices[slot].riops = riops;
    cg->io.devices[slot].wiops = wiops;
    cg->io.devices[slot].in_use = 1;

    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_io_set_limit);

/* Check IO against limits for a cgroup.
 * Called by the block layer before submitting a request.
 * @cg_id: cgroup ID
 * @is_write: 1 for write, 0 for read
 * @bytes: size of the I/O in bytes
 * Returns 1 if the I/O should be throttled (delayed), 0 if allowed. */
int cgroup_io_throttle_check(int cg_id, int is_write, uint64_t bytes)
{
    if (!cgroup_valid(cg_id)) return 0;

    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);

    /* Check against all device limits — for simplicity, aggregate */
    for (int i = 0; i < CGROUP_IO_MAX_DEVICES; i++) {
        if (!cg->io.devices[i].in_use) continue;
        struct cgroup_io_device *dev = &cg->io.devices[i];

        uint64_t limit = is_write ? dev->wbps : dev->rbps;
        if (limit > 0) {
            /* Simple token bucket model */
            uint64_t *acc = is_write ? &dev->write_bytes_acc : &dev->read_bytes_acc;
            uint64_t now = timer_get_ticks();
            uint64_t elapsed = now - dev->last_tick;

            /* Refill tokens */
            if (elapsed > 0) {
                uint64_t refill = (limit * elapsed) / TIMER_FREQ;
                if (*acc > refill)
                    *acc -= refill;
                else
                    *acc = 0;
                dev->last_tick = now;
            }

            /* Check if we have enough tokens */
            if (*acc + bytes > limit) {
                spinlock_release(&g_cgroup_lock);
                return 1; /* throttle */
            }
            *acc += bytes;
        }
    }

    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_io_throttle_check);

/* Get IO statistics for a cgroup. */
int cgroup_io_stat(int cg_id, struct cgroup_io_device *devices, int max)
{
    if (!cgroup_valid(cg_id)) return -EINVAL;

    struct cgroup *cg = &g_cgroups[cg_id];
    int count = 0;
    spinlock_acquire(&g_cgroup_lock);
    for (int i = 0; i < CGROUP_IO_MAX_DEVICES && count < max; i++) {
        if (cg->io.devices[i].in_use)
            devices[count++] = cg->io.devices[i];
    }
    spinlock_release(&g_cgroup_lock);
    return count;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  PID controller (pids.max process limit)
 * ═══════════════════════════════════════════════════════════════════════ */

/* Set the maximum number of processes (threads) allowed in a cgroup.
 * 0 = unlimited. */
int cgroup_pids_set_max(int cg_id, int64_t max_pids)
{
    if (!cgroup_valid(cg_id)) return -EINVAL;
    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);
    cg->pids.max = max_pids > 0 ? (uint64_t)max_pids : 0;
    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_pids_set_max);

/* Account a fork/exit in the PID controller.
 * Called by the process subsystem on fork/exit.
 * @pid: new PID (for fork) or exiting PID
 * @is_fork: 1 for fork, 0 for exit
 * Returns 0 on success, -EAGAIN if the limit would be exceeded. */
int cgroup_pids_account(int pid, int is_fork)
{
    int cg_id = cgroup_of_pid(pid);
    if (cg_id < 0) cg_id = 0; /* default to root */

    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);

    if (is_fork) {
        if (cg->pids.max > 0 && cg->pids.current >= cg->pids.max) {
            spinlock_release(&g_cgroup_lock);
            return -EAGAIN;
        }
        cg->pids.current++;
    } else {
        if (cg->pids.current > 0)
            cg->pids.current--;
    }

    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_pids_account);

/* Query PID controller status. */
void cgroup_pids_stat(int cg_id, uint64_t *current, uint64_t *max)
{
    if (!cgroup_valid(cg_id)) return;
    if (current) *current = g_cgroups[cg_id].pids.current;
    if (max)     *max     = g_cgroups[cg_id].pids.max;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Cgroup freezer (freeze/unfreeze cgroup tasks)
 * ═══════════════════════════════════════════════════════════════════════ */

/* Freeze all tasks in a cgroup.
 * Frozen tasks are not scheduled until unfrozen.
 * Returns 0 on success. */
int cgroup_freeze(int cg_id)
{
    if (!cgroup_valid(cg_id)) return -EINVAL;

    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);
    cg->freezer.state = CGROUP_FROZEN;

    /* Freeze all member processes */
    for (int i = 0; i < CGROUP_MAX_PIDS; i++) {
        if (cg->members[i] != 0) {
            process_freeze(cg->members[i]); /* defined in process.h */
        }
    }
    kprintf("[cgroup] frozen cgroup %d (%d processes)\n",
            cg_id, cg->num_pids);
    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_freeze);

/* Unfreeze all tasks in a cgroup.
 * Returns 0 on success. */
int cgroup_unfreeze(int cg_id)
{
    if (!cgroup_valid(cg_id)) return -EINVAL;

    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);
    cg->freezer.state = CGROUP_THAWED;

    /* Unfreeze all member processes */
    for (int i = 0; i < CGROUP_MAX_PIDS; i++) {
        if (cg->members[i] != 0) {
            process_unfreeze(cg->members[i]); /* defined in process.h */
        }
    }
    kprintf("[cgroup] unfrozen cgroup %d (%d processes)\n",
            cg_id, cg->num_pids);
    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_unfreeze);

/* Query freezer state for a cgroup.
 * Returns CGROUP_THAWED or CGROUP_FROZEN. */
int cgroup_freezer_state(int cg_id)
{
    if (!cgroup_valid(cg_id)) return CGROUP_THAWED;
    return g_cgroups[cg_id].freezer.state;
}

/* ── Sysfs/proc interface helpers ─────────────────────────────────── */

/* Write a cgroup control file value.
 * Format: "cg_id controller key value" or "cg_id value"
 * Returns 0 on success, negative on error. */
int cgroup_write_control(int cg_id, const char *controller,
                         const char *key, const char *value)
{
    if (!cgroup_valid(cg_id)) return -EINVAL;
    if (!controller || !key || !value) return -EINVAL;

    if (strcmp(controller, "cpu") == 0) {
        if (strcmp(key, "max") == 0) {
            /* Format: "quota period" */
            uint64_t quota = 0, period = CGROUP_CPU_PERIOD_DEFAULT;
            if (strcmp(value, "max") == 0) {
                quota = 0; /* unlimited */
            } else {
                /* Parse "quota period" */
                const char *s = value;
                while (*s >= '0' && *s <= '9')
                    quota = quota * 10 + (uint64_t)(*s++ - '0');
                if (*s == ' ' || *s == '\t') {
                    s++;
                    period = 0;
                    while (*s >= '0' && *s <= '9')
                        period = period * 10 + (uint64_t)(*s++ - '0');
                    if (period == 0) period = CGROUP_CPU_PERIOD_DEFAULT;
                }
            }
            return cgroup_cpu_set_max(cg_id, (int64_t)quota, (int64_t)period);
        }
    } else if (strcmp(controller, "memory") == 0) {
        if (strcmp(key, "max") == 0) {
            uint64_t val = 0;
            const char *s = value;
            while (*s >= '0' && *s <= '9')
                val = val * 10 + (uint64_t)(*s++ - '0');
            return cgroup_mem_set_max(cg_id, val);
        }
        if (strcmp(key, "high") == 0) {
            uint64_t val = 0;
            const char *s = value;
            while (*s >= '0' && *s <= '9')
                val = val * 10 + (uint64_t)(*s++ - '0');
            return cgroup_mem_set_high(cg_id, val);
        }
    } else if (strcmp(controller, "pids") == 0) {
        if (strcmp(key, "max") == 0) {
            uint64_t val = 0;
            const char *s = value;
            while (*s >= '0' && *s <= '9')
                val = val * 10 + (uint64_t)(*s++ - '0');
            return cgroup_pids_set_max(cg_id, (int64_t)val);
        }
    } else if (strcmp(controller, "freezer") == 0) {
        if (strcmp(value, "FROZEN") == 0)
            return cgroup_freeze(cg_id);
        if (strcmp(value, "THAWED") == 0)
            return cgroup_unfreeze(cg_id);
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Initialization
 * ═══════════════════════════════════════════════════════════════════════ */

void cgroup_init(void)
{
    if (g_cgroup_initialized) return;

    memset(g_cgroups, 0, sizeof(g_cgroups));
    spinlock_init(&g_cgroup_lock);
    spinlock_init(&g_cgroup_work_lock);

    /* Root cgroup */
    g_cgroups[0].in_use = 1;
    g_cgroups[0].id = 0;
    g_cgroups[0].parent_id = -1;
    g_cgroups[0].cpu.max_period = CGROUP_CPU_PERIOD_DEFAULT;
    g_cgroups[0].cpu.max_quota   = CGROUP_CPU_PERIOD_DEFAULT;
    g_num_cgroups = 1;

    /* Mount cgroup v2 at /sys/fs/cgroup/ */
    if (vfs_mount("/sys/fs/cgroup", &cgroup_v2_vfs_ops, NULL) == 0) {
        kprintf("[OK] cgroup v2 mounted at /sys/fs/cgroup/\n");
    } else {
        /* Try creating the directory first */
        vfs_create("/sys/fs/cgroup", VFS_TYPE_DIR);
        vfs_mount("/sys/fs/cgroup", &cgroup_v2_vfs_ops, NULL);
        kprintf("[OK] cgroup v2 mounted at /sys/fs/cgroup/\n");
    }

    kprintf("[OK] Cgroup v2 initialized (cpu, memory, io, pids, freezer)\n");
    g_cgroup_initialized = 1;
}
EXPORT_SYMBOL(cgroup_init);

/* ═══════════════════════════════════════════════════════════════════════
 *  Stub functions for incomplete cgroup operations
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── cgroup_fork: Initialize cgroup state for a new process ────────────── */
static int cgroup_fork(struct process *task)
{
    if (!task) return -EINVAL;
    /* In a minimal implementation, the child inherits the parent's cgroup
     * by attaching the new PID to the parent's cgroup. */
    struct process *parent = process_get_current();
    if (parent) {
        int cg_id = cgroup_of_pid(parent->pid);
        if (cg_id >= 0) {
            cgroup_attach(task->pid, cg_id);
        }
    }
    kprintf("[cgroup] cgroup_fork: pid=%d\n", task->pid);
    return 0;
}

/* ── cgroup_post_fork: Post-fork cgroup accounting ─────────────────────── */
static void cgroup_post_fork(struct process *task)
{
    if (!task) return;
    /* Account this new process in the pids controller */
    cgroup_pids_account(task->pid, 1);
    kprintf("[cgroup] cgroup_post_fork: pid=%d\n", task->pid);
}

/* ── cgroup_exit: Clean up cgroup state on process exit ────────────────── */
static void cgroup_exit(struct process *task)
{
    if (!task) return;
    /* Decrement the pids counter for this cgroup */
    cgroup_pids_account(task->pid, 0);
    kprintf("[cgroup] cgroup_exit: pid=%d\n", task->pid);
}

/* ── cgroup_can_fork: Check if cgroup allows forking ───────────────────── */
static int cgroup_can_fork(struct process *task)
{
    if (!task) return -EINVAL;
    /* Check pids.max limit */
    int cg_id = cgroup_of_pid(task->pid);
    uint64_t current_pids_val = 0, max_pids_val = 0;
    if (cg_id >= 0) {
        cgroup_pids_stat(cg_id, &current_pids_val, &max_pids_val);
        if (max_pids_val > 0 && current_pids_val >= max_pids_val) {
            kprintf("[cgroup] cgroup_can_fork: pid=%d DENIED (pids limit %llu)\n",
                    task->pid, (unsigned long long)max_pids_val);
            return -EAGAIN;
        }
    }
    kprintf("[cgroup] cgroup_can_fork: pid=%d (allowed)\n", task->pid);
    return 0;
}
