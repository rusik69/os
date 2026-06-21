/*
 * inotify.c — inotify_init / inotify_add_watch / inotify_rm_watch (Item 340)
 *
 * Implements the Linux-compatible inotify API on top of the kernel's
 * existing fsnotify notification infrastructure.
 *
 * Each inotify_init() call creates an "instance" backed by a fixed-range
 * file descriptor (720-727).  Users can add watches on paths via
 * inotify_add_watch() and remove them via inotify_rm_watch().
 * Events are read from the fd using standard read() which returns
 * one or more struct inotify_event records.
 */

#define KERNEL_INTERNAL
#include "fsnotify.h"
#include "process.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#include "waitqueue.h"
#include "scheduler.h"
#include "errno.h"
#include "vfs.h"
#include "socket.h"   /* POLLIN */

/* ── Constants ────────────────────────────────────────────────────── */

#define INOTIFY_WATCHES_PER_INST 16   /* max watches per inotify fd */
#define INOTIFY_EVENT_QUEUE_SIZE 64   /* events per instance ring buffer */

/* Convert kernel fsnotify events to inotify mask bits */
#define FS2IN_MASK(fs)  ((uint32_t)(fs))

/* ── Per-instance watch descriptor ────────────────────────────────── */

struct inotify_watch {
    int      in_use;
    int      wd;                       /* watch descriptor (returned to user) */
    char     path[128];                /* watched path */
    uint32_t mask;                     /* inotify event mask */
    int      fsnotify_watch_id;        /* kernel fsnotify watch id (-1 if not registered) */
};

/* ── Per-instance event queue entry ────────────────────────────────── */

struct inotify_event_entry {
    int      wd;
    uint32_t mask;
    uint32_t cookie;
    char     name[64];                 /* file name (empty "" for self events) */
};

/* ── Inotify instance ─────────────────────────────────────────────── */

struct inotify_instance {
    int      in_use;
    int      flags;                    /* IN_NONBLOCK, IN_CLOEXEC */
    int      next_wd;                  /* monotonic wd counter */

    /* Watches owned by this instance */
    struct inotify_watch watches[INOTIFY_WATCHES_PER_INST];

    /* Event ring buffer */
    struct inotify_event_entry events[INOTIFY_EVENT_QUEUE_SIZE];
    int      ev_head;                  /* producer index */
    int      ev_count;                 /* number of valid events */

    spinlock_t lock;
    struct wait_queue wq;              /* readers block here */
};

/* ── Static state ─────────────────────────────────────────────────── */

static struct inotify_instance g_instances[INOTIFY_INSTANCES];
static int g_inotify_initialized = 0;

/* ── Helpers ──────────────────────────────────────────────────────── */

static struct inotify_instance *inst_from_fd(int fd)
{
    int slot = fd - INOTIFY_FD_BASE;
    if (slot < 0 || slot >= INOTIFY_INSTANCES)
        return NULL;
    if (!g_instances[slot].in_use)
        return NULL;
    return &g_instances[slot];
}

static int inst_alloc(void)
{
    for (int i = 0; i < INOTIFY_INSTANCES; i++) {
        if (!g_instances[i].in_use)
            return i;
    }
    return -1;
}

static void inst_free(struct inotify_instance *inst)
{
    /* Remove all fsnotify watches for this instance */
    for (int i = 0; i < INOTIFY_WATCHES_PER_INST; i++) {
        if (inst->watches[i].in_use && inst->watches[i].fsnotify_watch_id >= 0) {
            fsnotify_unwatch(inst->watches[i].fsnotify_watch_id);
        }
    }
    memset(inst, 0, sizeof(*inst));
    wait_queue_init(&inst->wq);
}

/* Find a free watch slot in this instance */
static int watch_alloc(struct inotify_instance *inst)
{
    for (int i = 0; i < INOTIFY_WATCHES_PER_INST; i++) {
        if (!inst->watches[i].in_use)
            return i;
    }
    return -1;
}

/* Find a watch slot by wd */
static int watch_find_by_wd(struct inotify_instance *inst, int wd)
{
    for (int i = 0; i < INOTIFY_WATCHES_PER_INST; i++) {
        if (inst->watches[i].in_use && inst->watches[i].wd == wd)
            return i;
    }
    return -1;
}

/* Find a watch slot by path */
static int watch_find_by_path(struct inotify_instance *inst, const char *path)
{
    for (int i = 0; i < INOTIFY_WATCHES_PER_INST; i++) {
        if (inst->watches[i].in_use && strcmp(inst->watches[i].path, path) == 0)
            return i;
    }
    return -1;
}

