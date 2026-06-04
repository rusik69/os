/*
 * container.h — OCI container runtime data structures and API
 *
 * Defines the container descriptor, state machine, and runtime-spec
 * directory layout for the in-kernel OCI container runtime.
 *
 * Directory layout:
 *   /var/lib/containers/<id>/      — persistent container data (rootfs, config)
 *       config.json                  — OCI runtime-spec configuration
 *       rootfs/                      — container root filesystem
 *       log/                         — stdout/stderr log files
 *   /run/containers/<id>/           — runtime state (ephemeral)
 *       state.json                   — current container state
 *
 * Container lifecycle states (OCI runtime-spec):
 *   CREATING → CREATED → RUNNING → STOPPED → DELETED
 *                            ↓
 *                       PAUSED
 */

#ifndef CONTAINER_H
#define CONTAINER_H

#include "types.h"
#include "vfs.h"
#include "spinlock.h"

/* ── Constants ──────────────────────────────────────────────────────── */

#define CONTAINER_ID_MAX       64      /* Max length of container ID string */
#define CONTAINER_NAME_MAX     128     /* Max length of container name      */
#define CONTAINER_STATE_PATH   256     /* Max length of a state file path   */
#define CONTAINER_MAX          64      /* Max simultaneously active containers */
#define CONTAINER_ROOTFS_PATH  256     /* Max length of rootfs path        */

/* Container state machine values (mirrors OCI runtime-spec) */
#define CONTAINER_STATE_CREATING   0
#define CONTAINER_STATE_CREATED    1
#define CONTAINER_STATE_RUNNING    2
#define CONTAINER_STATE_STOPPED    3
#define CONTAINER_STATE_DELETING   4
#define CONTAINER_STATE_PAUSED     5
#define CONTAINER_STATE_MAX        6

/* Human-readable names for each container state */
static const char *const container_state_names[CONTAINER_STATE_MAX] = {
    [CONTAINER_STATE_CREATING] = "creating",
    [CONTAINER_STATE_CREATED]  = "created",
    [CONTAINER_STATE_RUNNING]  = "running",
    [CONTAINER_STATE_STOPPED]  = "stopped",
    [CONTAINER_STATE_DELETING] = "deleting",
    [CONTAINER_STATE_PAUSED]   = "paused",
};

/* ── Container descriptor ──────────────────────────────────────────── */

struct container {
    char  id[CONTAINER_ID_MAX];          /* Unique container ID (hex/sha256 prefix) */
    char  name[CONTAINER_NAME_MAX];      /* Human-friendly name (optional)          */
    int   state;                          /* Current state (CONTAINER_STATE_*)       */

    /* Process information */
    uint32_t init_pid;                    /* PID of the container's init process    */
    uint32_t creator_pid;                 /* PID that created this container        */

    /* Filesystem paths */
    char data_dir[CONTAINER_STATE_PATH];  /* /var/lib/containers/<id>/              */
    char run_dir[CONTAINER_STATE_PATH];   /* /run/containers/<id>/                  */
    char rootfs_path[CONTAINER_ROOTFS_PATH]; /* <data_dir>/rootfs/                  */

    /* Resource limits (cgroup integration) */
    uint64_t memory_limit;                /* Memory limit in bytes (0 = unlimited)  */
    uint64_t cpu_shares;                  /* CPU shares (relative weight)           */
    uint64_t cpu_quota_us;                /* CPU quota in microseconds per period   */
    uint64_t cpu_period_us;               /* CPU period in microseconds             */
    uint32_t pids_limit;                  /* Max number of PIDs (0 = unlimited)     */

    /* Namespace flags (OR of CLONE_NEW*) */
    uint64_t ns_flags;

    /* Container capabilities */
    uint32_t cap_effective;               /* Effective capability mask               */
    uint32_t cap_bounding;                /* Bounding capability mask                */
    uint32_t cap_permitted;               /* Permitted capability mask               */

    /* Reference count for concurrent access */
    int refcount;

    /* Synchronisation */
    spinlock_t lock;

    /* Container is in use (slot allocated) */
    int in_use;
};

/* ── Container state file helpers ──────────────────────────────────── */

/* Path templates */
#define CONTAINER_DATA_DIR    "/var/lib/containers"
#define CONTAINER_RUN_DIR     "/run/containers"
#define CONTAINER_CONFIG_JSON "config.json"
#define CONTAINER_STATE_JSON  "state.json"
#define CONTAINER_ROOTFS      "rootfs"

/* ── Public API ────────────────────────────────────────────────────── */

/**
 * container_init() — Initialise the container subsystem.
 * Creates /var/lib/containers/ and /run/containers/ directories.
 * Returns 0 on success, negative errno on failure.
 */
int container_init(void);

/**
 * container_alloc() — Allocate a container descriptor slot.
 * Returns a pointer to the container struct, or NULL if the table is full.
 * The returned container has refcount = 1 and state = CREATING.
 */
struct container *container_alloc(void);

/**
 * container_free() — Release a container descriptor slot.
 * The container must be in STOPPED state.
 * Frees any allocated resources and marks the slot as unused.
 */
void container_free(struct container *c);

/**
 * container_set_id() — Assign a unique ID to a container.
 * Generates a hex string from a hash of (creator PID + timestamp + counter).
 * Returns 0 on success, negative on error (e.g., duplicate ID).
 */
int container_set_id(struct container *c);

/**
 * container_create_dirs() — Create the data and run directories for a container.
 * Creates:
 *   <data_dir>/      (e.g., /var/lib/containers/<id>/)
 *   <data_dir>/rootfs/
 *   <data_dir>/log/
 *   <run_dir>/       (e.g., /run/containers/<id>/)
 *
 * Returns 0 on success, negative errno on failure.
 */
int container_create_dirs(struct container *c);

/**
 * container_remove_dirs() — Remove the data and run directories for a container.
 * Called during container cleanup (state → DELETED).
 * Returns 0 on success, negative errno on failure.
 */
int container_remove_dirs(struct container *c);

/**
 * container_set_state() — Atomically transition a container's state.
 * Validates the state transition against the OCI state machine.
 * Returns 0 on success, -EINVAL if the transition is invalid.
 */
int container_set_state(struct container *c, int new_state);

/**
 * container_state_name() — Return human-readable state name.
 */
static inline const char *container_state_name(int state) {
    if (state >= 0 && state < CONTAINER_STATE_MAX)
        return container_state_names[state];
    return "unknown";
}

#endif /* CONTAINER_H */
