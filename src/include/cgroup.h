#ifndef CGROUP_H
#define CGROUP_H

#include "types.h"
#include "process.h"

/* ── Cgroup v2 constants ─────────────────────────────────────────── */

#define CGROUP_MAX_PIDS 16   /* max processes per cgroup */
#define CGROUP_MAX_NAME 64

/* Freezer states */
#define CGROUP_THAWED   0
#define CGROUP_FROZEN   1

/* ── IO device bandwidth/IOPS limits ─────────────────────────────── */

struct cgroup_io_device {
    uint16_t major;
    uint16_t minor;
    uint64_t rbps;      /* read bytes per second */
    uint64_t wbps;      /* write bytes per second */
    uint64_t riops;     /* read IOPS */
    uint64_t wiops;     /* write IOPS */
    uint64_t read_bytes_acc;   /* accumulated bytes for token bucket */
    uint64_t write_bytes_acc;
    uint64_t last_tick;
    int      in_use;
};

/* ── Per-controller state ────────────────────────────────────────── */

/* CPU controller state */
struct cgroup_cpu_state {
    uint64_t max_quota;        /* cpu.max quota in µs */
    uint64_t max_period;       /* cpu.max period in µs */
    uint64_t usage_usec;       /* CPU time used in µs */
    int      throttled;        /* 1 if currently throttled */
    uint64_t nr_throttled;     /* total throttling events */
    uint64_t throttled_usec;   /* total time throttled in µs */
};

/* Memory controller state */
struct cgroup_mem_state {
    uint64_t max_bytes;        /* memory.max hard limit (0 = unlimited) */
    uint64_t high_bytes;       /* memory.high soft limit (0 = unlimited) */
    uint64_t usage_bytes;      /* current memory usage */
    uint64_t max_usage;        /* historical max usage */
    uint64_t high_crossings;   /* number of times high limit crossed */
    int      oom_triggered;    /* 1 if OOM is pending */
    int      oom_kills;        /* total OOM kills in this cgroup */
    uint64_t swap_bytes;       /* swap usage (future) */
};

/* IO controller state */
#define CGROUP_IO_MAX_DEVICES 8
struct cgroup_io_state {
    struct cgroup_io_device devices[CGROUP_IO_MAX_DEVICES];
};

/* PID controller state */
struct cgroup_pids_state {
    uint64_t current;          /* current number of tasks */
    uint64_t max;              /* pids.max (0 = unlimited) */
};

/* Freezer state */
struct cgroup_freezer_state {
    int state;                 /* CGROUP_THAWED or CGROUP_FROZEN */
};

/* ── Cgroup structure ────────────────────────────────────────────── */

struct cgroup {
    int     in_use;
    int     id;
    int     parent_id;             /* -1 for root */
    char    name[CGROUP_MAX_NAME];

    /* Child cgroups (flat array for simplicity) */
    int     children[8];
    int     num_children;

    /* Member processes */
    int     members[CGROUP_MAX_PIDS];
    int     num_pids;

    /* Controller states */
    struct cgroup_cpu_state      cpu;
    struct cgroup_mem_state      mem;
    struct cgroup_io_state       io;
    struct cgroup_pids_state     pids;
    struct cgroup_freezer_state  freezer;
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
int  cgroup_cpu_set_max(int cg_id, int64_t quota_us, int64_t period_us);
void cgroup_cpu_get_max(int cg_id, uint64_t *quota, uint64_t *period);
int  cgroup_cpu_account(int pid, uint64_t delta_us);
int  cgroup_cpu_is_throttled(int cg_id);
void cgroup_cpu_stat(int cg_id, uint64_t *usage_usec,
                     uint64_t *nr_throttled, uint64_t *throttled_usec);

/* Memory controller */
int  cgroup_mem_set_max(int cg_id, uint64_t max_bytes);
int  cgroup_mem_set_high(int cg_id, uint64_t high_bytes);
int  cgroup_mem_account(int pid, int64_t nr_pages);
void cgroup_mem_stat(int cg_id, uint64_t *usage, uint64_t *max_usage,
                     uint64_t *limit, uint64_t *high_limit,
                     int *oom_kills);
int  cgroup_oom_kill(int cg_id);

/* IO controller */
int  cgroup_io_set_limit(int cg_id, uint32_t major, uint32_t minor,
                         uint64_t rbps, uint64_t wbps,
                         uint64_t riops, uint64_t wiops);
int  cgroup_io_throttle_check(int cg_id, int is_write, uint64_t bytes);
int  cgroup_io_stat(int cg_id, struct cgroup_io_device *devices, int max);

/* PID controller */
int  cgroup_pids_set_max(int cg_id, int64_t max_pids);
int  cgroup_pids_account(int pid, int is_fork);
void cgroup_pids_stat(int cg_id, uint64_t *current, uint64_t *max);

/* Freezer */
int cgroup_freeze(int cg_id);
int cgroup_unfreeze(int cg_id);
int cgroup_freezer_state(int cg_id);

/* Control file interface */
int cgroup_write_control(int cg_id, const char *controller,
                         const char *key, const char *value);

#endif /* CGROUP_H */
