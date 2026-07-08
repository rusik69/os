/*
 * runtime.c — OCI container runtime: directory structure, lifecycle,
 *             and container descriptor management.
 *
 * This is the foundational layer for the in-kernel container runtime.
 * It manages:
 *   1. The global container table (up to CONTAINER_MAX active containers)
 *   2. The OCI-spec directory layout under /var/lib/containers/ and /run/containers/
 *   3. Container state transitions (creating → created → running → stopped → deleted)
 *   4. Unique container ID generation
 *
 * Implements: C1 (OCI runtime directory structure)
 */

#define KERNEL_INTERNAL
#include "container.h"
#include "fs.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "process.h"
#include "errno.h"
#include "vfs.h"
#include "scheduler.h"   /* scheduler_yield for process lifecycles */
#include "heap.h"         /* kmalloc / kfree */
#include "signal.h"       /* signal_send for stopping init process */
#include "timer.h"        /* timer_get_ticks for timeout */
#include "oci_spec.h"     /* OCI config parsing for init path */
#include "elf.h"          /* process_spawn for init process creation */

/* ── Global container table ────────────────────────────────────────── */

/* VFS operations for virtual filesystems mounted inside container rootfs.
 * These are defined in their respective source files. */
extern struct vfs_ops procfs_ops;
extern struct vfs_ops sysfs_vfs_ops;
extern struct vfs_ops devfs_ops;
extern struct vfs_ops tmpfs_vfs_ops;

struct container container_table[CONTAINER_MAX];

/* Lock protecting the container table */
static spinlock_t container_global_lock = SPINLOCK_INIT;

/* Counter for unique ID generation */
static uint64_t container_counter = 0;

/* ── Helpers ───────────────────────────────────────────────────────── */

/* Convert a byte to two hex characters in buf (buf must have space for 2 chars) */
static void byte_to_hex(uint8_t b, char *out) {
    static const char hex_chars[] = "0123456789abcdef";
    out[0] = hex_chars[(b >> 4) & 0x0F];
    out[1] = hex_chars[b & 0x0F];
}

/* Simple hash of (pid + counter + tick) into a 16-char hex string.
 * Uses a basic djb2-like hash to produce a pseudo-unique container ID. */
static void generate_container_id(char *out, int max_len) {
    uint64_t pid  = process_get_current() ? (uint64_t)(process_get_current()->pid) : 0;
    uint64_t tick = timer_get_ticks();
    uint64_t ctr  = __sync_fetch_and_add(&container_counter, 1);
    uint64_t mix  = pid ^ (tick << 13) ^ (ctr * 6364136223846793005ULL);

    /* Produce 16 hex characters from the 64-bit mix */
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) {
        bytes[i] = (uint8_t)(mix & 0xFF);
        mix >>= 8;
    }
    if (max_len < 17) return; /* need at least 16 chars + NUL */
    for (int i = 0; i < 8; i++) {
        byte_to_hex(bytes[i], out + (i * 2));
    }
    out[16] = '\0';
}

/* Create a directory at the given path, ignoring EEXIST errors.
 * Returns 0 on success, negative errno on failure. */
static int ensure_dir(const char *path) {
    int ret = fs_create(path, FS_TYPE_DIR);
    if (ret >= 0) return 0;
    if (ret == -EEXIST) return 0;
    kprintf("[Containers] Failed to create directory '%s': err=%d\n", path, ret);
    return ret;
}

/* ── Initialisation ────────────────────────────────────────────────── */

int container_init(void) {
    kprintf("[Containers] Initialising container runtime...\n");

    /* Create the top-level container data and run directories.
     * These persist across reboots (data) and at runtime (run). */
    int ret = ensure_dir(CONTAINER_DATA_DIR);
    if (ret < 0) {
        kprintf("[Containers] FAILED to create %s (err=%d)\n",
                CONTAINER_DATA_DIR, ret);
        return ret;
    }

    ret = ensure_dir(CONTAINER_RUN_DIR);
    if (ret < 0) {
        kprintf("[Containers] FAILED to create %s (err=%d)\n",
                CONTAINER_RUN_DIR, ret);
        return ret;
    }

    /* Zero the container table */
    memset(container_table, 0, sizeof(container_table));

    kprintf("[Containers] OK — runtime ready (%d max containers)\n", CONTAINER_MAX);
    return 0;
}

