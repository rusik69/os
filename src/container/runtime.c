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

/* ── Global container table ────────────────────────────────────────── */

/* VFS operations for virtual filesystems mounted inside container rootfs.
 * These are defined in their respective source files. */
extern struct vfs_ops procfs_ops;
extern struct vfs_ops sysfs_vfs_ops;
extern struct vfs_ops devfs_ops;
extern struct vfs_ops tmpfs_vfs_ops;

static struct container container_table[CONTAINER_MAX];

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
    if (ret == 0) return 0;  /* fs_create returns 0 on success for directories */
    /* fs_create returns negative on error; if path already exists as dir, that's OK */
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
        kprintf("[Containers] Invalid state transition: %s → %s for %s\n",
                container_state_name(old_state),
                container_state_name(new_state),
                c->id);
        return -EINVAL;
    }

    c->state = new_state;

    spinlock_release(&c->lock);

    kprintf("[Containers] Container %s: %s → %s\n",
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
