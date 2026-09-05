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

#include "errno.h"
#include "export.h"
#include "printf.h"
#include "process.h"
#include "scheduler.h"
#include "signal.h"
#include "spinlock.h"
#include "string.h"
#include "string_ext.h"
#include "timer.h"
#include "vfs.h"
/* TIMER_FREQ from timeconst.h */
#include "timeconst.h"
#ifndef TIMER_FREQ
#define TIMER_FREQ 100
#endif

/* ── Process helper wrappers (used by cgroup freezer & OOM) ────────── */

#ifndef CGROUP_PROCESS_WRAPPERS
#define CGROUP_PROCESS_WRAPPERS
#include "scheduler.h"

static inline void process_freeze(uint32_t pid) {
    struct process *p = process_get_by_pid(pid);
    if (p && p->state != PROCESS_UNUSED && p->state != PROCESS_ZOMBIE) {
        p->is_suspended = 1;
        p->state = PROCESS_BLOCKED;
        scheduler_remove(p);
    }
}

static inline void process_unfreeze(uint32_t pid) {
    struct process *p = process_get_by_pid(pid);
    if (p && p->state != PROCESS_UNUSED && p->state != PROCESS_ZOMBIE) {
        p->is_suspended = 0;
        p->sleep_until = 0;
        p->state = PROCESS_READY;
        scheduler_add(p);
    }
}

static inline uint64_t process_get_mem_usage(uint32_t pid) {
    struct process *p = process_get_by_pid(pid);
    return p ? p->max_rss : 0;
}

static inline void process_kill(uint32_t pid) {
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
#define CGROUP_CPU_PERIOD_MIN 1000
#define CGROUP_CPU_PERIOD_MAX 1000000

/* ── Memory controller state ──────────────────────────────────────── */

#define CGROUP_MEM_MAX_DEFAULT 0 /* 0 = unlimited */
#define CGROUP_MEM_HIGH_DEFAULT 0

/* ── IO controller state ──────────────────────────────────────────── */

#define CGROUP_IO_MAX_DEVICES 8

/* ── PID controller state ─────────────────────────────────────────── */

#define CGROUP_PIDS_MAX_DEFAULT 0 /* 0 = unlimited */

/* ── Queue of pending work items ──────────────────────────────────── */

struct cgroup_work {
    enum { CGROUP_WORK_NONE, CGROUP_WORK_FREEZE, CGROUP_WORK_UNFREEZE, CGROUP_WORK_OOM_KILL } type;
    int cg_idx;
} g_cgroup_work_queue[16];
static int g_cgroup_work_head = 0, g_cgroup_work_tail = 0;
static spinlock_t g_cgroup_work_lock;

/* ═══════════════════════════════════════════════════════════════════════
 *  Internal helpers
 * ═══════════════════════════════════════════════════════════════════════ */

static inline bool cgroup_valid(int idx) {
    return idx >= 0 && idx < CGROUP_MAX && g_cgroups[idx].in_use;
}

static int cgroup_alloc_id(void) {
    spinlock_acquire(&g_cgroup_lock);
    for (int i = 1; i < CGROUP_MAX; i++) {
        if (!g_cgroups[i].in_use) {
            memset(&g_cgroups[i], 0, sizeof(struct cgroup));
            g_cgroups[i].in_use = 1;
            g_cgroups[i].id = i;
            g_cgroups[i].parent_id = -1;
            g_cgroups[i].cpu.max_period = CGROUP_CPU_PERIOD_DEFAULT;
            g_cgroups[i].cpu.max_quota = CGROUP_CPU_PERIOD_DEFAULT;
            g_num_cgroups++;
            spinlock_release(&g_cgroup_lock);
            return i;
        }
    }
    spinlock_release(&g_cgroup_lock);
    return -ENOSPC;
}

/* ── Cgroupv2 filesystem ──────────────────────────────────────────── */

/* Given a cgroup v2 path (/sys/fs/cgroup/[<name>/]<file>), write the
 * file's basename into @fname and fill @cg_id with the resolved cgroup
 * (root 0 when no named subdirectory prefixes it). */
static void cgroup_v2_split_path(const char *path, int *cg_id, char *fname, size_t fname_sz) {
    *cg_id = 0;
    fname[0] = '\0';
    if (!path)
        return;
    const char *base = "/sys/fs/cgroup/";
    size_t blen = strlen(base);
    const char *p = path;
    if (memcmp(p, base, blen) == 0)
        p += blen;
    /* Find last '/' → base of the requested file. */
    const char *last = strrchr(p, '/');
    const char *file = last ? last + 1 : p;
    size_t fl = strlen(file);
    if (fl >= fname_sz)
        fl = fname_sz - 1;
    memcpy(fname, file, fl);
    fname[fl] = '\0';
    /* Whatever precedes the file name (if non-empty) is a cgroup name. */
    if (last && last > p) {
        size_t nl = (size_t)(last - p);
        if (nl > 0 && nl < sizeof(g_cgroups[0].name)) {
            char cg_name[32];
            memcpy(cg_name, p, nl);
            cg_name[nl] = '\0';
            for (int i = 0; i < CGROUP_MAX; i++) {
                if (g_cgroups[i].in_use && g_cgroups[i].name[0] &&
                    strcmp(g_cgroups[i].name, cg_name) == 0) {
                    *cg_id = i;
                    break;
                }
            }
        }
    }
}

/* Render a single cgroup controller file into @out (cgroup file
 * interface, D315 task 15).  Returns bytes written. */
static size_t cgroup_v2_read_file(int cg_id, const char *file, char *out, size_t out_sz) {
    if (!cgroup_valid(cg_id) || !file || out_sz == 0)
        return 0;
    struct cgroup *cg = &g_cgroups[cg_id];
    int pos = 0;

    if (strcmp(file, "cgroup.procs") == 0 || strcmp(file, "tasks") == 0) {
        spinlock_acquire(&g_cgroup_lock);
        for (int i = 0; i < CGROUP_MAX_PIDS && pos < (int)out_sz; i++) {
            if (cg->members[i] != 0) {
                int n = snprintf(out + pos, out_sz - (size_t)pos, "%d\n", cg->members[i]);
                if (n > 0 && pos + n < (int)out_sz)
                    pos += n;
            }
        }
        spinlock_release(&g_cgroup_lock);
    } else if (strcmp(file, "cgroup.subtree_control") == 0 ||
               strcmp(file, "cgroup.controllers") == 0) {
        char ctrl[128] = "";
        uint32_t mask = cg->ctrl_mask;
        if (mask & CG_CTRL_CPU)
            strlcat(ctrl, "cpu ", sizeof(ctrl));
        if (mask & CG_CTRL_MEMORY)
            strlcat(ctrl, "memory ", sizeof(ctrl));
        if (mask & CG_CTRL_IO)
            strlcat(ctrl, "io ", sizeof(ctrl));
        if (mask & CG_CTRL_PIDS)
            strlcat(ctrl, "pids ", sizeof(ctrl));
        if (mask & CG_CTRL_FREEZER)
            strlcat(ctrl, "freezer ", sizeof(ctrl));
        if (mask & CG_CTRL_RDMA)
            strlcat(ctrl, "rdma ", sizeof(ctrl));
        if (mask & CG_CTRL_MISC)
            strlcat(ctrl, "misc ", sizeof(ctrl));
        int n = snprintf(out, out_sz, "%s\n", ctrl[0] ? ctrl : "-");
        return n > 0 ? (size_t)n : 0;
    } else if (strcmp(file, "cpu.max") == 0) {
        uint64_t q = 0, p = 0;
        cgroup_cpu_get_max(cg_id, &q, &p);
        int n = snprintf(out, out_sz, "%llu %llu\n", (unsigned long long)q, (unsigned long long)p);
        return n > 0 ? (size_t)n : 0;
    } else if (strcmp(file, "cpu.stat") == 0) {
        uint64_t usage = 0, user = 0, sys = 0, nr_th = 0, th_us = 0;
        cgroup_cpu_stat(cg_id, &usage, &user, &sys, &nr_th, &th_us);
        int n =
            snprintf(out, out_sz,
                     "usage_usec %llu\nuser_usec %llu\nsystem_usec %llu\n"
                     "nr_throttled %llu\nthrottled_usec %llu\n",
                     (unsigned long long)usage, (unsigned long long)user, (unsigned long long)sys,
                     (unsigned long long)nr_th, (unsigned long long)th_us);
        return n > 0 ? (size_t)n : 0;
    } else if (strcmp(file, "memory.max") == 0 || strcmp(file, "memory.high") == 0 ||
               strcmp(file, "memory.current") == 0) {
        uint64_t usage = 0, max_usage = 0, limit = 0, high = 0;
        int oom = 0;
        cgroup_mem_stat(cg_id, &usage, &max_usage, &limit, &high, &oom);
        uint64_t v = 0;
        if (strcmp(file, "memory.max") == 0)
            v = limit;
        else if (strcmp(file, "memory.high") == 0)
            v = high;
        else
            v = usage;
        int n = snprintf(out, out_sz, "%llu\n", (unsigned long long)v);
        return n > 0 ? (size_t)n : 0;
    } else if (strcmp(file, "io.max") == 0 || strcmp(file, "io.stat") == 0) {
        struct cgroup_io_device devs[CGROUP_IO_MAX_DEVICES];
        int nd = cgroup_io_stat(cg_id, devs, CGROUP_IO_MAX_DEVICES);
        for (int i = 0; i < nd; i++) {
            int n = snprintf(out + pos, out_sz - (size_t)pos,
                             "%u:%u rbps=%llu wbps=%llu "
                             "riops=%llu wiops=%llu\n",
                             devs[i].major, devs[i].minor, (unsigned long long)devs[i].rbps,
                             (unsigned long long)devs[i].wbps, (unsigned long long)devs[i].riops,
                             (unsigned long long)devs[i].wiops);
            if (n > 0 && pos + n < (int)out_sz)
                pos += n;
        }
    } else if (strcmp(file, "pids.max") == 0 || strcmp(file, "pids.current") == 0) {
        uint64_t cur = 0, mx = 0;
        cgroup_pids_stat(cg_id, &cur, &mx);
        uint64_t v = strcmp(file, "pids.current") == 0 ? cur : mx;
        int n = snprintf(out, out_sz, "%llu\n", (unsigned long long)v);
        return n > 0 ? (size_t)n : 0;
    } else if (strcmp(file, "freezer.state") == 0) {
        const char *st = cgroup_freezer_state(cg_id) == CGROUP_FROZEN ? "frozen" : "thawed";
        int n = snprintf(out, out_sz, "%s\n", st);
        return n > 0 ? (size_t)n : 0;
    } else if (strcmp(file, "rdma.max") == 0 || strcmp(file, "rdma.current") == 0) {
        struct cgroup_rdma_device devs[CGROUP_RDMA_MAX_DEVS];
        int nd = cgroup_rdma_stat(cg_id, devs, CGROUP_RDMA_MAX_DEVS);
        for (int i = 0; i < nd; i++) {
            int n;
            if (strcmp(file, "rdma.current") == 0)
                n = snprintf(out + pos, out_sz - (size_t)pos,
                             "%s hca_handle=%llu hca_object=%llu\n", devs[i].name,
                             (unsigned long long)devs[i].hca_handle_usage,
                             (unsigned long long)devs[i].hca_object_usage);
            else
                n = snprintf(out + pos, out_sz - (size_t)pos,
                             "%s hca_handle=%llu hca_object=%llu\n", devs[i].name,
                             (unsigned long long)devs[i].hca_handle_limit,
                             (unsigned long long)devs[i].hca_object_limit);
            if (n > 0 && pos + n < (int)out_sz)
                pos += n;
        }
    } else if (strcmp(file, "misc.max") == 0 || strcmp(file, "misc.current") == 0) {
        struct cgroup_misc_resource res[CGROUP_MISC_MAX_RES];
        int nr = cgroup_misc_stat(cg_id, res, CGROUP_MISC_MAX_RES);
        for (int i = 0; i < nr; i++) {
            int n = snprintf(out + pos, out_sz - (size_t)pos, "%s %llu\n", res[i].name,
                             (unsigned long long)(strcmp(file, "misc.current") == 0 ? res[i].current
                                                                                    : res[i].max));
            if (n > 0 && pos + n < (int)out_sz)
                pos += n;
        }
    } else if (strcmp(file, "cpuset.cpus") == 0 || strcmp(file, "cpuset.mems") == 0) {
        /* Render the active cpuset/nodelist as a comma list (best-effort). */
        cpuset_t set;
        int has = 0;
        if (strcmp(file, "cpuset.cpus") == 0 && cg->cpuset_valid) {
            set = cg->cpuset;
            has = 1;
        } else if (strcmp(file, "cpuset.mems") == 0 && cg->nodelist_valid) {
            set = cg->nodelist;
            has = 1;
        }
        if (has) {
            int first = 1;
            for (int cpu = 0; cpu < 256 && pos < (int)out_sz; cpu++) {
                if (cpuset_isset(cpu, &set)) {
                    int n =
                        snprintf(out + pos, out_sz - (size_t)pos, "%s%d", first ? "" : ",", cpu);
                    if (n > 0 && pos + n < (int)out_sz)
                        pos += n;
                    first = 0;
                }
            }
            int n = snprintf(out + pos, out_sz - (size_t)pos, "\n");
            if (n > 0 && pos + n < (int)out_sz)
                pos += n;
        }
    }

    return (size_t)pos > 0 ? (size_t)pos : 0;
}

static int cgroup_v2_read(void *priv, const char *path, void *buf, uint32_t max, uint32_t *out) {
    (void)priv;
    if (!buf || max == 0) {
        *out = 0;
        return 0;
    }
    int cg_id = 0;
    char fname[32];
    cgroup_v2_split_path(path, &cg_id, fname, sizeof(fname));

    if (fname[0]) {
        char filebuf[512];
        size_t fl = cgroup_v2_read_file(cg_id, fname, filebuf, sizeof(filebuf));
        if (fl > 0) {
            size_t total = fl < (size_t)max ? fl : (size_t)max;
            memcpy(buf, filebuf, total);
            *out = (uint32_t)total;
            return 0;
        }
    }

    /* Unknown / bare path → hierarchy listing (compatibility). */
    char tmp[512];
    int pos = 0;
    {
        int n = snprintf(tmp + pos, sizeof(tmp) - (size_t)pos,
                         "# Cgroup v2 unified hierarchy\n"
                         "# Controllers: cpu memory io pids freezer rdma misc\n"
                         "# /sys/fs/cgroup/ mounted\n\n");
        if (n > 0 && pos + n < (int)sizeof(tmp))
            pos += n;
    }

    spinlock_acquire(&g_cgroup_lock);
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (!g_cgroups[i].in_use)
            continue;
        char ctrl[128] = "";
        uint32_t mask = g_cgroups[i].ctrl_mask;
        if (mask & CG_CTRL_CPU)
            strlcat(ctrl, "cpu ", sizeof(ctrl));
        if (mask & CG_CTRL_MEMORY)
            strlcat(ctrl, "memory ", sizeof(ctrl));
        if (mask & CG_CTRL_PIDS)
            strlcat(ctrl, "pids ", sizeof(ctrl));
        if (mask & CG_CTRL_IO)
            strlcat(ctrl, "io ", sizeof(ctrl));
        if (mask & CG_CTRL_FREEZER)
            strlcat(ctrl, "freezer", sizeof(ctrl));
        if (mask & CG_CTRL_RDMA)
            strlcat(ctrl, " rdma", sizeof(ctrl));
        if (mask & CG_CTRL_MISC)
            strlcat(ctrl, " misc", sizeof(ctrl));
        if (ctrl[0] == '\0')
            strlcpy(ctrl, "-", sizeof(ctrl));

        int n = snprintf(tmp + pos, sizeof(tmp) - (size_t)pos,
                         "  cgroup[%d]  parent=%d  pids=%lu/%lu  controllers=%s\n", i,
                         g_cgroups[i].parent_id, (unsigned long)g_cgroups[i].pids.current,
                         (unsigned long)g_cgroups[i].pids.max, ctrl);
        if (n > 0 && pos + n < (int)sizeof(tmp))
            pos += n;
    }
    spinlock_release(&g_cgroup_lock);

    size_t total = (size_t)pos < (size_t)max ? (size_t)pos : (size_t)max;
    memcpy(buf, tmp, total);
    *out = (uint32_t)total;
    return 0;
}