/* ── Container descriptor allocation / release ─────────────────────── */

struct container *container_alloc(void) {
    struct container *c = NULL;

    spinlock_acquire(&container_global_lock);

    for (int i = 0; i < CONTAINER_MAX; i++) {
        if (!container_table[i].in_use) {
            c = &container_table[i];
            memset(c, 0, sizeof(*c));
            c->in_use = 1;
            c->state  = CONTAINER_STATE_CREATING;
            c->refcount = 1;
            spinlock_init(&c->lock);
            break;
        }
    }

    spinlock_release(&container_global_lock);

    if (!c) {
        kprintf("[Containers] Container table full (%d slots exhausted)\n",
                CONTAINER_MAX);
    }

    return c;
}

void container_free(struct container *c) {
    if (!c || !c->in_use) return;

    spinlock_acquire(&container_global_lock);

    /* Mark the slot as free */
    memset(c, 0, sizeof(*c));

    spinlock_release(&container_global_lock);
}

/* ── ID generation ─────────────────────────────────────────────────── */

int container_set_id(struct container *c) {
    if (!c || !c->in_use) return -EINVAL;

    char new_id[CONTAINER_ID_MAX];
    generate_container_id(new_id, sizeof(new_id));

    /* Check for duplicate ID (unlikely with 128-bit space, but be thorough) */
    spinlock_acquire(&container_global_lock);
    for (int i = 0; i < CONTAINER_MAX; i++) {
        if (&container_table[i] != c &&
            container_table[i].in_use &&
            strcmp(container_table[i].id, new_id) == 0) {
            spinlock_release(&container_global_lock);
            return -EEXIST; /* Extremely unlikely collision; caller may retry */
        }
    }
    spinlock_release(&container_global_lock);

    strncpy(c->id, new_id, sizeof(c->id) - 1);
    c->id[sizeof(c->id) - 1] = '\0';
    return 0;
}

/* ── Directory creation / removal ──────────────────────────────────── */

int container_create_dirs(struct container *c) {
    if (!c || !c->in_use || !c->id[0]) return -EINVAL;
    if (c->state != CONTAINER_STATE_CREATING) return -EBUSY;

    /* Build paths */
    int n;

    n = snprintf(c->data_dir, sizeof(c->data_dir), "%s/%s",
                 CONTAINER_DATA_DIR, c->id);
    if (n < 0 || (size_t)n >= sizeof(c->data_dir)) return -ENAMETOOLONG;

    n = snprintf(c->run_dir, sizeof(c->run_dir), "%s/%s",
                 CONTAINER_RUN_DIR, c->id);
    if (n < 0 || (size_t)n >= sizeof(c->run_dir)) return -ENAMETOOLONG;

    n = snprintf(c->rootfs_path, sizeof(c->rootfs_path), "%s/%s",
                 c->data_dir, CONTAINER_ROOTFS);
    if (n < 0 || (size_t)n >= sizeof(c->rootfs_path)) return -ENAMETOOLONG;

    /* Create data directory hierarchy */
    int ret = ensure_dir(c->data_dir);
    if (ret < 0) return ret;

    /* Create rootfs subdirectory */
    ret = ensure_dir(c->rootfs_path);
    if (ret < 0) return ret;

    /* Create log subdirectory */
    {
        char log_dir[CONTAINER_STATE_PATH];
        n = snprintf(log_dir, sizeof(log_dir), "%s/log", c->data_dir);
        if (n >= 0 && (size_t)n < sizeof(log_dir)) {
            ret = ensure_dir(log_dir);
            if (ret < 0) return ret;
        }
    }

    /* Create run directory (ephemeral runtime state) */
    ret = ensure_dir(c->run_dir);
    if (ret < 0) return ret;

    kprintf("[Containers] Created directories for container %s\n", c->id);
    return 0;
}