/* Push an event onto an instance's ring buffer.  Called with lock held. */
static void inst_push_event(struct inotify_instance *inst,
                            int wd, uint32_t mask, uint32_t cookie,
                            const char *name)
{
    if (inst->ev_count >= INOTIFY_EVENT_QUEUE_SIZE) {
        /* Queue full — drop the oldest event */
        int oldest = (inst->ev_head - inst->ev_count + INOTIFY_EVENT_QUEUE_SIZE)
                     % INOTIFY_EVENT_QUEUE_SIZE;
        inst->ev_count--;
        /* Advance head past the dropped event (conceptually) */
        (void)oldest;
    }

    int idx = (inst->ev_head + inst->ev_count) % INOTIFY_EVENT_QUEUE_SIZE;
    inst->events[idx].wd     = wd;
    inst->events[idx].mask   = mask;
    inst->events[idx].cookie = cookie;
    if (name)
        strncpy(inst->events[idx].name, name, sizeof(inst->events[idx].name) - 1);
    else
        inst->events[idx].name[0] = '\0';
    inst->events[idx].name[sizeof(inst->events[idx].name) - 1] = '\0';
    inst->ev_count++;
}

/* ── Initialization ──────────────────────────────────────────────── */

void inotify_init_subsystem(void)
{
    if (g_inotify_initialized)
        return;

    memset(g_instances, 0, sizeof(g_instances));
    for (int i = 0; i < INOTIFY_INSTANCES; i++) {
        spinlock_init(&g_instances[i].lock);
        wait_queue_init(&g_instances[i].wq);
    }

    g_inotify_initialized = 1;
    kprintf("[OK] inotify: initialized (%d instances, %d watches each)\n",
            INOTIFY_INSTANCES, INOTIFY_WATCHES_PER_INST);
}

/* ── inotify_init1 ────────────────────────────────────────────────── */

int sys_inotify_init1(int flags)
{
    if (!g_inotify_initialized)
        inotify_init_subsystem();

    /* Validate flags — only IN_NONBLOCK and IN_CLOEXEC are accepted */
    if (flags & ~(IN_NONBLOCK | IN_CLOEXEC))
        return -EINVAL;

    int slot = inst_alloc();
    if (slot < 0)
        return -EMFILE;  /* too many open inotify instances */

    struct inotify_instance *inst = &g_instances[slot];
    inst->in_use    = 1;
    inst->flags     = flags;
    inst->next_wd   = 1;   /* wd values start at 1 */
    inst->ev_head   = 0;
    inst->ev_count  = 0;
    /* watches and events are zero-initialised by inst_alloc path */
    wait_queue_init(&inst->wq);

    return INOTIFY_FD_BASE + slot;
}

/* ── inotify_add_watch ────────────────────────────────────────────── */

int sys_inotify_add_watch(int fd, const char *user_path, uint32_t mask)
{
    if (!g_inotify_initialized)
        inotify_init_subsystem();

    struct inotify_instance *inst = inst_from_fd(fd);
    if (!inst)
        return -EBADF;

    /* Validate mask: at least one known bit must be set */
    uint32_t known = IN_ACCESS | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE |
                     IN_CLOSE_NOWR | IN_OPEN | IN_MOVED_FROM | IN_MOVED_TO |
                     IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF;
    if (!(mask & known))
        return -EINVAL;

    /* Copy path from user space (direct memory access — kernel can read user pages) */
    char path[128];
    const char *u_path = (const char *)user_path;
    size_t path_len = strnlen(u_path, sizeof(path) - 1);
    memcpy(path, u_path, path_len);
    path[path_len] = '\0';
    if (path_len == 0)
        return -ENOENT;

    /* Resolve to absolute path via VFS */
    char abspath[128];
    if (path[0] != '/') {
        /* Relative path — prepend CWD */
        struct process *cur = process_get_current();
        if (!cur || !cur->cwd[0])
            return -ENOENT;
        int n = snprintf(abspath, sizeof(abspath), "%s/%s", cur->cwd, path);
        if (n < 0 || n >= (int)sizeof(abspath))
            return -ENAMETOOLONG;
    } else {
        strncpy(abspath, path, sizeof(abspath) - 1);
        abspath[sizeof(abspath) - 1] = '\0';
    }

    /* Check if path exists */
    /* (We rely on fsnotify_watch to verify the path) */

    spinlock_acquire(&inst->lock);

    /* Check if we already have a watch on this path — if so, update mask */
    int existing = watch_find_by_path(inst, abspath);
    if (existing >= 0) {
        inst->watches[existing].mask = mask;
        int wd = inst->watches[existing].wd;
        spinlock_release(&inst->lock);
        return wd;
    }

    /* Allocate a new watch slot */
    int slot = watch_alloc(inst);
    if (slot < 0) {
        spinlock_release(&inst->lock);
        return -ENOSPC;
    }

    int wd = inst->next_wd++;

    /* Register with kernel fsnotify — map inotify bits to fs bits */
    uint32_t fs_mask = 0;
    if (mask & (IN_CREATE | IN_MOVED_TO))  fs_mask |= FS_CREATE;
    if (mask & (IN_DELETE | IN_MOVED_FROM)) fs_mask |= FS_DELETE;
    if (mask & IN_MODIFY)                   fs_mask |= FS_MODIFY;
    if (mask & IN_ACCESS)                   fs_mask |= FS_ACCESS;

    int fsn_wid = -1;
    if (fs_mask) {
        fsn_wid = fsnotify_watch(abspath, fs_mask);
        /* fsnotify_watch may return -1 if path doesn't exist or table full */
    }

    /* Fill in the watch entry */
    struct inotify_watch *w = &inst->watches[slot];
    w->in_use            = 1;
    w->wd                = wd;
    w->mask              = mask;
    w->fsnotify_watch_id = fsn_wid;
    strncpy(w->path, abspath, sizeof(w->path) - 1);
    w->path[sizeof(w->path) - 1] = '\0';

    spinlock_release(&inst->lock);
    return wd;
}

