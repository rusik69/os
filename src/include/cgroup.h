#ifndef CGROUP_H
#define CGROUP_H

#include "cpuset.h"
#include "process.h"
#include "types.h"

/* ── Cgroup v2 constants ─────────────────────────────────────────── */

#define CGROUP_MAX_PIDS 16 /* max processes per cgroup */
#define CGROUP_MAX_NAME 64

/* Freezer states */
#define CGROUP_THAWED 0
#define CGROUP_FROZEN 1

/* ── IO device bandwidth/IOPS limits ─────────────────────────────── */

struct cgroup_io_device {
    uint16_t major;
    uint16_t minor;
    uint64_t rbps;           /* read bytes per second */
    uint64_t wbps;           /* write bytes per second */
    uint64_t riops;          /* read IOPS */
    uint64_t wiops;          /* write IOPS */
    uint64_t read_bytes_acc; /* accumulated bytes for token bucket */
    uint64_t write_bytes_acc;
    uint64_t last_tick;
    int in_use;
};

/* ── Per-controller state ────────────────────────────────────────── */

/* CPU controller state */
#define CGROUP_CPUACCT_MAX_CPUS 16 /* per-cpu account slots */
struct cgroup_cpu_state {
    uint64_t max_quota;                             /* cpu.max quota in µs */
    uint64_t max_period;                            /* cpu.max period in µs */
    uint64_t usage_usec;                            /* CPU time used in µs (total) */
    uint64_t usage_user_usec;                       /* CPU time in user mode µs */
    uint64_t usage_system_usec;                     /* CPU time in kernel mode µs */
    uint64_t per_cpu_usec[CGROUP_CPUACCT_MAX_CPUS]; /* per-CPU usage */
    int throttled;                                  /* 1 if currently throttled */
    uint64_t nr_throttled;                          /* total throttling events */
    uint64_t throttled_usec;                        /* total time throttled in µs */
};

/* Memory controller state */
struct cgroup_mem_state {
    uint64_t max_bytes;      /* memory.max hard limit (0 = unlimited) */
    uint64_t high_bytes;     /* memory.high soft limit (0 = unlimited) */
    uint64_t usage_bytes;    /* current memory usage */
    uint64_t max_usage;      /* historical max usage */
    uint64_t high_crossings; /* number of times high limit crossed */
    int oom_triggered;       /* 1 if OOM is pending */
    int oom_kills;           /* total OOM kills in this cgroup */
    uint64_t swap_bytes;     /* swap usage (future) */
};

/* IO controller state */
#define CGROUP_IO_MAX_DEVICES 8
struct cgroup_io_state {
    struct cgroup_io_device devices[CGROUP_IO_MAX_DEVICES];
};

/* PID controller state */
struct cgroup_pids_state {
    uint64_t current; /* current number of tasks */
    uint64_t max;     /* pids.max (0 = unlimited) */
};

/* Freezer state */
struct cgroup_freezer_state {
    int state; /* CGROUP_THAWED or CGROUP_FROZEN */
};

/* Controller enable/disable mask bits (cgroup.subtree_control). */
#define CG_CTRL_CPU (1U << 0)
#define CG_CTRL_MEMORY (1U << 1)
#define CG_CTRL_IO (1U << 2)
#define CG_CTRL_PIDS (1U << 3)
#define CG_CTRL_FREEZER (1U << 4)
#define CG_CTRL_ALL (CG_CTRL_CPU | CG_CTRL_MEMORY | CG_CTRL_IO | CG_CTRL_PIDS | CG_CTRL_FREEZER)

/* ── Cgroup structure ────────────────────────────────────────────── */

struct cgroup {
    int in_use;
    int id;
    int parent_id; /* -1 for root */
    char name[CGROUP_MAX_NAME];

    /* Child cgroups (flat array for simplicity) */
    int children[8];
    int num_children;

    /* Member processes */
    int members[CGROUP_MAX_PIDS];
    int num_pids;

    /* Controller enable/disable mask (cgroup.subtree_control) */
    uint32_t ctrl_mask;

    /* cpuset controller: CPUs on which tasks in this cgroup may run */
    cpuset_t cpuset;
    int cpuset_valid;     /* 1 = cpuset active for this cgroup */
    int cpuset_exclusive; /* 1 = exclusive cpuset (no overlap with peers) */

    /* cpuset controller: memory nodes allowed for tasks in this cgroup */
    cpuset_t nodelist;
    int nodelist_valid; /* 1 = node affinity active */

    /* Controller states */
    struct cgroup_cpu_state cpu;
    struct cgroup_mem_state mem;
    struct cgroup_io_state io;
    struct cgroup_pids_state pids;
    struct cgroup_freezer_state freezer;
};

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