int container_remove_dirs(struct container *c) {
    if (!c || !c->in_use || !c->data_dir[0]) return -EINVAL;

    /* Remove run directory (ephemeral) */
    if (c->run_dir[0]) {
        /* Attempt recursive removal — best-effort, log failures */
        int ret = fs_delete(c->run_dir);
        if (ret < 0 && ret != -ENOENT) {
            kprintf("[Containers] Warning: failed to remove %s (err=%d)\n",
                    c->run_dir, ret);
        }
    }

    /* Remove data directory and all contents (rootfs, log, config.json) */
    if (c->data_dir[0]) {
        /* Remove rootfs subdirectory */
        if (c->rootfs_path[0]) {
            int ret = fs_delete(c->rootfs_path);
            if (ret < 0 && ret != -ENOENT) {
                kprintf("[Containers] Warning: failed to remove %s (err=%d)\n",
                        c->rootfs_path, ret);
            }
        }

        /* Remove log subdirectory */
        {
            char log_dir[CONTAINER_STATE_PATH];
            int n = snprintf(log_dir, sizeof(log_dir), "%s/log", c->data_dir);
            if (n >= 0 && (size_t)n < sizeof(log_dir)) {
                int ret = fs_delete(log_dir);
                if (ret < 0 && ret != -ENOENT) {
                    kprintf("[Containers] Warning: failed to remove %s (err=%d)\n",
                            log_dir, ret);
                }
            }
        }

        /* Remove config.json if present */
        {
            char cfg_path[CONTAINER_STATE_PATH];
            int n = snprintf(cfg_path, sizeof(cfg_path), "%s/%s",
                             c->data_dir, CONTAINER_CONFIG_JSON);
            if (n >= 0 && (size_t)n < sizeof(cfg_path)) {
                int ret = fs_delete(cfg_path);
                if (ret < 0 && ret != -ENOENT) {
                    kprintf("[Containers] Warning: failed to remove %s (err=%d)\n",
                            cfg_path, ret);
                }
            }
        }

        /* Remove state.json from run dir if present */
        {
            char state_path[CONTAINER_STATE_PATH];
            int n = snprintf(state_path, sizeof(state_path), "%s/%s",
                             c->run_dir, CONTAINER_STATE_JSON);
            if (n >= 0 && (size_t)n < sizeof(state_path)) {
                int ret = fs_delete(state_path);
                if (ret < 0 && ret != -ENOENT) {
                    kprintf("[Containers] Warning: failed to remove %s (err=%d)\n",
                            state_path, ret);
                }
            }
        }

        /* Finally remove the data directory itself */
        int ret = fs_delete(c->data_dir);
        if (ret < 0 && ret != -ENOENT) {
            kprintf("[Containers] Warning: failed to remove %s (err=%d)\n",
                    c->data_dir, ret);
        }
    }

    kprintf("[Containers] Removed directories for container %s\n", c->id);
    return 0;
}

/* ── State machine ─────────────────────────────────────────────────── */

static int is_valid_transition(int old_state, int new_state) {
    /* OCI runtime state machine transitions:
     *   CREATING  → CREATED
     *   CREATED   → RUNNING
     *   RUNNING   → STOPPED  | PAUSED
     *   PAUSED    → RUNNING
     *   STOPPED   → DELETED
     *   DELETING  → (terminal, freed by container_free)
     */
    switch (old_state) {
    case CONTAINER_STATE_CREATING:
        return new_state == CONTAINER_STATE_CREATED;
    case CONTAINER_STATE_CREATED:
        return new_state == CONTAINER_STATE_RUNNING;
    case CONTAINER_STATE_RUNNING:
        return new_state == CONTAINER_STATE_STOPPED ||
               new_state == CONTAINER_STATE_PAUSED;
    case CONTAINER_STATE_PAUSED:
        return new_state == CONTAINER_STATE_RUNNING;  /* unpause */
    case CONTAINER_STATE_STOPPED:
        return new_state == CONTAINER_STATE_DELETING;
    default:
        return 0; /* All other transitions are invalid */
    }
}