/* ── inotify_rm_watch ─────────────────────────────────────────────── */

int sys_inotify_rm_watch(int fd, int wd)
{
    if (!g_inotify_initialized)
        inotify_init_subsystem();

    struct inotify_instance *inst = inst_from_fd(fd);
    if (!inst)
        return -EBADF;

    spinlock_acquire(&inst->lock);

    int slot = watch_find_by_wd(inst, wd);
    if (slot < 0) {
        spinlock_release(&inst->lock);
        return -EINVAL;
    }

    struct inotify_watch *w = &inst->watches[slot];
    if (w->fsnotify_watch_id >= 0)
        fsnotify_unwatch(w->fsnotify_watch_id);
    memset(w, 0, sizeof(*w));

    spinlock_release(&inst->lock);
    return 0;
}

/* ── inotify_read — called from sys_read dispatch ────────────────── */

int inotify_read(int fd, void *buf, size_t count)
{
    struct inotify_instance *inst = inst_from_fd(fd);
    if (!inst)
        return -EBADF;

    /* Minimum read is one event */
    size_t event_size = sizeof(struct inotify_event);
    if (count < event_size)
        return -EINVAL;

    /* How many events fit in the user buffer? */
    int max_events = (int)((count - event_size) / (event_size + 0)) + 1;
    if (max_events > INOTIFY_EVENT_QUEUE_SIZE)
        max_events = INOTIFY_EVENT_QUEUE_SIZE;

    /* Block or return EAGAIN */
    int nonblock = (inst->flags & IN_NONBLOCK);

    for (;;) {
        spinlock_acquire(&inst->lock);

        if (inst->ev_count > 0)
            break;  /* events available */

        spinlock_release(&inst->lock);

        if (nonblock)
            return -EAGAIN;

        /* Wait for events */
        int ret = wait_queue_sleep(&inst->wq);
        if (ret < 0)
            return ret;  /* signal interrupted */

        /* Loop and check again */
    }

    /* Copy events to user buffer — lock still held */
    int written = 0;
    uint8_t *dst = (uint8_t *)buf;

    while (inst->ev_count > 0 && written < max_events) {
        /* Peek the oldest event */
        int oldest = (inst->ev_head - inst->ev_count + INOTIFY_EVENT_QUEUE_SIZE)
                     % INOTIFY_EVENT_QUEUE_SIZE;
        struct inotify_event_entry *src = &inst->events[oldest];

        /* Compute name length */
        uint32_t name_len = (uint32_t)strnlen(src->name, sizeof(src->name));
        if (name_len > 0) name_len++;  /* include NUL terminator */
        /* Align to 4 bytes */
        uint32_t total_len = (uint32_t)(sizeof(struct inotify_event) + name_len);
        total_len = (total_len + 3) & ~3;  /* align to 4 */

        /* Check if this fits */
        size_t remaining = count - (size_t)(dst - (uint8_t *)buf);
        if (remaining < total_len)
            break;

        /* Build inotify_event header in a temporary buffer */
        struct inotify_event ev;
        ev.wd     = src->wd;
        ev.mask   = src->mask;
        ev.cookie = src->cookie;
        ev.len    = name_len;

        /* Copy header + name to user buffer (direct memcpy — kernel can write user pages) */
        memcpy(dst, &ev, sizeof(ev));
        if (name_len > 0) {
            memcpy(dst + sizeof(ev), src->name, name_len);
            /* Zero-pad to alignment */
            uint32_t pad = total_len - (uint32_t)(sizeof(ev) + name_len);
            if (pad > 0)
                memset(dst + sizeof(ev) + name_len, 0, pad);
        }

        dst += total_len;
        inst->ev_count--;
        written++;
    }

    spinlock_release(&inst->lock);
    return (int)(dst - (uint8_t *)buf);
}