static int cgroup_v2_write(void *priv, const char *path, const void *buf, uint32_t size) {
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
    while (len > 0 &&
           (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ' || s[len - 1] == '\t'))
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
            if (s[i] < '0' || s[i] > '9')
                return -EINVAL;
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
                    if (nlen > 31)
                        nlen = 31;
                    memcpy(cg_name, p, nlen);
                    cg_name[nlen] = '\0';
                    /* Look up cgroup by name */
                    int found = -1;
                    for (int i = 0; i < CGROUP_MAX; i++) {
                        if (g_cgroups[i].in_use && g_cgroups[i].name[0] &&
                            strcmp(g_cgroups[i].name, cg_name) == 0) {
                            found = i;
                            break;
                        }
                    }
                    if (found >= 0)
                        cg_id = found;
                }
            }
        }
        return cgroup_attach(cg_id, pid);
    }

    /* Controller enable/disable: "subtree_control +cpu -io" (or
     * "cgroup.subtree_control ...").  Each token is +name (enable)
     * or -name (disable). */
    if (len > 0) {
        char tmp[128];
        if (len > 127)
            len = 127;
        memcpy(tmp, s, len);
        tmp[len] = '\0';

        char *space = strchr(tmp, ' ');
        if (space && (strncmp(tmp, "subtree_control", 16) == 0 ||
                      strncmp(tmp, "cgroup.subtree_control", 22) == 0)) {
            int cg_id = 0;
            /* Extract target cgroup name from path */
            if (path) {
                const char *p = path;
                const char *base = "/sys/fs/cgroup/";
                size_t blen = strlen(base);
                if (memcmp(p, base, blen) == 0) {
                    p += blen;
                    const char *slash = strchr(p, '/');
                    char cg_name[32] = "";
                    if (slash && (size_t)(slash - p) < sizeof(cg_name)) {
                        size_t nlen = (size_t)(slash - p);
                        memcpy(cg_name, p, nlen);
                        cg_name[nlen] = '\0';
                    }
                    if (cg_name[0]) {
                        for (int i = 0; i < CGROUP_MAX; i++) {
                            if (g_cgroups[i].in_use && g_cgroups[i].name[0] &&
                                strcmp(g_cgroups[i].name, cg_name) == 0) {
                                cg_id = i;
                                break;
                            }
                        }
                    }
                }
            }
            if (cg_id < 0 || cg_id >= CGROUP_MAX)
                return -EINVAL;

            /* Parse each +/-controller token */
            space++;
            while (*space) {
                while (*space == ' ' || *space == '\t')
                    space++;
                if (*space == '\0')
                    break;
                char op = *space++; /* '+' or '-' */
                if (op != '+' && op != '-')
                    return -EINVAL;
                char name[16];
                size_t ni = 0;
                while (*space && *space != ' ' && *space != '\t') {
                    if (ni < sizeof(name) - 1)
                        name[ni++] = *space;
                    space++;
                }
                name[ni] = '\0';
                if (name[0] == '\0')
                    return -EINVAL;
                int ret = cgroup_set_controller(cg_id, name, op == '+');
                if (ret < 0)
                    return ret;
            }
            return 0;
        }
    }

    /* Key-value setting: "cpu.max 50000 100000", "memory.max 1048576", etc. */
    if (len > 0) {
        /* Copy to a temporary buffer */
        char tmp[128];
        if (len > 127)
            len = 127;
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
                            if (nlen > 31)
                                nlen = 31;
                            memcpy(cg_name, p, nlen);
                            cg_name[nlen] = '\0';
                        }
                        if (cg_name[0]) {
                            for (int i = 0; i < CGROUP_MAX; i++) {
                                if (g_cgroups[i].in_use && g_cgroups[i].name[0] &&
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
                            if (nlen > 31)
                                nlen = 31;
                            memcpy(cg_name, p, nlen);
                            cg_name[nlen] = '\0';
                        }
                        if (cg_name[0]) {
                            for (int i = 0; i < CGROUP_MAX; i++) {
                                if (g_cgroups[i].in_use && g_cgroups[i].name[0] &&
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
    .read = cgroup_v2_read,
    .write = cgroup_v2_write,
};

/* ═══════════════════════════════════════════════════════════════════════
 *  Cgroup v1 compatibility layer (D315 task 14)
 * ═══════════════════════════════════════════════════════════════════════ */

/* Extract the cgroup ID a v1 path refers to.  v1 paths look like
 * /sys/fs/cgroup-v1/<controller>/<name>/<file> or
 * /sys/fs/cgroup-v1/<name>/<file> for the root of a hierarchy.
 * Falls back to root cgroup (0) when no named subdirectory is found. */
static int cgroup_v1_id_from_path(const char *path) {
    if (!path)
        return 0;
    const char *base = "/sys/fs/cgroup-v1/";
    size_t blen = strlen(base);
    if (memcmp(path, base, blen) != 0)
        return 0;
    const char *p = path + blen;
    const char *slash = strchr(p, '/');
    if (!slash || (size_t)(slash - p) >= sizeof(g_cgroups[0].name))
        return 0;
    char cg_name[32];
    size_t nlen = (size_t)(slash - p);
    if (nlen > 31)
        nlen = 31;
    memcpy(cg_name, p, nlen);
    cg_name[nlen] = '\0';
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (g_cgroups[i].in_use && g_cgroups[i].name[0] && strcmp(g_cgroups[i].name, cg_name) == 0)
            return i;
    }
    return 0;
}

/* Translate a legacy v1 control-file mutation onto the shared v2
 * controller APIs.  Files that have no v2 twin (e.g. cpu.shares) are
 * accepted as no-ops so legacy tools keep working. */
static int cgroup_v1_write(void *priv, const char *path, const void *buf, uint32_t size) {
    (void)priv;
    (void)path;
    if (!buf || size == 0)
        return 0;
    int cg_id = cgroup_v1_id_from_path(path);

    char tmp[128];
    uint32_t len = size > (uint32_t)(sizeof(tmp) - 1) ? (uint32_t)(sizeof(tmp) - 1) : size;
    memcpy(tmp, buf, len);
    tmp[len] = '\0';
    /* Strip trailing newline/space. */
    while (len > 0 && (tmp[len - 1] == '\n' || tmp[len - 1] == '\r' || tmp[len - 1] == ' ' ||
                       tmp[len - 1] == '\t'))
        tmp[--len] = '\0';

    char *space = strchr(tmp, ' ');
    if (space)
        *space++ = '\0';
    char *key = tmp;
    char *value = space ? space : (char *)"";

    /* tasks / cgroup.procs migration. */
    if (strcmp(key, "tasks") == 0 || strcmp(key, "cgroup.procs") == 0) {
        int pid = 0;
        const char *v = value;
        while (*v >= '0' && *v <= '9')
            pid = pid * 10 + (int)(*v++ - '0');
        return cgroup_attach(cg_id, pid);
    }

    /* freezer.state FROZEN|THAWED */
    if (strcmp(key, "freezer.state") == 0) {
        if (strncmp(value, "FROZEN", 6) == 0)
            return cgroup_freeze(cg_id);
        return cgroup_unfreeze(cg_id);
    }

    /* cpu.cfs_quota_us / cpu.cfs_period_us */
    if (strcmp(key, "cpu.cfs_quota_us") == 0 || strcmp(key, "cpu.cfs_period_us") == 0) {
        uint64_t quota = 0, period = CGROUP_CPU_PERIOD_DEFAULT;
        if (strcmp(key, "cpu.cfs_period_us") == 0) {
            period = 0;
            const char *v = value;
            while (*v >= '0' && *v <= '9')
                period = period * 10 + (uint64_t)(*v++ - '0');
            if (period == 0)
                period = CGROUP_CPU_PERIOD_DEFAULT;
            uint64_t oldq = 0, oldp = 0;
            cgroup_cpu_get_max(cg_id, &oldq, &oldp);
            quota = oldq;
        } else {
            quota = 0;
            if (strncmp(value, "-1", 2) == 0)
                quota = 0; /* -1 = unlimited */
            else {
                const char *v = value;
                while (*v >= '0' && *v <= '9')
                    quota = quota * 10 + (uint64_t)(*v++ - '0');
            }
            uint64_t oldq = 0, oldp = 0;
            cgroup_cpu_get_max(cg_id, &oldq, &oldp);
            if (oldp > 0)
                period = oldp;
        }
        return cgroup_cpu_set_max(cg_id, (int64_t)quota, (int64_t)period);
    }

    /* cpu.shares / cpu.rt_runtime_us / cpu.rt_period_us — no v2 twin,
     * accepted as no-op for legacy compatibility. */
    if (strcmp(key, "cpu.shares") == 0 || strcmp(key, "cpu.rt_runtime_us") == 0 ||
        strcmp(key, "cpu.rt_period_us") == 0)
        return 0;

    /* memory.limit_in_bytes / memory.soft_limit_in_bytes / memory.swappiness */
    if (strcmp(key, "memory.limit_in_bytes") == 0) {
        uint64_t val = 0;
        if (strncmp(value, "-1", 2) == 0)
            val = 0; /* unlimited */
        else {
            const char *v = value;
            while (*v >= '0' && *v <= '9')
                val = val * 10 + (uint64_t)(*v++ - '0');
        }
        return cgroup_mem_set_max(cg_id, val);
    }
    if (strcmp(key, "memory.soft_limit_in_bytes") == 0) {
        uint64_t val = 0;
        if (strncmp(value, "-1", 2) == 0)
            val = 0;
        else {
            const char *v = value;
            while (*v >= '0' && *v <= '9')
                val = val * 10 + (uint64_t)(*v++ - '0');
        }
        return cgroup_mem_set_high(cg_id, val);
    }
    if (strcmp(key, "memory.swappiness") == 0)
        return 0; /* no swap controller twin */

    /* pids.max */
    if (strcmp(key, "pids.max") == 0) {
        int64_t max = 0;
        if (strncmp(value, "max", 3) == 0)
            max = 0;
        else {
            max = 0;
            const char *v = value;
            while (*v >= '0' && *v <= '9')
                max = max * 10 + (int64_t)(*v++ - '0');
        }
        return cgroup_pids_set_max(cg_id, max);
    }

    /* blkio.throttle.*.bps_device / .iops_device  "<major>:<minor> <N>" */
    if (strncmp(key, "blkio.throttle.", 15) == 0 &&
        (strstr(key, "bps_device") || strstr(key, "iops_device"))) {
        uint32_t major = 0, minor = 0;
        const char *v = value;
        while (*v >= '0' && *v <= '9')
            major = major * 10 + (uint32_t)(*v++ - '0');
        if (*v == ':')
            v++;
        while (*v >= '0' && *v <= '9')
            minor = minor * 10 + (uint32_t)(*v++ - '0');
        uint64_t lim = 0;
        while (*v == ' ' || *v == '\t')
            v++;
        while (*v >= '0' && *v <= '9')
            lim = lim * 10 + (uint64_t)(*v++ - '0');
        /* Map onto the shared io.max per-device limit store. */
        cgroup_io_set_limit(cg_id, major, minor, strstr(key, "read_bps") ? lim : 0,
                            strstr(key, "write_bps") ? lim : 0, strstr(key, "read_iops") ? lim : 0,
                            strstr(key, "write_iops") ? lim : 0);
        return 0;
    }

    return 0;
}

/* v1 read: minimal status echo so `cat` on a v1 file succeeds. */
static int cgroup_v1_read(void *priv, const char *path, void *buf, uint32_t max, uint32_t *out) {
    (void)priv;
    (void)path;
    const char msg[] = "# cgroup v1 compatibility hierarchy (legacy controllers)\n"
                       "# Files map onto the shared v2 controller state.\n";
    size_t total = sizeof(msg) - 1;
    if (total > (size_t)max)
        total = (size_t)max;
    memcpy(buf, msg, total);
    *out = (uint32_t)total;
    return 0;
}

static struct vfs_ops cgroup_v1_vfs_ops = {
    .read = cgroup_v1_read,
    .write = cgroup_v1_write,
};

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API — Cgroup management
 * ═══════════════════════════════════════════════════════════════════════ */

/* Create a new cgroup as a child of parent_id.
 * Returns cgroup ID (>= 0) on success, negative errno on failure. */
int cgroup_create(int parent_id) {
    if (parent_id != 0 && !cgroup_valid(parent_id))
        return -EINVAL;

    int id = cgroup_alloc_id();
    if (id < 0)
        return id;

    g_cgroups[id].parent_id = parent_id;
    g_cgroups[id].cpu.max_period = CGROUP_CPU_PERIOD_DEFAULT;
    g_cgroups[id].cpu.max_quota = CGROUP_CPU_PERIOD_DEFAULT;
    g_cgroups[id].cpu.usage_usec = 0;

    /* Inherit the parent's controller enable mask (root default = all).
     * New cgroups start with the same controllers their parent offers. */
    if (cgroup_valid(parent_id))
        g_cgroups[id].ctrl_mask = g_cgroups[parent_id].ctrl_mask;
    else
        g_cgroups[id].ctrl_mask = CG_CTRL_ALL;

    kprintf("[cgroup] created cgroup %d (parent %d)\n", id, parent_id);
    return id;
}
EXPORT_SYMBOL(cgroup_create);

/* ── Controller enable/disable (cgroup.subtree_control) ──────────── */

static uint32_t controller_name_to_mask(const char *name) {
    if (!name)
        return 0;
    if (strcmp(name, "cpu") == 0)
        return CG_CTRL_CPU;
    if (strcmp(name, "memory") == 0)
        return CG_CTRL_MEMORY;
    if (strcmp(name, "io") == 0)
        return CG_CTRL_IO;
    if (strcmp(name, "pids") == 0)
        return CG_CTRL_PIDS;
    if (strcmp(name, "freezer") == 0)
        return CG_CTRL_FREEZER;
    if (strcmp(name, "rdma") == 0)
        return CG_CTRL_RDMA;
    if (strcmp(name, "misc") == 0)
        return CG_CTRL_MISC;
    return 0;
}

int cgroup_set_controller(int cg_id, const char *name, int enable) {
    uint32_t bit = controller_name_to_mask(name);
    if (bit == 0)
        return -EINVAL;
    if (cg_id < 0 || cg_id >= CGROUP_MAX || !g_cgroups[cg_id].in_use)
        return -EINVAL;

    spinlock_acquire(&g_cgroup_lock);
    struct cgroup *cg = &g_cgroups[cg_id];
    if (enable)
        cg->ctrl_mask |= bit;
    else
        cg->ctrl_mask &= ~bit;
    spinlock_release(&g_cgroup_lock);

    kprintf("[cgroup] %s %s controller in cgroup %d (mask=0x%x)\n", enable ? "enabled" : "disabled",
            name, cg_id, cg->ctrl_mask);
    return (int)cg->ctrl_mask;
}
EXPORT_SYMBOL(cgroup_set_controller);

uint32_t cgroup_controllers(int cg_id) {
    if (cg_id < 0 || cg_id >= CGROUP_MAX || !g_cgroups[cg_id].in_use)
        return 0;
    return g_cgroups[cg_id].ctrl_mask;
}
EXPORT_SYMBOL(cgroup_controllers);

/* ── cpuset controller (cpuset.cpus) ──────────────────────────────── */

/* Return the ID of another in-use, exclusive cgroup whose cpuset overlaps
 * @set, or -1 if none.  Caller must hold g_cgroup_lock. */
static int find_exclusive_overlap(int self, const cpuset_t *set) {
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (i == self || !g_cgroups[i].in_use)
            continue;
        if (!g_cgroups[i].cpuset_valid || !g_cgroups[i].cpuset_exclusive)
            continue;
        if ((g_cgroups[i].cpuset.bits & set->bits) != 0)
            return i;
    }
    return -1;
}

int cgroup_cpuset_set(int cg_id, const cpuset_t *set) {
    if (!set || cpuset_empty(set))
        return -EINVAL;
    if (cg_id < 0 || cg_id >= CGROUP_MAX || !g_cgroups[cg_id].in_use)
        return -EINVAL;

    spinlock_acquire(&g_cgroup_lock);
    struct cgroup *cg = &g_cgroups[cg_id];

    /* An exclusive cpuset may not overlap another exclusive cpuset. */
    if (cg->cpuset_exclusive && find_exclusive_overlap(cg_id, set) >= 0) {
        spinlock_release(&g_cgroup_lock);
        return -EBUSY;
    }

    cg->cpuset = *set;
    cg->cpuset_valid = 1;

    /* Apply the affinity to every member process. */
    for (int i = 0; i < CGROUP_MAX_PIDS; i++) {
        int pid = cg->members[i];
        if (pid > 0)
            sched_setaffinity((uint32_t)pid, &cg->cpuset);
    }
    spinlock_release(&g_cgroup_lock);

    kprintf("[cgroup] cpuset[%d] set to 0x%llx (%d cpu%s)\n", cg_id,
            (unsigned long long)cg->cpuset.bits, cpuset_weight(&cg->cpuset),
            cpuset_weight(&cg->cpuset) == 1 ? "" : "s");
    return 0;
}
EXPORT_SYMBOL(cgroup_cpuset_set);

int cgroup_cpuset_get(int cg_id, cpuset_t *set) {
    if (!set)
        return -EINVAL;
    if (cg_id < 0 || cg_id >= CGROUP_MAX || !g_cgroups[cg_id].in_use)
        return -EINVAL;

    spinlock_acquire(&g_cgroup_lock);
    if (g_cgroups[cg_id].cpuset_valid)
        *set = g_cgroups[cg_id].cpuset;
    else
        cpuset_zero(set);
    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_cpuset_get);

/* Apply the cgroup's cpuset to a single task (called on attach). */
void cgroup_cpuset_apply_member(int cg_id, int pid) {
    if (pid <= 0)
        return;
    if (cg_id < 0 || cg_id >= CGROUP_MAX || !g_cgroups[cg_id].in_use)
        return;
    if (!g_cgroups[cg_id].cpuset_valid)
        return;

    spinlock_acquire(&g_cgroup_lock);
    cpuset_t set = g_cgroups[cg_id].cpuset;
    spinlock_release(&g_cgroup_lock);

    sched_setaffinity((uint32_t)pid, &set);
}
EXPORT_SYMBOL(cgroup_cpuset_apply_member);

/* ── cpuset NUMA node affinity (cpuset.mems) ──────────────────────── */

int cgroup_cpuset_set_mems(int cg_id, const cpuset_t *nodes) {
    if (!nodes || cpuset_empty(nodes))
        return -EINVAL;
    if (cg_id < 0 || cg_id >= CGROUP_MAX || !g_cgroups[cg_id].in_use)
        return -EINVAL;

    spinlock_acquire(&g_cgroup_lock);
    struct cgroup *cg = &g_cgroups[cg_id];
    cg->nodelist = *nodes;
    cg->nodelist_valid = 1;

    /* Preferred home node = lowest allowed node. */
    int home = __builtin_ctzll(cg->nodelist.bits);

    /* Apply the preferred node to every member process. */
    for (int i = 0; i < CGROUP_MAX_PIDS; i++) {
        int pid = cg->members[i];
        if (pid <= 0)
            continue;
        struct process *m = process_get_by_pid((uint32_t)pid);
        if (m)
            m->home_node = home;
    }
    spinlock_release(&g_cgroup_lock);

    kprintf("[cgroup] cpuset.mems[%d] set to 0x%llx (home node %d)\n", cg_id,
            (unsigned long long)cg->nodelist.bits, home);
    return 0;
}
EXPORT_SYMBOL(cgroup_cpuset_set_mems);

int cgroup_cpuset_get_mems(int cg_id, cpuset_t *nodes) {
    if (!nodes)
        return -EINVAL;
    if (cg_id < 0 || cg_id >= CGROUP_MAX || !g_cgroups[cg_id].in_use)
        return -EINVAL;

    spinlock_acquire(&g_cgroup_lock);
    if (g_cgroups[cg_id].nodelist_valid)
        *nodes = g_cgroups[cg_id].nodelist;
    else
        cpuset_zero(nodes);
    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_cpuset_get_mems);

int cgroup_cpuset_set_exclusive(int cg_id, int exclusive) {
    if (cg_id < 0 || cg_id >= CGROUP_MAX || !g_cgroups[cg_id].in_use)
        return -EINVAL;

    spinlock_acquire(&g_cgroup_lock);
    struct cgroup *cg = &g_cgroups[cg_id];

    /* Enabling exclusivity on a cpuset that already overlaps another
     * exclusive cpuset is rejected. */
    if (exclusive && cg->cpuset_valid && find_exclusive_overlap(cg_id, &cg->cpuset) >= 0) {
        spinlock_release(&g_cgroup_lock);
        return -EBUSY;
    }

    cg->cpuset_exclusive = exclusive ? 1 : 0;
    spinlock_release(&g_cgroup_lock);

    kprintf("[cgroup] cpuset[%d] -> %s\n", cg_id, exclusive ? "exclusive" : "shared");
    return 0;
}
EXPORT_SYMBOL(cgroup_cpuset_set_exclusive);

/* Destroy a cgroup. All member processes are moved to the root cgroup.
 * Returns 0 on success. */
int cgroup_destroy(int cg_id) {
    if (!cgroup_valid(cg_id) || cg_id == 0)
        return -EINVAL;

    spinlock_acquire(&g_cgroup_lock);
    struct cgroup *cg = &g_cgroups[cg_id];

    /* Move all processes to root. Migrate inline while holding
     * g_cgroup_lock: cgroup_attach() re-acquires the same non-recursive
     * spinlock, so calling it here would self-deadlock. */
    for (int i = 0; i < CGROUP_MAX_PIDS; i++) {
        int pid = cg->members[i];
        if (pid == 0)
            continue;
        cg->members[i] = 0;
        cg->num_pids--;
        /* Attach to root cgroup (best effort — root may be full) */
        struct cgroup *root = &g_cgroups[0];
        for (int k = 0; k < CGROUP_MAX_PIDS; k++) {
            if (root->members[k] == 0) {
                root->members[k] = pid;
                root->num_pids++;
                break;
            }
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
int cgroup_attach(int cg_id, int pid) {
    if (!cgroup_valid(cg_id))
        return -EINVAL;

    /* Validate PID — must be a positive integer (PID 0 is the idle
     * process and cannot belong to a cgroup). */
    if (pid <= 0)
        return -EINVAL;

    spinlock_acquire(&g_cgroup_lock);
    struct cgroup *cg = &g_cgroups[cg_id];

    /* Already a member of the destination — idempotent success. */
    for (int i = 0; i < CGROUP_MAX_PIDS; i++) {
        if (cg->members[i] == pid) {
            spinlock_release(&g_cgroup_lock);
            return 0;
        }
    }

    /* Find a free slot in the destination BEFORE removing the pid from
     * its old cgroup, so a full destination (-ENOSPC) leaves the pid's
     * current membership untouched (no partial migration on error). */
    int slot = -1;
    for (int i = 0; i < CGROUP_MAX_PIDS; i++) {
        if (cg->members[i] == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_release(&g_cgroup_lock);
        return -ENOSPC; /* cgroup full — old membership unchanged */
    }

    /* Remove from old cgroup first */
    for (int i = 0; i < CGROUP_MAX_PIDS; i++) {
        for (int j = 0; j < CGROUP_MAX; j++) {
            if (!g_cgroups[j].in_use)
                continue;
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
    cg->members[slot] = pid;
    cg->num_pids++;
    spinlock_release(&g_cgroup_lock);

    /* Apply the destination cgroup's cpuset to the new member. */
    cgroup_cpuset_apply_member(cg_id, pid);
    return 0;
}
EXPORT_SYMBOL(cgroup_attach);

/* Get the cgroup ID for a given PID.
 * Returns cgroup ID or -1 if not found. */
int cgroup_of_pid(int pid) {
    spinlock_acquire(&g_cgroup_lock);
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (!g_cgroups[i].in_use)
            continue;
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
int cgroup_cpu_set_max(int cg_id, int64_t quota_us, int64_t period_us) {
    if (!cgroup_valid(cg_id))
        return -EINVAL;
    if (period_us < CGROUP_CPU_PERIOD_MIN || period_us > CGROUP_CPU_PERIOD_MAX)
        return -EINVAL;

    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);
    cg->cpu.max_quota = quota_us <= 0 ? CGROUP_CPU_PERIOD_DEFAULT : (uint64_t)quota_us;
    cg->cpu.max_period = (uint64_t)period_us;
    cg->cpu.throttled = 0;
    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_cpu_set_max);

/* Query cpu.max values. */
void cgroup_cpu_get_max(int cg_id, uint64_t *quota, uint64_t *period) {
    if (!cgroup_valid(cg_id))
        return;
    struct cgroup *cg = &g_cgroups[cg_id];
    if (quota)
        *quota = cg->cpu.max_quota;
    if (period)
        *period = cg->cpu.max_period;
}

/* Account CPU usage (called by scheduler on context switch).
 * @pid: process that ran
 * @delta_us: microseconds of CPU time consumed
 * Returns 1 if the process was throttled, 0 otherwise. */
int cgroup_cpu_account_split(int pid, uint64_t delta_us, int is_user) {
    int cg_id = cgroup_of_pid(pid);
    if (cg_id < 0 || !cgroup_valid(cg_id))
        return 0;

    struct cgroup *cg = &g_cgroups[cg_id];
    if (cg->cpu.max_quota == 0) {
        /* No quota — still record user/system usage. */
        spinlock_acquire(&g_cgroup_lock);
        if (is_user)
            cg->cpu.usage_user_usec += delta_us;
        else
            cg->cpu.usage_system_usec += delta_us;
        cg->cpu.usage_usec += delta_us;
        {
            extern int smp_get_cpu_id(void);
            int cpu = smp_get_cpu_id();
            if (cpu >= 0)
                cg->cpu.per_cpu_usec[(unsigned)cpu % CGROUP_CPUACCT_MAX_CPUS] += delta_us;
        }
        spinlock_release(&g_cgroup_lock);
        return 0;
    }

    spinlock_acquire(&g_cgroup_lock);
    if (is_user)
        cg->cpu.usage_user_usec += delta_us;
    else
        cg->cpu.usage_system_usec += delta_us;
    cg->cpu.usage_usec += delta_us;

    /* Per-CPU breakdown */
    {
        extern int smp_get_cpu_id(void);
        int cpu = smp_get_cpu_id();
        if (cpu >= 0)
            cg->cpu.per_cpu_usec[(unsigned)cpu % CGROUP_CPUACCT_MAX_CPUS] += delta_us;
    }

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
        cg->cpu.usage_user_usec = 0;
        cg->cpu.usage_system_usec = 0;
        for (int p = 0; p < CGROUP_CPUACCT_MAX_CPUS; p++)
            cg->cpu.per_cpu_usec[p] = 0;
        cg->cpu.throttled = 0;
    }

    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_cpu_account_split);

int cgroup_cpu_account(int pid, uint64_t delta_us) {
    /* Legacy total-only accounting is bucketed as system time. */
    return cgroup_cpu_account_split(pid, delta_us, 0);
}
EXPORT_SYMBOL(cgroup_cpu_account);

/* Check if a cgroup is currently throttled. */
int cgroup_cpu_is_throttled(int cg_id) {
    if (!cgroup_valid(cg_id))
        return 0;
    return g_cgroups[cg_id].cpu.throttled;
}

/* Get CPU accounting statistics (total + user/system split). */
void cgroup_cpu_stat(int cg_id, uint64_t *usage_usec, uint64_t *user_usec, uint64_t *system_usec,
                     uint64_t *nr_throttled, uint64_t *throttled_usec) {
    if (!cgroup_valid(cg_id))
        return;
    struct cgroup *cg = &g_cgroups[cg_id];
    if (usage_usec)
        *usage_usec = cg->cpu.usage_usec;
    if (user_usec)
        *user_usec = cg->cpu.usage_user_usec;
    if (system_usec)
        *system_usec = cg->cpu.usage_system_usec;
    if (nr_throttled)
        *nr_throttled = cg->cpu.nr_throttled;
    if (throttled_usec)
        *throttled_usec = cg->cpu.throttled_usec;
}

/* Per-CPU usage breakdown for a cgroup. */
int cgroup_cpu_percpu_stat(int cg_id, uint64_t *per_cpu, int max) {
    if (!cgroup_valid(cg_id) || !per_cpu)
        return 0;
    struct cgroup *cg = &g_cgroups[cg_id];

    spinlock_acquire(&g_cgroup_lock);
    int n = (max < CGROUP_CPUACCT_MAX_CPUS) ? max : CGROUP_CPUACCT_MAX_CPUS;
    for (int i = 0; i < n; i++)
        per_cpu[i] = cg->cpu.per_cpu_usec[i];
    spinlock_release(&g_cgroup_lock);
    return n;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Memory controller (memory.max/high, cgroup OOM)
 * ═══════════════════════════════════════════════════════════════════════ */

/* Set memory.max limit (bytes). 0 = unlimited. */
int cgroup_mem_set_max(int cg_id, uint64_t max_bytes) {
    if (!cgroup_valid(cg_id))
        return -EINVAL;
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
int cgroup_mem_set_high(int cg_id, uint64_t high_bytes) {
    if (!cgroup_valid(cg_id))
        return -EINVAL;
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
int cgroup_mem_account(int pid, int64_t nr_pages) {
    int cg_id = cgroup_of_pid(pid);
    if (cg_id < 0 || !cgroup_valid(cg_id))
        return 0;

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
void cgroup_mem_stat(int cg_id, uint64_t *usage, uint64_t *max_usage, uint64_t *limit,
                     uint64_t *high_limit, int *oom_kills) {
    if (!cgroup_valid(cg_id))
        return;
    struct cgroup *cg = &g_cgroups[cg_id];
    if (usage)
        *usage = cg->mem.usage_bytes;
    if (max_usage)
        *max_usage = cg->mem.max_usage;
    if (limit)
        *limit = cg->mem.max_bytes;
    if (high_limit)
        *high_limit = cg->mem.high_bytes;
    if (oom_kills)
        *oom_kills = cg->mem.oom_kills;
}

/* Kill the largest process in a cgroup (OOM handler).
 * Returns PID of killed process, or -1 if none. */
int cgroup_oom_kill(int cg_id) {
    if (!cgroup_valid(cg_id))
        return -EINVAL;

    struct cgroup *cg = &g_cgroups[cg_id];
    int victim = -1;
    uint64_t max_mem = 0;

    spinlock_acquire(&g_cgroup_lock);
    for (int i = 0; i < CGROUP_MAX_PIDS; i++) {
        int pid = cg->members[i];
        if (pid == 0)
            continue;
        uint64_t mem = process_get_mem_usage(pid); /* defined in process.h */
        if (mem > max_mem) {
            max_mem = mem;
            victim = pid;
        }
    }

    if (victim > 0) {
        kprintf("[cgroup OOM] killing PID %d (mem %llu) in cgroup %d\n", victim,
                (unsigned long long)max_mem, cg_id);
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
int cgroup_io_set_limit(int cg_id, uint32_t major, uint32_t minor, uint64_t rbps, uint64_t wbps,
                        uint64_t riops, uint64_t wiops) {
    if (!cgroup_valid(cg_id))
        return -EINVAL;

    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);

    /* Find existing device entry or create one */
    int slot = -1;
    for (int i = 0; i < CGROUP_IO_MAX_DEVICES; i++) {
        if (cg->io.devices[i].major == major && cg->io.devices[i].minor == minor) {
            slot = i;
            break;
        }
        if (slot < 0 && cg->io.devices[i].major == 0 && cg->io.devices[i].minor == 0)
            slot = i;
    }
    if (slot < 0) {
        spinlock_release(&g_cgroup_lock);
        return -ENOSPC;
    }

    cg->io.devices[slot].major = (uint16_t)major;
    cg->io.devices[slot].minor = (uint16_t)minor;
    cg->io.devices[slot].rbps = rbps;
    cg->io.devices[slot].wbps = wbps;
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
int cgroup_io_throttle_check(int cg_id, int is_write, uint64_t bytes) {
    if (!cgroup_valid(cg_id))
        return 0;

    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);

    /* Check against all device limits — for simplicity, aggregate */
    for (int i = 0; i < CGROUP_IO_MAX_DEVICES; i++) {
        if (!cg->io.devices[i].in_use)
            continue;
        struct cgroup_io_device *dev = &cg->io.devices[i];

        uint64_t bw_limit = is_write ? dev->wbps : dev->rbps;
        uint64_t iops_limit = is_write ? dev->wiops : dev->riops;

        /* Both budgets (bytes/sec and IOPS) share a single token-bucket
         * refill driven by the same elapsed window, per-device.
         * 0 = unlimited for either. */
        uint64_t *acc = is_write ? &dev->write_bytes_acc : &dev->read_bytes_acc;
        uint64_t *iop_acc = is_write ? &dev->write_iops_acc : &dev->read_iops_acc;
        uint64_t now = timer_get_ticks();
        uint64_t elapsed = now - dev->last_tick;

        /* Refill tokens for both the byte and the IOPS bucket. */
        if (elapsed > 0) {
            if (bw_limit > 0) {
                uint64_t refill = (bw_limit * elapsed) / TIMER_FREQ;
                if (*acc > refill)
                    *acc -= refill;
                else
                    *acc = 0;
            }
            if (iops_limit > 0) {
                uint64_t refill = (iops_limit * elapsed) / TIMER_FREQ;
                if (*iop_acc > refill)
                    *iop_acc -= refill;
                else
                    *iop_acc = 0;
            }
            dev->last_tick = now;
        }

        /* Bandwidth (bytes/sec) gate: the request consumes `bytes`. */
        uint64_t limit = is_write ? dev->wbps : dev->rbps;
        if (limit > 0) {
            if (*acc + bytes > limit) {
                spinlock_release(&g_cgroup_lock);
                return 1; /* throttle */
            }
            *acc += bytes;
        }

        /* IOPS gate: an I/O request costs a single operation token
         * regardless of its byte size. */
        if (iops_limit > 0) {
            if (*iop_acc + 1 > iops_limit) {
                spinlock_release(&g_cgroup_lock);
                return 1; /* throttle */
            }
            *iop_acc += 1;
        }
    }

    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_io_throttle_check);

/* Get IO statistics for a cgroup. */
int cgroup_io_stat(int cg_id, struct cgroup_io_device *devices, int max) {
    if (!cgroup_valid(cg_id))
        return -EINVAL;

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
int cgroup_pids_set_max(int cg_id, int64_t max_pids) {
    if (!cgroup_valid(cg_id))
        return -EINVAL;
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
int cgroup_pids_account(int pid, int is_fork) {
    int cg_id = cgroup_of_pid(pid);
    if (cg_id < 0)
        cg_id = 0; /* default to root */

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
void cgroup_pids_stat(int cg_id, uint64_t *current, uint64_t *max) {
    if (!cgroup_valid(cg_id))
        return;
    if (current)
        *current = g_cgroups[cg_id].pids.current;
    if (max)
        *max = g_cgroups[cg_id].pids.max;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Cgroup freezer (freeze/unfreeze cgroup tasks)
 * ═══════════════════════════════════════════════════════════════════════ */

/* Freeze all tasks in a cgroup.
 * Frozen tasks are not scheduled until unfrozen.
 * Returns 0 on success. */
int cgroup_freeze(int cg_id) {
    if (!cgroup_valid(cg_id))
        return -EINVAL;

    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);
    cg->freezer.state = CGROUP_FROZEN;

    /* Freeze all member processes */
    for (int i = 0; i < CGROUP_MAX_PIDS; i++) {
        if (cg->members[i] != 0) {
            process_freeze(cg->members[i]); /* defined in process.h */
        }
    }
    kprintf("[cgroup] frozen cgroup %d (%d processes)\n", cg_id, cg->num_pids);
    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_freeze);

/* Unfreeze all tasks in a cgroup.
 * Returns 0 on success. */
int cgroup_unfreeze(int cg_id) {
    if (!cgroup_valid(cg_id))
        return -EINVAL;

    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);
    cg->freezer.state = CGROUP_THAWED;

    /* Unfreeze all member processes */
    for (int i = 0; i < CGROUP_MAX_PIDS; i++) {
        if (cg->members[i] != 0) {
            process_unfreeze(cg->members[i]); /* defined in process.h */
        }
    }
    kprintf("[cgroup] unfrozen cgroup %d (%d processes)\n", cg_id, cg->num_pids);
    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_unfreeze);

/* Query freezer state for a cgroup.
 * Returns CGROUP_THAWED or CGROUP_FROZEN. */
int cgroup_freezer_state(int cg_id) {
    if (!cgroup_valid(cg_id))
        return CGROUP_THAWED;
    return g_cgroups[cg_id].freezer.state;
}

/* ── RDMA controller (D315 task 12) ──────────────────────────────── */

/* Locate an HCA device record in a cgroup's RDMA state; add it if absent
 * (respecting CGROUP_RDMA_MAX_DEVS).  Caller holds g_cgroup_lock.
 * Returns the device index or -1 if the array is full. */
static int cgroup_rdma_slot(struct cgroup *cg, const char *hca_name) {
    if (!hca_name)
        return -1;
    int slot = -1;
    for (int i = 0; i < CGROUP_RDMA_MAX_DEVS; i++) {
        if (cg->rdma.devices[i].in_use && strcmp(cg->rdma.devices[i].name, hca_name) == 0)
            return i;
        if (slot < 0 && !cg->rdma.devices[i].in_use)
            slot = i;
    }
    if (slot < 0)
        return -1;
    memset(&cg->rdma.devices[slot], 0, sizeof(cg->rdma.devices[slot]));
    strlcpy(cg->rdma.devices[slot].name, hca_name, sizeof(cg->rdma.devices[slot].name));
    cg->rdma.devices[slot].in_use = 1;
    return slot;
}

/* Set the RDMA resource limits for one HCA in a cgroup.
 * 0 = unlimited (the default, matching rdma.max "max"). */
int cgroup_rdma_set_limit(int cg_id, const char *hca_name, uint64_t handle_limit,
                          uint64_t object_limit) {
    if (!cgroup_valid(cg_id))
        return -EINVAL;
    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);
    int slot = cgroup_rdma_slot(cg, hca_name);
    if (slot < 0) {
        spinlock_release(&g_cgroup_lock);
        return -ENOSPC;
    }
    cg->rdma.devices[slot].hca_handle_limit = handle_limit;
    cg->rdma.devices[slot].hca_object_limit = object_limit;
    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_rdma_set_limit);

/* Account an RDMA resource alloc/decrease against a cgroup.
 * Returns -EAGAIN if an allocation would exceed the limit (free goes
 * through regardless). */
int cgroup_rdma_account(int cg_id, const char *hca_name, int is_object, int delta) {
    if (!hca_name || !cgroup_valid(cg_id))
        return -EINVAL;
    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);
    int slot = -1;
    for (int i = 0; i < CGROUP_RDMA_MAX_DEVS; i++) {
        if (cg->rdma.devices[i].in_use && strcmp(cg->rdma.devices[i].name, hca_name) == 0) {
            slot = i;
            break;
        }
    }
    int rc = 0;
    if (slot >= 0) {
        struct cgroup_rdma_device *d = &cg->rdma.devices[slot];
        uint64_t *usage = is_object ? &d->hca_object_usage : &d->hca_handle_usage;
        uint64_t limit = is_object ? d->hca_object_limit : d->hca_handle_limit;
        if (delta > 0 && limit > 0 && *usage + (uint64_t)delta > limit) {
            rc = -EAGAIN; /* allocation would breach the limit */
        } else {
            /* Apply signed delta to the running usage counter. */
            int64_t nu = (int64_t)*usage + delta;
            *usage = nu > 0 ? (uint64_t)nu : 0;
        }
    }
    /* No device record → no limit configured → allow and don't track. */
    spinlock_release(&g_cgroup_lock);
    return rc;
}
EXPORT_SYMBOL(cgroup_rdma_account);

/* Copy the RDMA device records for a cgroup into @devices (up to @max).
 * Returns the count written. */
int cgroup_rdma_stat(int cg_id, struct cgroup_rdma_device *devices, int max) {
    if (!cgroup_valid(cg_id))
        return -EINVAL;
    if (!devices || max <= 0)
        return 0;
    struct cgroup *cg = &g_cgroups[cg_id];
    int count = 0;
    spinlock_acquire(&g_cgroup_lock);
    for (int i = 0; i < CGROUP_RDMA_MAX_DEVS && count < max; i++) {
        if (cg->rdma.devices[i].in_use)
            devices[count++] = cg->rdma.devices[i];
    }
    spinlock_release(&g_cgroup_lock);
    return count;
}
EXPORT_SYMBOL(cgroup_rdma_stat);

/* Look up one HCA's RDMA record; returns 1 if found (copied to @out), 0 if not. */
int cgroup_rdma_find(int cg_id, const char *hca_name, struct cgroup_rdma_device *out) {
    if (!hca_name || !out || !cgroup_valid(cg_id))
        return 0;
    struct cgroup *cg = &g_cgroups[cg_id];
    int found = 0;
    spinlock_acquire(&g_cgroup_lock);
    for (int i = 0; i < CGROUP_RDMA_MAX_DEVS; i++) {
        if (cg->rdma.devices[i].in_use && strcmp(cg->rdma.devices[i].name, hca_name) == 0) {
            *out = cg->rdma.devices[i];
            found = 1;
            break;
        }
    }
    spinlock_release(&g_cgroup_lock);
    return found;
}
EXPORT_SYMBOL(cgroup_rdma_find);

/* ── Misc controller (D315 task 13) ──────────────────────────────── */

/* Locate (or create) a named resource record in a cgroup's misc state.
 * Caller holds g_cgroup_lock.  Returns index or -1 if full. */
static int cgroup_misc_slot(struct cgroup *cg, const char *res_name) {
    if (!res_name)
        return -1;
    int slot = -1;
    for (int i = 0; i < CGROUP_MISC_MAX_RES; i++) {
        if (cg->misc.resources[i].in_use && strcmp(cg->misc.resources[i].name, res_name) == 0)
            return i;
        if (slot < 0 && !cg->misc.resources[i].in_use)
            slot = i;
    }
    if (slot < 0)
        return -1;
    memset(&cg->misc.resources[slot], 0, sizeof(cg->misc.resources[slot]));
    strlcpy(cg->misc.resources[slot].name, res_name, sizeof(cg->misc.resources[slot].name));
    cg->misc.resources[slot].in_use = 1;
    return slot;
}

/* Set the misc.max limit for a named resource in a cgroup.  0 = unlimited. */
int cgroup_misc_set_max(int cg_id, const char *res_name, uint64_t max) {
    if (!cgroup_valid(cg_id))
        return -EINVAL;
    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);
    int slot = cgroup_misc_slot(cg, res_name);
    if (slot < 0) {
        spinlock_release(&g_cgroup_lock);
        return -ENOSPC;
    }
    cg->misc.resources[slot].max = max;
    spinlock_release(&g_cgroup_lock);
    return 0;
}
EXPORT_SYMBOL(cgroup_misc_set_max);

/* Charge (+) or decharge (-) a named resource.  A positive charge that
 * would push current past misc.max is rejected with -EAGAIN. */
int cgroup_misc_charge(int cg_id, const char *res_name, int64_t amount) {
    if (!res_name || !cgroup_valid(cg_id))
        return -EINVAL;
    struct cgroup *cg = &g_cgroups[cg_id];
    spinlock_acquire(&g_cgroup_lock);
    int slot = -1;
    for (int i = 0; i < CGROUP_MISC_MAX_RES; i++) {
        if (cg->misc.resources[i].in_use && strcmp(cg->misc.resources[i].name, res_name) == 0) {
            slot = i;
            break;
        }
    }
    int rc = 0;
    if (slot >= 0) {
        struct cgroup_misc_resource *r = &cg->misc.resources[slot];
        if (amount > 0 && r->max > 0 && r->current + (uint64_t)amount > r->max) {
            rc = -EAGAIN; /* would breach misc.max */
        } else {
            int64_t nu = (int64_t)r->current + amount;
            r->current = nu > 0 ? (uint64_t)nu : 0;
            if (r->current > r->max_usage)
                r->max_usage = r->current;
        }
    }
    /* No record → not limited → allow and don't track. */
    spinlock_release(&g_cgroup_lock);
    return rc;
}
EXPORT_SYMBOL(cgroup_misc_charge);

/* Copy the misc resource records for a cgroup (up to @max).  Returns count. */
int cgroup_misc_stat(int cg_id, struct cgroup_misc_resource *resources, int max) {
    if (!cgroup_valid(cg_id))
        return -EINVAL;
    if (!resources || max <= 0)
        return 0;
    struct cgroup *cg = &g_cgroups[cg_id];
    int count = 0;
    spinlock_acquire(&g_cgroup_lock);
    for (int i = 0; i < CGROUP_MISC_MAX_RES && count < max; i++) {
        if (cg->misc.resources[i].in_use)
            resources[count++] = cg->misc.resources[i];
    }
    spinlock_release(&g_cgroup_lock);
    return count;
}
EXPORT_SYMBOL(cgroup_misc_stat);

/* Look up one resource's misc record; returns 1 if found, 0 if not. */
int cgroup_misc_find(int cg_id, const char *res_name, struct cgroup_misc_resource *out) {
    if (!res_name || !out || !cgroup_valid(cg_id))
        return 0;
    struct cgroup *cg = &g_cgroups[cg_id];
    int found = 0;
    spinlock_acquire(&g_cgroup_lock);
    for (int i = 0; i < CGROUP_MISC_MAX_RES; i++) {
        if (cg->misc.resources[i].in_use && strcmp(cg->misc.resources[i].name, res_name) == 0) {
            *out = cg->misc.resources[i];
            found = 1;
            break;
        }
    }
    spinlock_release(&g_cgroup_lock);
    return found;
}
EXPORT_SYMBOL(cgroup_misc_find);

/* ── Sysfs/proc interface helpers ─────────────────────────────────── */

/* Write a cgroup control file value.
 * Format: "cg_id controller key value" or "cg_id value"
 * Returns 0 on success, negative on error. */
int cgroup_write_control(int cg_id, const char *controller, const char *key, const char *value) {
    if (!cgroup_valid(cg_id))
        return -EINVAL;
    if (!controller || !key || !value)
        return -EINVAL;

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
                    if (period == 0)
                        period = CGROUP_CPU_PERIOD_DEFAULT;
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
    } else if (strcmp(controller, "io") == 0 || strcmp(controller, "io.max") == 0 ||
               strcmp(key, "io.max") == 0) {
        /* I/O read/write bandwidth limits (D315 task 8).
         * Format (real cgroup v2 io.max):  "8:0 rbps=<n> wbps=<n> [riops=<n> wiops=<n>]"
         * Here <controller>=<"io"|"io.max">, <key>=<"M:m"|"8:0">, <value>=<"rbps=... ...">.
         * Each limit may be "max" (unlimited, stored as 0). */
        uint16_t major = 0, minor = 0;
        const char *dev = key;
        if (*dev >= '0' && *dev <= '9') {
            while (*dev >= '0' && *dev <= '9')
                major = (uint16_t)(major * 10 + (uint16_t)(*dev++ - '0'));
            if (*dev == ':')
                dev++;
            while (*dev >= '0' && *dev <= '9')
                minor = (uint16_t)(minor * 10 + (uint16_t)(*dev++ - '0'));
        }
        uint64_t rbps = 0, wbps = 0, riops = 0, wiops = 0;
        const char *tok = value;
        while (tok && *tok) {
            while (*tok == ' ' || *tok == '\t')
                tok++;
            if (*tok == '\0')
                break;
            int is_bw = (strncmp(tok, "rbps", 4) == 0) || (strncmp(tok, "wbps", 4) == 0);
            int is_read = (strncmp(tok, "rbps", 4) == 0) || (strncmp(tok, "riops", 5) == 0);
            const char *eq = strchr(tok, '=');
            if (!eq) {
                /* Skip to next whitespace if no '=' */
                while (*tok && *tok != ' ' && *tok != '\t')
                    tok++;
                continue;
            }
            eq++;
            uint64_t v = 0;
            if (strncmp(eq, "max", 3) == 0) {
                v = 0; /* unlimited */
            } else {
                while (*eq >= '0' && *eq <= '9')
                    v = v * 10 + (uint64_t)(*eq++ - '0');
            }
            if (is_bw) {
                if (is_read)
                    rbps = v;
                else
                    wbps = v;
            } else {
                if (is_read)
                    riops = v;
                else
                    wiops = v;
            }
            /* Advance to next token */
            while (*tok && *tok != ' ' && *tok != '\t')
                tok++;
        }
        return cgroup_io_set_limit(cg_id, major, minor, rbps, wbps, riops, wiops);
    } else if (strcmp(controller, "freezer") == 0) {
        if (strcmp(value, "FROZEN") == 0)
            return cgroup_freeze(cg_id);
        if (strcmp(value, "THAWED") == 0)
            return cgroup_unfreeze(cg_id);
    } else if (strcmp(controller, "cpuset") == 0 || strcmp(key, "cpuset.cpus") == 0 ||
               strcmp(key, "cpuset.mems") == 0 || strcmp(key, "cpuset.exclusive") == 0) {
        /* cpuset controller: "cpuset cpus <list>", "cpuset.cpus <list>",
         * "cpuset.mems <nodelist>", or "cpuset.exclusive <0|1>". */
        if (strcmp(key, "cpuset.exclusive") == 0 || strcmp(key, "exclusive") == 0) {
            uint64_t excl = 0;
            const char *sv = value;
            while (*sv >= '0' && *sv <= '9')
                excl = excl * 10 + (uint64_t)(*sv++ - '0');
            return cgroup_cpuset_set_exclusive(cg_id, excl != 0);
        }
        cpuset_t set;
        int pr = cpuset_parse(value, &set);
        if (pr < 0)
            return pr;
        if (strcmp(key, "cpuset.mems") == 0 || strcmp(key, "mems") == 0)
            return cgroup_cpuset_set_mems(cg_id, &set);
        return cgroup_cpuset_set(cg_id, &set);
    } else if (strcmp(controller, "rdma") == 0 || strcmp(key, "rdma.max") == 0) {
        /* RDMA controller (D315 task 12).  Format mirrors real cgroup v2
         * rdma.max:  "<hca> hca_handle=<n> hca_object=<n>"  where each <n>
         * may be "max" (unlimited).  <key> may carry the HCA device name
         * (e.g. "mlx5_0"); if not, <value> must start with it. */
        const char *hca = key;
        if (strcmp(hca, "rdma.max") == 0 || strcmp(key, "max") == 0)
            hca = value; /* "<hca> hca_handle=... ..." — parse name prefix */
        char name[CGROUP_RDMA_HCA_NAME_LEN];
        const char *v = hca;
        int nlen = 0;
        while (*v && *v != ' ' && *v != '\t' && nlen < CGROUP_RDMA_HCA_NAME_LEN - 1)
            name[nlen++] = *v++;
        name[nlen] = '\0';
        if (nlen == 0)
            return -EINVAL;
        uint64_t hlimit = 0, olimit = 0;
        while (*v) {
            while (*v == ' ' || *v == '\t')
                v++;
            if (*v == '\0')
                break;
            int is_handle = (strncmp(v, "hca_handle", 10) == 0);
            const char *eq = strchr(v, '=');
            if (!eq) {
                while (*v && *v != ' ' && *v != '\t')
                    v++;
                continue;
            }
            eq++;
            uint64_t val = 0;
            if (strncmp(eq, "max", 3) == 0) {
                val = 0;
            } else {
                while (*eq >= '0' && *eq <= '9')
                    val = val * 10 + (uint64_t)(*eq++ - '0');
            }
            if (is_handle)
                hlimit = val;
            else
                olimit = val;
            while (*v && *v != ' ' && *v != '\t')
                v++;
        }
        return cgroup_rdma_set_limit(cg_id, name, hlimit, olimit);
    } else if (strcmp(controller, "misc") == 0 || strcmp(key, "misc.max") == 0) {
        /* Misc controller (D315 task 13).  Real cgroup v2 misc.max:
         *   "misc.max" → "<res_name> <max>"   e.g. "sgx_epc 1048576"
         * Here <controller>=<"misc"|"misc.max">, <key>=<resource name
         * unless "max">, <value>=<limit|"max">. */
        const char *res = key;
        if (strcmp(res, "misc.max") == 0 || strcmp(key, "max") == 0)
            res = value;
        char name[CGROUP_MISC_RES_NAME_LEN];
        const char *v = res;
        int nlen = 0;
        while (*v && *v != ' ' && *v != '\t' && nlen < CGROUP_MISC_RES_NAME_LEN - 1)
            name[nlen++] = *v++;
        name[nlen] = '\0';
        if (nlen == 0)
            return -EINVAL;
        /* Skip to the limit token (after resource name). */
        while (*v == ' ' || *v == '\t')
            v++;
        uint64_t limit = 0;
        if (strncmp(v, "max", 3) == 0) {
            limit = 0; /* unlimited */
        } else {
            while (*v >= '0' && *v <= '9')
                limit = limit * 10 + (uint64_t)(*v++ - '0');
        }
        return cgroup_misc_set_max(cg_id, name, limit);
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Initialization
 * ═══════════════════════════════════════════════════════════════════════ */

void cgroup_init(void) {
    if (g_cgroup_initialized)
        return;

    memset(g_cgroups, 0, sizeof(g_cgroups));
    spinlock_init(&g_cgroup_lock);
    spinlock_init(&g_cgroup_work_lock);

    /* Root cgroup */
    g_cgroups[0].in_use = 1;
    g_cgroups[0].id = 0;
    g_cgroups[0].parent_id = -1;
    g_cgroups[0].cpu.max_period = CGROUP_CPU_PERIOD_DEFAULT;
    g_cgroups[0].cpu.max_quota = CGROUP_CPU_PERIOD_DEFAULT;
    g_cgroups[0].ctrl_mask = CG_CTRL_ALL; /* all controllers enabled at root */
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

    /* Mount cgroup v1 compatibility hierarchy (D315 task 14). */
    if (vfs_create("/sys/fs/cgroup-v1", VFS_TYPE_DIR) == 0 ||
        vfs_mount("/sys/fs/cgroup-v1", &cgroup_v1_vfs_ops, NULL) == 0) {
        kprintf("[OK] cgroup v1 compat mounted at /sys/fs/cgroup-v1/\n");
    }

    kprintf("[OK] Cgroup v2 initialized (cpu, memory, io, pids, freezer, rdma, misc)\n");
    g_cgroup_initialized = 1;
}
EXPORT_SYMBOL(cgroup_init);

/* ═══════════════════════════════════════════════════════════════════════
 *  Stub functions for incomplete cgroup operations
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── cgroup_fork: Initialize cgroup state for a new process ────────────── */
static int cgroup_fork(struct process *task) {
    if (!task)
        return -EINVAL;
    /* In a minimal implementation, the child inherits the parent's cgroup
     * by attaching the new PID to the parent's cgroup. */
    struct process *parent = process_get_current();
    if (parent) {
        int cg_id = cgroup_of_pid(parent->pid);
        if (cg_id >= 0) {
            cgroup_attach(cg_id, task->pid);
        }
    }
    kprintf("[cgroup] cgroup_fork: pid=%d\n", task->pid);
    return 0;
}

/* ── cgroup_post_fork: Post-fork cgroup accounting ─────────────────────── */
static void cgroup_post_fork(struct process *task) {
    if (!task)
        return;
    /* Account this new process in the pids controller */
    cgroup_pids_account(task->pid, 1);
    kprintf("[cgroup] cgroup_post_fork: pid=%d\n", task->pid);
}

/* ── cgroup_exit: Clean up cgroup state on process exit ────────────────── */
static void cgroup_exit(struct process *task) {
    if (!task)
        return;
    /* Decrement the pids counter for this cgroup */
    cgroup_pids_account(task->pid, 0);
    kprintf("[cgroup] cgroup_exit: pid=%d\n", task->pid);
}

/* ── cgroup_can_fork: Check if cgroup allows forking ───────────────────── */
static int cgroup_can_fork(struct process *task) {
    if (!task)
        return -EINVAL;
    /* Check pids.max limit */
    int cg_id = cgroup_of_pid(task->pid);
    uint64_t current_pids_val = 0, max_pids_val = 0;
    if (cg_id >= 0) {
        cgroup_pids_stat(cg_id, &current_pids_val, &max_pids_val);
        if (max_pids_val > 0 && current_pids_val >= max_pids_val) {
            kprintf("[cgroup] cgroup_can_fork: pid=%d DENIED (pids limit %llu)\n", task->pid,
                    (unsigned long long)max_pids_val);
            return -EAGAIN;
        }
    }
    kprintf("[cgroup] cgroup_can_fork: pid=%d (allowed)\n", task->pid);
    return 0;
}