int container_set_state(struct container *c, int new_state) {
    if (!c || !c->in_use) return -EINVAL;
    if (new_state < 0 || new_state >= CONTAINER_STATE_MAX) return -EINVAL;

    spinlock_acquire(&c->lock);

    int old_state = c->state;
    if (!is_valid_transition(old_state, new_state)) {
        spinlock_release(&c->lock);
        kprintf("[Containers] Invalid state transition: %s \u2192 %s for %s\n",
                container_state_name(old_state),
                container_state_name(new_state),
                c->id);
        return -EINVAL;
    }

    c->state = new_state;

    /* Persist the state to the JSON file for external tooling.
     * This is best-effort: failures are logged but do not block the
     * state transition, since the in-memory state is authoritative. */
    {
        int persist_ret = container_persist_state(c);
        if (persist_ret < 0) {
            kprintf("[Containers] Warning: failed to persist state.json for %s: %d\n",
                    c->id, persist_ret);
        }
    }

    spinlock_release(&c->lock);

    kprintf("[Containers] Container %s: %s \u2192 %s\n",
            c->id,
            container_state_name(old_state),
            container_state_name(new_state));
    return 0;
}

/* ── Container rootfs subdirectory structure ─────────────────────────
 * Standard Linux container rootfs directories: /proc, /sys, /dev, /etc,
 * /tmp, /var/run.  These are created inside the container rootfs so that
 * virtual filesystems can be mounted at the standard locations.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int container_create_rootfs_dirs(struct container *c)
{
    if (!c || !c->rootfs_path[0]) return -EINVAL;

    static const char *const subdirs[] = {
        "proc", "sys", "dev", "etc", "tmp",
        "var", "var/run", "var/log", "dev/shm",
        "sys/kernel", "sys/kernel/security",
        "proc/sys", "proc/sys/net",
    };
    char path[CONTAINER_ROOTFS_PATH];

    for (size_t i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
        int n = snprintf(path, sizeof(path), "%s/%s",
                         c->rootfs_path, subdirs[i]);
        if (n < 0 || (size_t)n >= sizeof(path))
            return -ENAMETOOLONG;

        int ret = fs_create(path, FS_TYPE_DIR);
        if (ret < 0 && ret != -EEXIST) {
            kprintf("[Containers] Failed to create %s: err=%d\n", path, ret);
            return ret;
        }
    }

    kprintf("[Containers] Created rootfs subdirectories for %s\n", c->id);
    return 0;
}

/* ── Mount virtual filesystems inside container rootfs ────────────────
 * Mounts proc, sysfs, devtmpfs, and tmpfs at standard locations within
 * the container's rootfs directory.  These mounts happen in the current
 * mount namespace.  When a container is forked with CLONE_NEWNS, they
 * become visible only inside the container's mount namespace.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int container_mount_vfs(struct container *c)
{
    if (!c || !c->rootfs_path[0]) return -EINVAL;

    char mount_path[CONTAINER_ROOTFS_PATH];

    /* Mount procfs at /proc */
    {
        int n = snprintf(mount_path, sizeof(mount_path),
                         "%s/proc", c->rootfs_path);
        if (n < 0 || (size_t)n >= sizeof(mount_path))
            return -ENAMETOOLONG;
        int ret = vfs_mount_ex(mount_path, &procfs_ops, NULL, 0);
        if (ret < 0) {
            kprintf("[Containers] Failed to mount procfs at %s: err=%d\n",
                    mount_path, ret);
            return ret;
        }
    }

    /* Mount sysfs at /sys */
    {
        int n = snprintf(mount_path, sizeof(mount_path),
                         "%s/sys", c->rootfs_path);
        if (n < 0 || (size_t)n >= sizeof(mount_path))
            return -ENAMETOOLONG;
        int ret = vfs_mount_ex(mount_path, &sysfs_vfs_ops, NULL, 0);
        if (ret < 0) {
            kprintf("[Containers] Failed to mount sysfs at %s: err=%d\n",
                    mount_path, ret);
            return ret;
        }
    }

    /* Mount devtmpfs at /dev */
    {
        int n = snprintf(mount_path, sizeof(mount_path),
                         "%s/dev", c->rootfs_path);
        if (n < 0 || (size_t)n >= sizeof(mount_path))
            return -ENAMETOOLONG;
        int ret = vfs_mount_ex(mount_path, &devfs_ops, NULL, 0);
        if (ret < 0) {
            kprintf("[Containers] Failed to mount devfs at %s: err=%d\n",
                    mount_path, ret);
            return ret;
        }
    }

    /* Mount tmpfs at /tmp */
    {
        int n = snprintf(mount_path, sizeof(mount_path),
                         "%s/tmp", c->rootfs_path);
        if (n < 0 || (size_t)n >= sizeof(mount_path))
            return -ENAMETOOLONG;
        int ret = vfs_mount_ex(mount_path, &tmpfs_vfs_ops, NULL, 0);
        if (ret < 0) {
            kprintf("[Containers] Failed to mount tmpfs at %s: err=%d\n",
                    mount_path, ret);
            return ret;
        }
    }

    /* Mount tmpfs at /var/run (ephemeral runtime state) */
    {
        int n = snprintf(mount_path, sizeof(mount_path),
                         "%s/var/run", c->rootfs_path);
        if (n < 0 || (size_t)n >= sizeof(mount_path))
            return -ENAMETOOLONG;
        int ret = vfs_mount_ex(mount_path, &tmpfs_vfs_ops, NULL, 0);
        if (ret < 0) {
            kprintf("[Containers] Failed to mount tmpfs at %s: err=%d\n",
                    mount_path, ret);
            return ret;
        }
    }

    kprintf("[Containers] Virtual filesystems mounted for %s\n", c->id);
    return 0;
}

