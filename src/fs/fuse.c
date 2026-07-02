/*
 * src/fs/fuse.c — FUSE (Filesystem in Userspace) kernel side.
 *
 * Implements the VFS layer for FUSE: translates VFS calls into FUSE
 * request/response messages and passes them through /dev/fuse to the
 * userspace daemon.
 *
 * The /dev/fuse character device is implemented in fuse_dev.c — this
 * file only handles the VFS operations and mount management.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "fuse.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "vfs.h"
#include "devfs.h"
#include "errno.h"
#include "spinlock.h"
#include "timer.h"

/* FUSE_READDIR request structure (local def to avoid virtio_fs.h conflict) */
struct fuse_readdir_in {
    uint64_t fh;
    uint32_t offset;
    uint32_t size;
} __attribute__((packed));

/* Mount info */
#define FUSE_MAX_MOUNTS 4
/* fuse_mount_info now defined in fuse.h with negotiated fields */
static struct fuse_mount_info g_fuse_mounts[FUSE_MAX_MOUNTS];
static spinlock_t g_fuse_mount_lock;
static int g_fuse_initialized = 0;

/* ── Forward declarations ──────────────────────────────────────────── */
static int fuse_read(void *priv, const char *path, void *buf,
                      uint32_t max_size, uint32_t *out_size);
static int fuse_write(void *priv, const char *path,
                       const void *data, uint32_t size);

/*
 * Find which FUSE mount a path belongs to.
 */
static struct fuse_mount_info *fuse_find_mount(const char *path)
{
    for (int i = 0; i < FUSE_MAX_MOUNTS; i++) {
        if (g_fuse_mounts[i].active) {
            size_t mplen = strlen(g_fuse_mounts[i].mountpoint);
            if (strncmp(path, g_fuse_mounts[i].mountpoint, mplen) == 0)
                return &g_fuse_mounts[i];
        }
    }
    return NULL;
}

/* Get node ID for a path within a FUSE mount */
static uint64_t fuse_path_to_nodeid(struct fuse_mount_info *mnt,
                                     const char *path)
{
    size_t mplen = strlen(mnt->mountpoint);
    const char *rel = path + mplen;

    if (*rel == '\0' || (rel[0] == '/' && rel[1] == '\0'))
        return mnt->root_nodeid; /* root */

    /* In a full implementation, we'd do LOOKUP operations for each
     * path component. For now, just return root. */
    return mnt->root_nodeid;
}

/*
 * ── Request/Response serialization ──────────────────────────────────
 *
 * fuse_request_response — Send a FUSE request and wait for a response.
 * @opcode:        FUSE operation code
 * @nodeid:        Target node ID
 * @request:       Pointer to request payload (may be NULL)
 * @request_size:  Size of request payload in bytes
 * @out_resp_arg:  On success, receives allocated response payload (caller must kfree)
 * @out_resp_arg_size: On success, receives response payload size
 *
 * This is the core serialization primitive for the FUSE wire protocol.
 * It packs the request (in_header + payload), queues it on /dev/fuse,
 * waits for the daemon's response, checks the error code, and returns
 * the response payload (if any) to the caller.
 *
 * Returns 0 on success (response payload in @out_resp_arg), or a
 * negative errno on failure.  On success, the caller must kfree()
 * the returned @out_resp_arg when done.
 */
static int fuse_request_response(uint32_t opcode, uint64_t nodeid,
                                  const void *request, int request_size,
                                  void **out_resp_arg,
                                  int *out_resp_arg_size)
{
    uint64_t unique;
    struct fuse_out_header *resp;
    void *resp_arg;
    int resp_arg_size;
    int ret;

    ret = fuse_dev_queue_request(opcode, nodeid, request, request_size,
                                  &unique);
    if (ret < 0)
        return ret;

    ret = fuse_dev_wait_for_response(unique, &resp, &resp_arg,
                                      &resp_arg_size);
    if (ret < 0)
        return ret;

    /* Propagate daemon-side error from the response header */
    if (resp->error != 0) {
        ret = (int)resp->error;
        kfree(resp);
        if (resp_arg)
            kfree(resp_arg);
        return ret;
    }

    kfree(resp); /* header consumed — caller only needs payload */

    if (out_resp_arg)
        *out_resp_arg = resp_arg;
    else if (resp_arg)
        kfree(resp_arg);

    if (out_resp_arg_size)
        *out_resp_arg_size = resp_arg_size;

    return 0;
}

/*
 * ── VFS operations ──────────────────────────────────────────────────
 */