/* Initialize cgroup subsystem */
void cgroup_init(void);

/* Cgroup tree management */
int cgroup_create(int parent_id);
int cgroup_destroy(int cg_id);
int cgroup_attach(int cg_id, int pid);
int cgroup_of_pid(int pid);

/* CPU controller */
int cgroup_cpu_set_max(int cg_id, int64_t quota_us, int64_t period_us);
void cgroup_cpu_get_max(int cg_id, uint64_t *quota, uint64_t *period);
int cgroup_cpu_account(int pid, uint64_t delta_us);
/* Account CPU time to a cgroup's user/system buckets (@is_user 1=user). */
int cgroup_cpu_account_split(int pid, uint64_t delta_us, int is_user);
int cgroup_cpu_is_throttled(int cg_id);
void cgroup_cpu_stat(int cg_id, uint64_t *usage_usec, uint64_t *user_usec, uint64_t *system_usec,
                     uint64_t *nr_throttled, uint64_t *throttled_usec);
/* Fill @per_cpu (up to @max) with per-CPU usage µs for a cgroup.
 * Returns the number of slots written. */
int cgroup_cpu_percpu_stat(int cg_id, uint64_t *per_cpu, int max);

/* Memory controller */
int cgroup_mem_set_max(int cg_id, uint64_t max_bytes);
int cgroup_mem_set_high(int cg_id, uint64_t high_bytes);
int cgroup_mem_account(int pid, int64_t nr_pages);
void cgroup_mem_stat(int cg_id, uint64_t *usage, uint64_t *max_usage, uint64_t *limit,
                     uint64_t *high_limit, int *oom_kills);
int cgroup_oom_kill(int cg_id);

/* IO controller */
int cgroup_io_set_limit(int cg_id, uint32_t major, uint32_t minor, uint64_t rbps, uint64_t wbps,
                        uint64_t riops, uint64_t wiops);
int cgroup_io_throttle_check(int cg_id, int is_write, uint64_t bytes);
int cgroup_io_stat(int cg_id, struct cgroup_io_device *devices, int max);

/* PID controller */
int cgroup_pids_set_max(int cg_id, int64_t max_pids);
int cgroup_pids_account(int pid, int is_fork);
void cgroup_pids_stat(int cg_id, uint64_t *current, uint64_t *max);

/* Freezer */
int cgroup_freeze(int cg_id);
int cgroup_unfreeze(int cg_id);
int cgroup_freezer_state(int cg_id);

/* Control file interface */
int cgroup_write_control(int cg_id, const char *controller, const char *key, const char *value);

/* Controller enable/disable interface (cgroup.subtree_control).
 * @name is "cpu", "memory", "io", "pids" or "freezer"; @enable 1 = on, 0 = off.
 * Returns the new controller mask, or negative errno. */
int cgroup_set_controller(int cg_id, const char *name, int enable);

/* Return the current controller enable mask for a cgroup. */
uint32_t cgroup_controllers(int cg_id);

/* ── cpuset controller (cpuset.cpus) ────────────────────────────────
 * Each cgroup may restrict its tasks to a subset of CPUs.  Setting the
 * cpuset immediately applies the affinity to every member process. */

/* Set the CPUs allowed for a cgroup and apply to all its members.
 * Returns 0 on success, negative errno on error. */
int cgroup_cpuset_set(int cg_id, const cpuset_t *set);

/* Read the CPUs currently allowed for a cgroup into @set. */
int cgroup_cpuset_get(int cg_id, cpuset_t *set);

/* Apply a cgroup's cpuset to a single (newly attached) task.
 * Used by cgroup_attach(). */
void cgroup_cpuset_apply_member(int cg_id, int pid);

/* Parse a CPU-list string ("0-3,5") into a cpuset.  Returns 0 on
 * success, -EINVAL on malformed input. */
int cpuset_parse(const char *str, cpuset_t *set);

/* Set the memory nodes allowed for a cgroup (NUMA affinity) and apply
 * the preferred node to every member.  Returns 0 on success. */
int cgroup_cpuset_set_mems(int cg_id, const cpuset_t *nodes);

/* Read the memory-node list currently allowed for a cgroup. */
int cgroup_cpuset_get_mems(int cg_id, cpuset_t *nodes);

/* Mark a cpuset exclusive (1) or shared (0).  Exclusive cpusets may not
 * overlap another exclusive cpuset.  Returns 0 or -EBUSY on conflict. */
int cgroup_cpuset_set_exclusive(int cg_id, int exclusive);

#endif /* CGROUP_H */