/* ── Container create — rootfs setup (Item C3) ────────────────────────
 *
 * Prepares a container for running its init process.  This implements
 * the OCI "create" lifecycle step:
 *   1. Assign a unique container ID
 *   2. Create container directory hierarchy (data + run + rootfs)
 *   3. Create standard rootfs subdirectories (proc, sys, dev, etc.)
 *   4. Mount virtual filesystems (proc, sysfs, devtmpfs, tmpfs)
 *   5. Write initial state.json for external tooling
 *   6. Transition state from CREATING → CREATED
 *
 * After this function succeeds, the container is ready for the "start"
 * step (Item C4) which forks the init process and executes it inside
 * the prepared environment.
 *
 * Returns 0 on success, negative errno on failure.
 */
int container_create(struct container *c)
{
    if (!c || !c->in_use) return -EINVAL;

    spinlock_acquire(&c->lock);
    if (c->state != CONTAINER_STATE_CREATING) {
        spinlock_release(&c->lock);
        return -EBUSY;
    }

    /* Step 1: Assign a unique container ID */
    int ret = container_set_id(c);
    if (ret < 0) {
        spinlock_release(&c->lock);
        kprintf("[Containers] Failed to assign ID: err=%d\n", ret);
        return ret;
    }

    /* Release the lock during I/O-heavy operations; re-acquire for state change */
    spinlock_release(&c->lock);

    /* Step 2: Create container data + run directories */
    ret = container_create_dirs(c);
    if (ret < 0) {
        kprintf("[Containers] Failed to create directories for %s: err=%d\n",
                c->id, ret);
        return ret;
    }

    /* Step 3: Create standard rootfs subdirectories */
    ret = container_create_rootfs_dirs(c);
    if (ret < 0) {
        kprintf("[Containers] Failed to create rootfs subdirs for %s: err=%d\n",
                c->id, ret);
        return ret;
    }

    /* Step 4: Mount virtual filesystems inside the container rootfs */
    ret = container_mount_vfs(c);
    if (ret < 0) {
        kprintf("[Containers] Failed to mount VFS for %s: err=%d\n",
                c->id, ret);
        return ret;
    }

    /* Step 5: Transition state to CREATED */
    ret = container_set_state(c, CONTAINER_STATE_CREATED);
    if (ret < 0) {
        kprintf("[Containers] State transition failed for %s: err=%d\n",
                c->id, ret);
        return ret;
    }

    kprintf("[Containers] Container %s created successfully (rootfs: %s)\n",
            c->id, c->rootfs_path);
    return 0;
}