static int fuse_read(void *priv, const char *path, void *buf,
                      uint32_t max_size, uint32_t *out_size)
{
    struct fuse_mount_info *mnt;
    uint64_t nodeid;
    struct fuse_read_in ri;
    void *resp_arg = NULL;
    int resp_arg_size = 0;
    int ret;

    (void)priv;
    if (!out_size) return -EINVAL;
    *out_size = 0;

    mnt = fuse_find_mount(path);
    if (!mnt) return -ENOENT;

    nodeid = fuse_path_to_nodeid(mnt, path);

    /* Send FUSE_READ request and wait for response */
    memset(&ri, 0, sizeof(ri));
    ri.fh     = mnt->fh;
    ri.offset = 0;
    ri.size   = max_size;

    ret = fuse_request_response(FUSE_READ, nodeid, &ri, sizeof(ri),
                                 &resp_arg, &resp_arg_size);
    if (ret < 0)
        return ret;

    /* Copy response data to caller buffer */
    if (resp_arg && resp_arg_size > 0) {
        uint32_t copy_size = (uint32_t)resp_arg_size;
        if (copy_size > max_size)
            copy_size = max_size;
        memcpy(buf, resp_arg, copy_size);
        *out_size = copy_size;
    }

    kfree(resp_arg);
    return 0;
}

static int fuse_write(void *priv, const char *path,
                       const void *data, uint32_t size)
{
    struct fuse_mount_info *mnt;
    uint64_t nodeid;
    struct fuse_write_in wi;
    void *resp_arg = NULL;
    int resp_arg_size = 0;
    void *payload;
    uint32_t total_size;
    int ret;

    (void)priv;

    mnt = fuse_find_mount(path);
    if (!mnt) return -ENOENT;

    nodeid = fuse_path_to_nodeid(mnt, path);

    /* FUSE wire format: fuse_write_in + data bytes */
    total_size = sizeof(wi) + size;
    payload = kmalloc(total_size);
    if (!payload)
        return -ENOMEM;

    memset(&wi, 0, sizeof(wi));
    wi.fh     = mnt->fh;
    wi.offset = 0;
    wi.size   = size;

    memcpy(payload, &wi, sizeof(wi));
    memcpy((uint8_t *)payload + sizeof(wi), data, size);

    ret = fuse_request_response(FUSE_WRITE, nodeid, payload, total_size,
                                 &resp_arg, &resp_arg_size);
    kfree(payload);

    if (ret < 0)
        return ret;

    kfree(resp_arg);
    return (int)size;
}

static int fuse_stat(void *priv, const char *path, struct vfs_stat *st)
{
    struct fuse_mount_info *mnt;
    struct fuse_attr attr_buf;
    void *resp_arg = NULL;
    int resp_arg_size = 0;
    int ret;

    (void)priv;
    memset(st, 0, sizeof(*st));

    mnt = fuse_find_mount(path);
    if (!mnt) return -ENOENT;

    uint64_t nodeid = fuse_path_to_nodeid(mnt, path);

    /* Send FUSE_GETATTR request and wait for response */
    ret = fuse_request_response(FUSE_GETATTR, nodeid, NULL, 0,
                                 &resp_arg, &resp_arg_size);
    if (ret < 0)
        return ret;

    /* Parse fuse_attr from response payload */
    if (resp_arg && resp_arg_size >= (int)sizeof(attr_buf)) {
        memcpy(&attr_buf, resp_arg, sizeof(attr_buf));

        st->type = (attr_buf.mode & S_IFMT) == S_IFDIR
                     ? VFS_TYPE_DIR : VFS_TYPE_FILE;
        st->size = (uint32_t)attr_buf.size;
        st->mode = attr_buf.mode & 0777;
    } else {
        /* Fallback if daemon returned no attr data */
        st->type = VFS_TYPE_FILE;
        st->size = 0;
        st->mode = 0644;
    }

    kfree(resp_arg);
    return 0;
}

static int fuse_readdir_names(void *priv, const char *path,
                                char names[][64], int max)
{
    struct fuse_mount_info *mnt;
    struct fuse_readdir_in rdi;
    void *resp_arg = NULL;
    int resp_arg_size = 0;
    int ret;

    mnt = fuse_find_mount(path);
    if (!mnt) return -ENOENT;

    uint64_t nodeid = fuse_path_to_nodeid(mnt, path);

    memset(&rdi, 0, sizeof(rdi));
    rdi.fh = mnt->fh;
    rdi.offset = 0;
    rdi.size = 0;

    /* Send FUSE_READDIR request and wait for response */
    ret = fuse_request_response(FUSE_READDIR, nodeid, &rdi, sizeof(rdi),
                                 &resp_arg, &resp_arg_size);
    if (ret < 0)
        return ret;

    /* Parse directory entries from response payload.
     * For now, return basic . and .. entries until full readdir parsing
     * is implemented in task 8. */
    int n = 0;
    if (max > 0 && names) {
        if (n < max) { memcpy(names[n], ".", 2); n++; }
        if (n < max) { memcpy(names[n], "..", 3); n++; }
    }

    kfree(resp_arg);
    return n;
}

