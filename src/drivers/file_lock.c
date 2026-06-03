/*
 * file_lock.c — File locking (advisory + mandatory)
 *
 * Implements:
 *   - Advisory record locking (POSIX fcntl-style F_SETLK/F_GETLK)
 *   - Mandatory lock enforcement: before every read/write the VFS calls
 *     file_lock_check_mandatory() to reject conflicting access with -EAGAIN.
 *
 * Lock table: fixed-size array indexed by file path. Each path can hold
 * at most one lock entry (simplified — a real kernel would track per-fd
 * or per-inode locks with a list of lock structs per file).
 *
 * Item 53: mandatory lock violation detection.
 */

#define KERNEL_INTERNAL
#include "file_lock.h"
#include "vfs.h"            /* struct file_lock */
#include "string.h"
#include "printf.h"
#include "process.h"
#include "spinlock.h"
#include "errno.h"
#include "waitqueue.h"
#include "scheduler.h"

/* ── Lock table ───────────────────────────────────────────────────── */

#define FILE_LOCK_MAX 64   /* maximum number of simultaneously locked files */

struct lock_entry {
    char path[128];             /* canonical absolute path */
    struct file_lock flk;       /* lock descriptor (uses struct from vfs.h) */
    struct wait_queue wq;       /* waiters for F_SETLKW blocking */
    int in_use;
};

static struct lock_entry lock_table[FILE_LOCK_MAX];
static spinlock_t lock_spinlock;
static int lock_initialized = 0;

/* ── Initialisation ───────────────────────────────────────────────── */

void file_lock_init(void)
{
    if (lock_initialized)
        return;

    memset(lock_table, 0, sizeof(lock_table));
    spinlock_init(&lock_spinlock);
    for (int i = 0; i < FILE_LOCK_MAX; i++)
        wait_queue_init(&lock_table[i].wq);
    lock_initialized = 1;

    kprintf("[OK] file_lock: advisory + mandatory file locking (%d entries with F_SETLKW wait)\n",
            FILE_LOCK_MAX);
}

/* ── Helpers ──────────────────────────────────────────────────────── */

/*
 * Check whether two lock ranges overlap and are conflicting.
 * Two locks conflict when:
 *   - Their byte ranges overlap, AND
 *   - At least one of them is a write lock (read locks don't conflict
 *     with each other).
 *   - An UNLOCK never conflicts.
 */
static int lock_conflicts(const struct file_lock *a, const struct file_lock *b)
{
    if (a->l_type == F_UNLCK || b->l_type == F_UNLCK)
        return 0;

    /* Compute exclusive-end: l_len == 0 means "to EOF" (INT64_MAX) */
    int64_t a_start = a->l_start;
    int64_t a_end   = (a->l_len == 0) ? INT64_MAX : a->l_start + a->l_len;
    int64_t b_start = b->l_start;
    int64_t b_end   = (b->l_len == 0) ? INT64_MAX : b->l_start + b->l_len;

    /* No range overlap → no conflict */
    if (a_end <= b_start || b_end <= a_start)
        return 0;

    /* Write lock conflicts with any other lock; read locks coexist */
    if (a->l_type == F_WRLCK || b->l_type == F_WRLCK)
        return 1;

    return 0;
}

/*
 * Find an active lock entry by path.
 * Returns a pointer to the entry or NULL.
 */
static struct lock_entry *find_entry(const char *path)
{
    for (int i = 0; i < FILE_LOCK_MAX; i++) {
        if (lock_table[i].in_use && strcmp(lock_table[i].path, path) == 0)
            return &lock_table[i];
    }
    return NULL;
}

/*
 * Allocate a free slot in the lock table.
 * Returns the index or -1 if the table is full.
 */
static int alloc_slot(void)
{
    for (int i = 0; i < FILE_LOCK_MAX; i++) {
        if (!lock_table[i].in_use)
            return i;
    }
    return -1;
}

/* ── Core API ─────────────────────────────────────────────────────── */