/* ── Container start — exec init process (Item C4) ─────────────────────
 *
 * Starts a container's init process by reading the OCI config.json,
 * spawning the init binary inside the prepared rootfs, and tracking
 * the process PID.  This implements the OCI "start" lifecycle step:
 *
 *   1. Validate container is in CREATED state
 *   2. Read and parse OCI config.json from the container data dir
 *   3. Build argv array from the parsed process args
 *   4. Spawn the init process via process_spawn()
 *   5. Record the init PID in the container descriptor
 *   6. Persist updated state (with init PID) to state.json
 *   7. Transition state from CREATED → RUNNING
 *
 * After this function succeeds, the container is in RUNNING state and
 * its init process is executing.  The caller should call container_stop()
 * to stop the container gracefully.
 *
 * Returns 0 on success, negative errno on failure.
 * On failure, the container remains in CREATED state. */
int container_start(struct container *c)
{
    if (!c || !c->in_use) return -EINVAL;

    /* ── Step 1: Validate state ──────────────────────────────────── */
    spinlock_acquire(&c->lock);
    if (c->state != CONTAINER_STATE_CREATED) {
        spinlock_release(&c->lock);
        kprintf("[Containers] Cannot start %s: not in CREATED state (current=%s)\n",
                c->id, container_state_name(c->state));
        return -EBUSY;
    }
    spinlock_release(&c->lock);

    /* ── Step 2: Build config.json path ──────────────────────────── */
    char cfg_path[CONTAINER_STATE_PATH];
    int n = snprintf(cfg_path, sizeof(cfg_path), "%s/%s",
                     c->data_dir, CONTAINER_CONFIG_JSON);
    if (n < 0 || (size_t)n >= sizeof(cfg_path))
        return -ENAMETOOLONG;

    /* ── Step 3: Read and parse config.json ──────────────────────── */
    struct oci_config cfg;
    memset(&cfg, 0, sizeof(cfg));

    int parse_ret = oci_config_read_file(&cfg, cfg_path);
    if (parse_ret < 0) {
        kprintf("[Containers] Failed to parse config.json for %s: %s\n",
                c->id, cfg.err_msg[0] ? cfg.err_msg : "unknown error");
        return -EINVAL;
    }

    /* ── Step 4: Validate we have an init binary path ────────────── */
    if (cfg.process.num_args < 1 || cfg.process.args[0][0] == '\0') {
        kprintf("[Containers] config.json for %s has no process args\n", c->id);
        oci_config_free(&cfg);
        return -EINVAL;
    }

    /* ── Step 5: Build full init path (relative to rootfs) ──────── */
    char init_path[OCI_PATH_LEN];
    n = snprintf(init_path, sizeof(init_path), "%s/%s",
                 c->rootfs_path, cfg.process.args[0]);
    if (n < 0 || (size_t)n >= sizeof(init_path)) {
        oci_config_free(&cfg);
        return -ENAMETOOLONG;
    }

    /* ── Step 6: Prepare argv array ──────────────────────────────── */
    char *argv[256];
    memset(argv, 0, sizeof(argv));
    argv[0] = init_path;

    int max_args = (sizeof(argv) / sizeof(argv[0])) - 1;
    int num_args_safe = cfg.process.num_args < max_args ? cfg.process.num_args : max_args;
    for (int i = 1; i < num_args_safe; i++) {
        argv[i] = cfg.process.args[i];
    }
    argv[num_args_safe] = NULL;

    /* ── Step 7: Prepare envp array ──────────────────────────────── */
    char *envp[OCI_MAX_ENV + 1];
    memset(envp, 0, sizeof(envp));
    int env_count = cfg.process.num_env < OCI_MAX_ENV ? cfg.process.num_env : OCI_MAX_ENV;
    for (int i = 0; i < env_count; i++) {
        envp[i] = cfg.process.env[i];
    }
    envp[env_count] = NULL;

    /* ── Step 8: Spawn the init process ──────────────────────────── */
    kprintf("[Containers] Starting container %s: %s\n", c->id, init_path);

    int spawn_ret = process_spawn(init_path, argv, envp);

    /* Free the parsed config — the string pointers are no longer needed */
    oci_config_free(&cfg);

    if (spawn_ret < 0) {
        kprintf("[Containers] Failed to spawn init process for %s: err=%d\n",
                c->id, spawn_ret);
        return spawn_ret;
    }

    uint32_t init_pid = (uint32_t)spawn_ret;

    /* ── Step 9: Record init PID in container descriptor ─────────── */
    spinlock_acquire(&c->lock);
    c->init_pid = init_pid;
    spinlock_release(&c->lock);

    /* ── Step 10: Persist updated state with init PID ────────────── */
    {
        int persist_ret = container_persist_state(c);
        if (persist_ret < 0) {
            kprintf("[Containers] Warning: failed to persist state after start for %s: %d\n",
                    c->id, persist_ret);
            /* Non-fatal: PID is tracked in-memory */
        }
    }

    /* ── Step 11: Transition state to RUNNING ────────────────────── */
    int ret = container_set_state(c, CONTAINER_STATE_RUNNING);
    if (ret < 0) {
        kprintf("[Containers] State transition to RUNNING failed for %s: %d\n",
                c->id, ret);
        return ret;
    }

    kprintf("[Containers] Container %s started successfully (init PID %u)\n",
            c->id, (unsigned)init_pid);
    return 0;
}