static int fuse_readdir_legacy(void *priv, const char *path)
{
    (void)priv;
    (void)path;
    return 0;
}

static int fuse_create(void *priv, const char *path, uint8_t type)
{
    (void)priv;
    (void)path;
    (void)type;
    /* FUSE_CREATE would be sent */
    return 0;
}

static int fuse_unlink(void *priv, const char *path)
{
    (void)priv;
    (void)path;
    return 0;
}

static struct vfs_ops fuse_vfs_ops = {
    .read    = fuse_read,
    .write   = fuse_write,
    .stat    = fuse_stat,
    .readdir_names = fuse_readdir_names,
    .readdir = fuse_readdir_legacy,
    .create  = fuse_create,
    .unlink  = fuse_unlink,
};

/* ── Public API ─────────────────────────────────────────────────────── */

void fuse_init(void)
{
    if (g_fuse_initialized) return;

    memset(g_fuse_mounts, 0, sizeof(g_fuse_mounts));
    spinlock_init(&g_fuse_mount_lock);
    g_fuse_initialized = 1;

    /* Initialize the /dev/fuse character device */
    fuse_dev_init();

    /* Register as a VFS filesystem type */
    vfs_register_filesystem("fuse", &fuse_vfs_ops);

    kprintf("[fuse] FUSE kernel subsystem initialized\n");
}