int file_lock_set(const char *path, struct file_lock *flk, int wait)
{
    if (!path || !flk || !lock_initialized)
        return -EINVAL;

    /* Validate lock type */
    if (flk->l_type != F_RDLCK && flk->l_type != F_WRLCK && flk->l_type != F_UNLCK)
        return -EINVAL;

    char ap[128];
    if (path[0] == '/') {
        strncpy(ap, path, sizeof(ap) - 1);
        ap[sizeof(ap) - 1] = '\0';
    } else {
        strncpy(ap, path, sizeof(ap) - 1);
        ap[sizeof(ap) - 1] = '\0';
    }

    for (;;) {
        spinlock_acquire(&lock_spinlock);

        struct lock_entry *entry = find_entry(ap);

        /* ── Unlock ────────────────────────────────────────────────── */
        if (flk->l_type == F_UNLCK) {
            if (entry) {
                struct process *cur = process_get_current();
                uint32_t caller_pid = cur ? cur->pid : 0;

                /* Only the lock owner can unlock */
                if ((uint32_t)entry->flk.l_pid == caller_pid) {
                    /* Wake all waiters before clearing the lock */
                    wait_queue_wake_all(&entry->wq);
                    memset(entry, 0, sizeof(*entry));
                    wait_queue_init(&entry->wq);
                }
            }
            spinlock_release(&lock_spinlock);
            return 0;
        }

        /* ── Lock (F_RDLCK or F_WRLCK) ─────────────────────────────── */

        if (entry) {
            /* File already has a lock — check for conflicts */
            if (lock_conflicts(&entry->flk, flk)) {
                struct process *cur = process_get_current();
                uint32_t caller_pid = cur ? cur->pid : 0;

                if ((uint32_t)entry->flk.l_pid == caller_pid) {
                    /* Same process: upgrade/downgrade the lock */
                    memcpy(&entry->flk, flk, sizeof(struct file_lock));
                    entry->flk.l_pid = (int32_t)caller_pid;
                    entry->flk.used   = 1;
                    spinlock_release(&lock_spinlock);
                    return 0;
                }

                /* Different PID: conflict */
                if (wait) {
                    /* F_SETLKW: block until lock is released */
                    spinlock_release(&lock_spinlock);
                    wait_queue_sleep(&entry->wq);
                    continue; /* retry after wake-up */
                }
                spinlock_release(&lock_spinlock);
                return -EAGAIN;
            }

            /* No conflict: update the existing lock */
            memcpy(&entry->flk, flk, sizeof(struct file_lock));
            struct process *cur = process_get_current();
            entry->flk.l_pid = cur ? (int32_t)cur->pid : -1;
            entry->flk.used   = 1;
            /* Wake any waiters in case lock type changed (read->write etc) */
            wait_queue_wake_all(&entry->wq);
            spinlock_release(&lock_spinlock);
            return 0;
        }

        /* ── No existing lock — create a new entry ─────────────────── */
        int slot = alloc_slot();
        if (slot < 0) {
            spinlock_release(&lock_spinlock);
            return -ENOLCK;
        }

        struct lock_entry *new_entry = &lock_table[slot];
        strncpy(new_entry->path, ap, sizeof(new_entry->path) - 1);
        new_entry->path[sizeof(new_entry->path) - 1] = '\0';
        memcpy(&new_entry->flk, flk, sizeof(struct file_lock));
        struct process *cur = process_get_current();
        new_entry->flk.l_pid = cur ? (int32_t)cur->pid : -1;
        new_entry->flk.used   = 1;
        new_entry->in_use = 1;
        /* Initialize the wait queue (was cleared by memset in alloc_slot's reset) */
        wait_queue_init(&new_entry->wq);

        spinlock_release(&lock_spinlock);
        return 0;
    }
}

int file_lock_unlock(const char *path, struct file_lock *flk)
{
    (void)flk; /* compatibility — we ignore the lock descriptor and unlock all */
    struct file_lock unlock_flk;
    memset(&unlock_flk, 0, sizeof(unlock_flk));
    unlock_flk.l_type = F_UNLCK;
    return file_lock_set(path, &unlock_flk, 0);
}

int file_lock_get(const char *path, struct file_lock *flk)
{
    if (!path || !flk || !lock_initialized)
        return -EINVAL;

    char ap[128];
    strncpy(ap, path, sizeof(ap) - 1);
    ap[sizeof(ap) - 1] = '\0';

    spinlock_acquire(&lock_spinlock);

    struct lock_entry *entry = find_entry(ap);
    if (!entry) {
        spinlock_release(&lock_spinlock);
        return -ENOENT;
    }

    memcpy(flk, &entry->flk, sizeof(struct file_lock));
    spinlock_release(&lock_spinlock);
    return 0;
}

/* ── Mandatory lock check (called by VFS before read/write) ────── */

int file_lock_check_mandatory(const char *path, int for_write)
{
    if (!path || !lock_initialized)
        return 0; /* no locks → access allowed */

    char ap[128];
    strncpy(ap, path, sizeof(ap) - 1);
    ap[sizeof(ap) - 1] = '\0';

    spinlock_acquire(&lock_spinlock);

    struct lock_entry *entry = find_entry(ap);
    if (!entry || !entry->flk.mandatory) {
        spinlock_release(&lock_spinlock);
        return 0; /* no mandatory lock → access allowed */
    }

    int ltype = entry->flk.l_type;

    if (for_write) {
        /*
         * Write operation: blocked by ANY mandatory lock.
         * A mandatory read lock prevents writes; a mandatory write
         * lock also prevents writes.
         */
        if (ltype == F_RDLCK || ltype == F_WRLCK) {
            spinlock_release(&lock_spinlock);
            return -EAGAIN;
        }
    } else {
        /*
         * Read operation: blocked only by a mandatory write lock.
         * A mandatory read lock allows concurrent reads.
         */
        if (ltype == F_WRLCK) {
            spinlock_release(&lock_spinlock);
            return -EAGAIN;
        }
    }

    spinlock_release(&lock_spinlock);
    return 0; /* access allowed */
}