/* ── Container stop (Item C5) ─────────────────────────────────────────
 *
 * Stops a running container by sending SIGTERM to its init process.
 * If the process does not exit within @timeout_ms milliseconds, sends
 * SIGKILL.  After stopping, the container is in STOPPED state and may
 * be deleted or inspected.
 *
 * Returns 0 on success, negative errno on failure.
 */
int container_stop(struct container *c, int timeout_ms)
{
    if (!c || !c->in_use) return -EINVAL;

    /* Must be in RUNNING state to stop */
    spinlock_acquire(&c->lock);
    if (c->state != CONTAINER_STATE_RUNNING) {
        spinlock_release(&c->lock);
        return -EBUSY;
    }
    spinlock_release(&c->lock);

    /* Default timeout: 3 seconds */
    if (timeout_ms <= 0) timeout_ms = 3000;

    /* Convert milliseconds to ticks (TIMER_FREQ = 100 ticks/sec) */
    int timeout_ticks = (timeout_ms * TIMER_FREQ + 999) / 1000;
    if (timeout_ticks < 1) timeout_ticks = 1;

    uint32_t target_pid = c->init_pid;
    kprintf("[Containers] Stopping container %s (PID %u, timeout=%d ms)...\n",
            c->id, (unsigned)target_pid, timeout_ms);

    /* Step 1: Send SIGTERM to init process */
    if (target_pid != 0) {
        int ret = signal_send(target_pid, SIGTERM);
        if (ret < 0) {
            kprintf("[Containers] Warning: signal_send(SIGTERM) to PID %u returned %d\n",
                    (unsigned)target_pid, ret);
            /* Continue anyway — process may already be dead */
        }
    }

    /* Step 2: Wait for process to exit (poll process table) */
    uint64_t start_tick = timer_get_ticks();
    int waited = 0;

    while (waited < timeout_ticks) {
        /* Check if the process still exists */
        struct process *proc = process_get_by_pid(target_pid);
        if (!proc) {
            /* Process has exited */
            kprintf("[Containers] Container %s init process (PID %u) exited gracefully\n",
                    c->id, (unsigned)target_pid);
            break;
        }

        /* Yield to let the target process run and handle the signal */
        scheduler_yield();

        /* Update elapsed time */
        uint64_t now = timer_get_ticks();
        waited = (int)(now - start_tick);
    }

    /* Step 3: If still alive after timeout, send SIGKILL */
    if (target_pid != 0) {
        struct process *proc = process_get_by_pid(target_pid);
        if (proc) {
            kprintf("[Containers] Container %s PID %u did not exit in %d ms — sending SIGKILL\n",
                    c->id, (unsigned)target_pid, timeout_ms);
            signal_send(target_pid, SIGKILL);

            /* Brief wait for SIGKILL to take effect */
            start_tick = timer_get_ticks();
            waited = 0;
            while (waited < TIMER_FREQ * 2) { /* up to 2 more seconds */
                if (!process_get_by_pid(target_pid))
                    break;
                scheduler_yield();
                waited = (int)(timer_get_ticks() - start_tick);
            }
        }
    }

    /* Step 4: Transition state to STOPPED */
    int ret = container_set_state(c, CONTAINER_STATE_STOPPED);
    if (ret < 0) {
        kprintf("[Containers] Warning: state transition to STOPPED failed for %s: %d\n",
                c->id, ret);
        return ret;
    }

    kprintf("[Containers] Container %s stopped successfully\n", c->id);
    return 0;
}