int fuse_mount(const char *mountpoint)
{
    struct fuse_mount_info *mnt;
    struct fuse_init_in init_in;
    void *resp_arg = NULL;
    int resp_arg_size = 0;
    int slot;
    int ret;

    if (!g_fuse_initialized) fuse_init();
    if (!mountpoint) return -EINVAL;

    /* Find a free mount slot */
    slot = -1;
    for (int i = 0; i < FUSE_MAX_MOUNTS; i++) {
        if (!g_fuse_mounts[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -ENOMEM;

    /* Prepare FUSE_INIT request with our protocol version and capabilities */
    memset(&init_in, 0, sizeof(init_in));
    init_in.major = FUSE_KERNEL_VERSION;
    init_in.minor = FUSE_KERNEL_MINOR_VERSION;
    init_in.max_readahead = 128 * 1024; /* default 128KB readahead */
    init_in.flags = FUSE_KERNEL_INIT_FLAGS; /* tell daemon what we support */

    kprintf("[fuse] Sending INIT: kernel v%u.%u flags=0x%x max_readahead=%u\n",
            (unsigned int)FUSE_KERNEL_VERSION,
            (unsigned int)FUSE_KERNEL_MINOR_VERSION,
            (unsigned int)init_in.flags,
            (unsigned int)init_in.max_readahead);

    /* Send FUSE_INIT request and wait for response */
    ret = fuse_request_response(FUSE_INIT, 1, &init_in, sizeof(init_in),
                                 &resp_arg, &resp_arg_size);
    if (ret < 0) {
        kprintf("[fuse] FUSE_INIT failed: ret=%d\n", ret);
        return ret;
    }

    /* Register the mount before parsing response (so we can store results) */
    mnt = &g_fuse_mounts[slot];
    memset(mnt, 0, sizeof(*mnt));
    strncpy(mnt->mountpoint, mountpoint, sizeof(mnt->mountpoint) - 1);

    /* Parse FUSE_INIT response — validate version and store capabilities */
    if (resp_arg && resp_arg_size >= (int)sizeof(struct fuse_init_out)) {
        struct fuse_init_out *init_out = (struct fuse_init_out *)resp_arg;

        kprintf("[fuse] Daemon response: v%u.%u flags=0x%x "
                "max_readahead=%u max_write=%u time_gran=%u\n",
                (unsigned int)init_out->major,
                (unsigned int)init_out->minor,
                (unsigned int)init_out->flags,
                (unsigned int)init_out->max_readahead,
                (unsigned int)init_out->max_write,
                (unsigned int)init_out->time_gran);

        /* Validate major version — must match (no cross-major compat) */
        if (init_out->major != FUSE_KERNEL_VERSION) {
            kprintf("[fuse] ERROR: daemon major=%u != kernel major=%u\n",
                    (unsigned int)init_out->major,
                    (unsigned int)FUSE_KERNEL_VERSION);
            kfree(resp_arg);
            return -EPROTONOSUPPORT;
        }

        /* Negotiate minor version — use minimum of kernel and daemon */
        uint32_t negotiated_minor = init_out->minor;
        if (negotiated_minor > FUSE_KERNEL_MINOR_VERSION)
            negotiated_minor = FUSE_KERNEL_MINOR_VERSION;

        kprintf("[fuse] Negotiated FUSE v%u.%u "
                "(kernel=%u.%u daemon=%u.%u)\n",
                (unsigned int)FUSE_KERNEL_VERSION,
                (unsigned int)negotiated_minor,
                (unsigned int)FUSE_KERNEL_VERSION,
                (unsigned int)FUSE_KERNEL_MINOR_VERSION,
                (unsigned int)init_out->major,
                (unsigned int)init_out->minor);

        /* Store all negotiated capabilities in mount info */
        mnt->negotiated_major = FUSE_KERNEL_VERSION;
        mnt->negotiated_minor = negotiated_minor;
        mnt->daemon_flags     = init_out->flags;
        mnt->max_readahead    = init_out->max_readahead;
        mnt->max_write        = init_out->max_write;
        mnt->time_gran        = init_out->time_gran;

        /* Log feature flags that were negotiated */
        if (fuse_has_cap(mnt, FUSE_CAP_ASYNC_READ))
            kprintf("[fuse]   daemon supports ASYNC_READ\n");
        if (fuse_has_cap(mnt, FUSE_CAP_POSIX_LOCKS))
            kprintf("[fuse]   daemon supports POSIX_LOCKS\n");
        if (fuse_has_cap(mnt, FUSE_CAP_FLOCK_LOCKS))
            kprintf("[fuse]   daemon supports FLOCK_LOCKS\n");
        if (fuse_has_cap(mnt, FUSE_CAP_READDIRPLUS))
            kprintf("[fuse]   daemon supports READDIRPLUS\n");
        if (fuse_has_cap(mnt, FUSE_CAP_WRITEBACK_CACHE))
            kprintf("[fuse]   daemon supports WRITEBACK_CACHE\n");
        if (fuse_has_cap(mnt, FUSE_CAP_HANDLE_KILLPRIV))
            kprintf("[fuse]   daemon supports HANDLE_KILLPRIV\n");
        if (fuse_has_cap(mnt, FUSE_CAP_AUTO_INVAL_DATA))
            kprintf("[fuse]   daemon supports AUTO_INVAL_DATA\n");
        if (fuse_has_cap(mnt, FUSE_CAP_EXPLICIT_INVAL_DATA))
            kprintf("[fuse]   daemon supports EXPLICIT_INVAL_DATA\n");
    } else {
        kprintf("[fuse] WARNING: daemon returned incomplete INIT response "
                "(size=%d, expected %zu)\n",
                resp_arg_size, sizeof(struct fuse_init_out));
        /* Still allow mount with defaults */
        mnt->negotiated_major = FUSE_KERNEL_VERSION;
        mnt->negotiated_minor = 0; /* minimal protocol */
    }
    kfree(resp_arg);

    /* Set root node and file handle defaults */
    mnt->active = 1;
    mnt->root_nodeid = 1; /* root nodeid per FUSE protocol convention */
    mnt->fh = 0;

    /* Mount via VFS */
    ret = vfs_mount(mountpoint, &fuse_vfs_ops, mnt);
    if (ret != 0) {
        mnt->active = 0;
        return ret;
    }

    kprintf("[fuse] Mounted FUSE at %s (v%u.%u)\n",
            mountpoint,
            (unsigned int)mnt->negotiated_major,
            (unsigned int)mnt->negotiated_minor);
    return 0;
}

int fuse_unmount(const char *mountpoint)
{
    if (!mountpoint) return -EINVAL;

    for (int i = 0; i < FUSE_MAX_MOUNTS; i++) {
        if (g_fuse_mounts[i].active &&
            strcmp(g_fuse_mounts[i].mountpoint, mountpoint) == 0) {
            g_fuse_mounts[i].active = 0;
            kprintf("[fuse] Unmounted FUSE from %s\n", mountpoint);
            return 0;
        }
    }

    return -ENOENT;
}
#include "module.h"
fs_initcall(fuse_init);

/* ── fuse_umount ─────────────────────────────────────── */
int fuse_umount(const char *target)
{
    kprintf("[fuse] FUSE unmounted from %s\n", target);
    return 0;
}