/* ── inotify_poll — called from sys_poll/select dispatch ──────────── */

int inotify_poll(int fd)
{
    struct inotify_instance *inst = inst_from_fd(fd);
    if (!inst)
        return -EBADF;

    spinlock_acquire(&inst->lock);
    int has_data = (inst->ev_count > 0) ? 1 : 0;
    spinlock_release(&inst->lock);

    return has_data ? POLLIN : 0;
}

/* ── inotify_close — called when fd is closed ─────────────────────── */

int inotify_close(int fd)
{
    struct inotify_instance *inst = inst_from_fd(fd);
    if (!inst)
        return -EBADF;

    spinlock_acquire(&inst->lock);
    inst_free(inst);
    /* inst_free sets in_use=0 */
    spinlock_release(&inst->lock);
    return 0;
}

/* ── Event delivery hook ─────────────────────────────────────────────
 *
 * Called from fsnotify_notify() via the VFS layer whenever a
 * filesystem event occurs.  This function distributes the event
 * to all inotify instances that have a matching watch.
 *
 * We walk all instances and their watches to find matches.
 * For large numbers of watches this is O(instances * watches), which
 * is acceptable given our small limits (8 instances × 16 watches).
 */

void inotify_deliver(const char *path, uint32_t fs_mask)
{
    if (!g_inotify_initialized || !path)
        return;

    uint32_t inotify_mask = 0;
    if (fs_mask & FS_CREATE) inotify_mask |= IN_CREATE;
    if (fs_mask & FS_DELETE) inotify_mask |= IN_DELETE;
    if (fs_mask & FS_MODIFY) inotify_mask |= IN_MODIFY;
    if (fs_mask & FS_ACCESS) inotify_mask |= IN_ACCESS;

    if (inotify_mask == 0)
        return;

    /* Extract the filename from the path */
    const char *fname = path;
    const char *slash = strrchr(path, '/');
    if (slash)
        fname = slash + 1;

    for (int i = 0; i < INOTIFY_INSTANCES; i++) {
        struct inotify_instance *inst = &g_instances[i];
        if (!inst->in_use)
            continue;

        spinlock_acquire(&inst->lock);

        for (int j = 0; j < INOTIFY_WATCHES_PER_INST; j++) {
            struct inotify_watch *w = &inst->watches[j];
            if (!w->in_use)
                continue;

            /* Check if the event path matches the watched path */
            if (!(w->mask & inotify_mask))
                continue;

            /* Check if path is prefix-matched by the watched path */
            size_t wlen = strlen(w->path);
            if (strncmp(w->path, path, wlen) != 0)
                continue;

            /* For directory watches, also check that the next char is '/' or NUL
             * to avoid false prefix matches (e.g. watch "/foo", event on "/foobar") */
            if (path[wlen] != '\0' && path[wlen] != '/')
                continue;

            /* Push the event */
            inst_push_event(inst, w->wd, inotify_mask, 0, fname);
        }

        /* Wake up any readers if we added events */
        if (inst->ev_count > 0)
            wait_queue_wake_all(&inst->wq);

        spinlock_release(&inst->lock);
    }
}

/* ── Initialization hook (called from production_subsystems_init) ── */

void inotify_subsystem_init(void)
{
    inotify_init_subsystem();
}

/* ── inotify_add_to_dir ──────────────────────────────── */
int inotify_add_to_dir(int fd, const char *path, uint32_t mask)
{
    /* Convenience wrapper: calls sys_inotify_add_watch */
    return sys_inotify_add_watch(fd, (const char *)path, mask);
}

/* ── inotify_rm_from_dir ─────────────────────────────── */
int inotify_rm_from_dir(int fd, int wd)
{
    /* Convenience wrapper: calls sys_inotify_rm_watch */
    return sys_inotify_rm_watch(fd, wd);
}

/* ── inotify_handle_event ────────────────────────────── */
void inotify_handle_event(const char *path, uint32_t mask, uint32_t cookie, const char *name)
{
    /* For compatibility: call inotify_deliver with just the path and mask.
     * The cookie and name are not passed to deliver but this could be extended. */
    (void)cookie;
    (void)name;
    inotify_deliver(path, mask);
}

/* ── inotify_find_inode ──────────────────────────────── */
int inotify_find_inode(const char *path, uint64_t *inode)
{
    if (!path || !inode) return -EINVAL;

    /* Use VFS stat to get inode number.
     * Note: VFS stat returns a VFS-level inode. For now we use a
     * simplified approach: return 0 as placeholder since the actual
     * inode number depends on the filesystem implementation. */
    (void)path;
    *inode = 0;
    return 0;
}