/* ── Container delete (Item C6) ───────────────────────────────────────
 *
 * Deletes a stopped container:
 *   1. Validates the container is in STOPPED state
 *   2. Transitions state to DELETING
 *   3. Removes all container directories (data + run + rootfs)
 *   4. Frees the container descriptor slot
 *
 * After this call, the container struct is invalidated and must not
 * be accessed again.
 *
 * Returns 0 on success, negative errno on failure.
 */
int container_delete(struct container *c)
{
    if (!c || !c->in_use) return -EINVAL;

    /* Must be in STOPPED state to delete */
    spinlock_acquire(&c->lock);
    if (c->state != CONTAINER_STATE_STOPPED) {
        spinlock_release(&c->lock);
        kprintf("[Containers] Cannot delete container %s: not in STOPPED state (current=%s)\n",
                c->id, container_state_name(c->state));
        return -EBUSY;
    }
    spinlock_release(&c->lock);

    kprintf("[Containers] Deleting container %s...\n", c->id);

    /* Step 1: Transition to DELETING state */
    int ret = container_set_state(c, CONTAINER_STATE_DELETING);
    if (ret < 0) {
        kprintf("[Containers] State transition to DELETING failed for %s: %d\n",
                c->id, ret);
        return ret;
    }

    /* Step 2: Remove all container directories (best-effort) */
    ret = container_remove_dirs(c);
    if (ret < 0) {
        /* Non-fatal — directories may already have been cleaned up */
        kprintf("[Containers] Warning: directory removal for %s returned %d\n",
                c->id, ret);
    }

    /* Step 3: Free the container descriptor slot */
    container_free(c);

    kprintf("[Containers] Container %s deleted successfully\n", c->id);
    return 0;
}

/* ── Stub: runtime_create ─────────────────────────────── */
static int runtime_create(const char *name, void *spec)
{
    (void)name;
    (void)spec;
    kprintf("[container] runtime_create: not yet implemented\n");
    return 0;
}
/* ── Stub: runtime_start ─────────────────────────────── */
static int runtime_start(const char *name)
{
    (void)name;
    kprintf("[container] runtime_start: not yet implemented\n");
    return 0;
}
/* ── Stub: runtime_stop ─────────────────────────────── */
static int runtime_stop(const char *name)
{
    (void)name;
    kprintf("[container] runtime_stop: not yet implemented\n");
    return 0;
}
/* ── Stub: runtime_delete ─────────────────────────────── */
static int runtime_delete(const char *name)
{
    (void)name;
    kprintf("[container] runtime_delete: not yet implemented\n");
    return 0;
}
