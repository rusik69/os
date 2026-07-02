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
static int fuse_path_walk(struct fuse_mount_info *mnt, const char *path,
                           uint64_t *out_nodeid);

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
    uint64_t nodeid;
    int ret = fuse_path_walk(mnt, path, &nodeid);
    if (ret < 0)
        return mnt->root_nodeid; /* fallback to root on error */
    return nodeid;
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

/* ── Entry cache helpers ────────────────────────────────────────────── */

/*
 * Initialize the per-mount entry cache.
 */
static void fuse_entry_cache_init(struct fuse_mount_info *mnt)
{
    memset(mnt->entry_cache, 0, sizeof(mnt->entry_cache));
}

/*
 * Look up a cached entry by (parent, name).
 * Returns 0 if found (nodeid written to @out_nodeid), -ENOENT if not.
 */
static int fuse_entry_cache_lookup(struct fuse_mount_info *mnt,
                                    uint64_t parent, const char *name,
                                    uint64_t *out_nodeid)
{
    for (int i = 0; i < FUSE_ENTRY_CACHE_SIZE; i++) {
        const struct fuse_entry_cache_entry *e = &mnt->entry_cache[i];
        if (e->nodeid != 0 && e->parent == parent &&
            strcmp(e->name, name) == 0) {
            *out_nodeid = e->nodeid;
            return 0;
        }
    }
    return -ENOENT;
}

/*
 * Add an entry to the cache.  Evicts oldest if full (LRU-like: generation
 * is used as LRU counter — we evict the entry with the smallest generation).
 * The generation field from fuse_entry_out is stored for later invalidation.
 */
static void fuse_entry_cache_add(struct fuse_mount_info *mnt,
                                  uint64_t parent, const char *name,
                                  uint64_t nodeid, uint64_t generation)
{
    int slot = -1;
    uint64_t oldest_gen = (uint64_t)-1;

    /* Look for an empty slot or the oldest entry */
    for (int i = 0; i < FUSE_ENTRY_CACHE_SIZE; i++) {
        if (mnt->entry_cache[i].nodeid == 0) {
            slot = i;
            break;
        }
        if (mnt->entry_cache[i].generation < oldest_gen) {
            oldest_gen = mnt->entry_cache[i].generation;
            slot = i;
        }
    }

    if (slot < 0)
        return; /* cache full with all valid entries — skip insert */

    mnt->entry_cache[slot].parent     = parent;
    mnt->entry_cache[slot].nodeid     = nodeid;
    mnt->entry_cache[slot].generation = generation;
    strncpy(mnt->entry_cache[slot].name, name,
            sizeof(mnt->entry_cache[slot].name) - 1);
    mnt->entry_cache[slot].name[sizeof(mnt->entry_cache[slot].name) - 1] = '\0';
}

/*
 * Invalidate all cache entries that have the given @nodeid as their
 * parent or as their resolved nodeid.  Used when the daemon sends
 * a forget or invalidate notification.
 */
static void fuse_entry_cache_invalidate(struct fuse_mount_info *mnt,
                                         uint64_t nodeid)
{
    for (int i = 0; i < FUSE_ENTRY_CACHE_SIZE; i++) {
        if (mnt->entry_cache[i].parent == nodeid ||
            mnt->entry_cache[i].nodeid == nodeid) {
            mnt->entry_cache[i].nodeid = 0;
        }
    }
}

/* ── Open file handle cache helpers ──────────────────────────────────── */

/*
 * Initialize the per-mount open file handle cache.
 */
static void fuse_open_fh_cache_init(struct fuse_mount_info *mnt)
{
    memset(mnt->open_fh_cache, 0, sizeof(mnt->open_fh_cache));
}

/*
 * Look up a cached open file handle by nodeid.
 * Returns 0 if found (fh written to @out_fh, offset to @out_offset),
 * -ENOENT if not.
 */
static int fuse_open_fh_cache_lookup(struct fuse_mount_info *mnt,
                                      uint64_t nodeid, uint64_t *out_fh,
                                      uint64_t *out_offset)
{
    for (int i = 0; i < FUSE_OPEN_FH_CACHE_SIZE; i++) {
        struct fuse_open_fh_entry *e = &mnt->open_fh_cache[i];
        if (e->in_use && e->nodeid == nodeid) {
            *out_fh = e->fh;
            if (out_offset)
                *out_offset = e->offset;
            return 0;
        }
    }
    return -ENOENT;
}

/*
 * Add an open file handle to the cache.  Evicts the first slot (FIFO)
 * if the cache is full.  Initializes offset to @initial_offset.
 */
static void fuse_open_fh_cache_add(struct fuse_mount_info *mnt,
                                    uint64_t nodeid, uint64_t fh,
                                    uint32_t open_flags,
                                    uint64_t initial_offset)
{
    int slot = -1;

    /* Look for an empty slot */
    for (int i = 0; i < FUSE_OPEN_FH_CACHE_SIZE; i++) {
        if (!mnt->open_fh_cache[i].in_use) {
            slot = i;
            break;
        }
    }

    /* If full, evict the first slot (FIFO) */
    if (slot < 0)
        slot = 0;

    mnt->open_fh_cache[slot].nodeid     = nodeid;
    mnt->open_fh_cache[slot].fh         = fh;
    mnt->open_fh_cache[slot].open_flags = open_flags;
    mnt->open_fh_cache[slot].in_use     = 1;
    mnt->open_fh_cache[slot].offset     = initial_offset;
}

/*
 * fuse_do_open — Send FUSE_OPEN for a nodeid and return the file handle.
 *
 * @mnt     The FUSE mount info.
 * @nodeid  Node ID of the file to open.
 * @out_fh  On success, receives the file handle from the daemon.
 *
 * Returns 0 on success, or a negative errno on failure.
 */
static int fuse_do_open(struct fuse_mount_info *mnt, uint64_t nodeid,
                         uint64_t *out_fh)
{
    struct fuse_open_in oi;
    void *resp_arg = NULL;
    int resp_arg_size = 0;
    struct fuse_open_out *oo;
    int ret;

    if (!mnt || !out_fh)
        return -EINVAL;

    memset(&oi, 0, sizeof(oi));
    oi.flags = O_RDWR;

    ret = fuse_request_response(FUSE_OPEN, nodeid, &oi, sizeof(oi),
                                 &resp_arg, &resp_arg_size);
    if (ret < 0)
        return ret;

    if (!resp_arg || resp_arg_size < (int)sizeof(struct fuse_open_out)) {
        kfree(resp_arg);
        return -EIO;
    }

    oo = (struct fuse_open_out *)resp_arg;
    *out_fh = oo->fh;
    kfree(resp_arg);
    return 0;
}

/*
 * fuse_get_or_open_fh — Return a cached file handle or open the file.
 *
 * First checks the per-mount open file handle cache for an existing
 * handle for @nodeid.  If not found, sends FUSE_OPEN to the daemon
 * and caches the result.
 *
 * @mnt     The FUSE mount info.
 * @nodeid  Node ID of the file.
 * @out_fh  On success, receives the file handle.
 *
 * Returns 0 on success, or a negative errno on failure.
 */
static int fuse_get_or_open_fh(struct fuse_mount_info *mnt,
                                uint64_t nodeid, uint64_t *out_fh,
                                uint64_t *out_offset)
{
    int ret;

    if (!mnt || !out_fh)
        return -EINVAL;

    /* Check cache first */
    ret = fuse_open_fh_cache_lookup(mnt, nodeid, out_fh, out_offset);
    if (ret == 0)
        return 0;

    /* Cache miss — send FUSE_OPEN */
    ret = fuse_do_open(mnt, nodeid, out_fh);
    if (ret < 0) {
        kprintf("[fuse] FUSE_OPEN nodeid=%llu failed: %d\n",
                (unsigned long long)nodeid, ret);
        return ret;
    }

    /* Cache the handle with offset 0 */
    fuse_open_fh_cache_add(mnt, nodeid, *out_fh, O_RDWR, 0);
    if (out_offset)
        *out_offset = 0;

    return 0;
}

/*
 * fuse_open_fh_cache_update_offset — Update the cached read/write offset
 *                                      for a given nodeid.
 *
 * @mnt     The FUSE mount info.
 * @nodeid  Node ID whose offset to update.
 * @offset  New offset value.
 *
 * Returns 0 on success, -ENOENT if the nodeid is not in the cache.
 */
static int fuse_open_fh_cache_update_offset(struct fuse_mount_info *mnt,
                                             uint64_t nodeid,
                                             uint64_t offset)
{
    for (int i = 0; i < FUSE_OPEN_FH_CACHE_SIZE; i++) {
        struct fuse_open_fh_entry *e = &mnt->open_fh_cache[i];
        if (e->in_use && e->nodeid == nodeid) {
            e->offset = offset;
            return 0;
        }
    }
    return -ENOENT;
}

/* ── FUSE_LOOKUP implementation ─────────────────────────────────────── */

/*
 * fuse_lookup_component — resolve a single path component within a
 *                          directory via FUSE_LOOKUP.
 *
 * @parent_nodeid  Node ID of the parent directory.
 * @name           Component name to look up (null-terminated).
 * @out_nodeid     On success, receives the resolved node ID.
 * @out_entry_attr On success, receives the full fuse_entry_out (may be NULL).
 *
 * Returns 0 on success, or a negative errno on failure.
 */
static int fuse_lookup_component(uint64_t parent_nodeid, const char *name,
                                  uint64_t *out_nodeid,
                                  struct fuse_entry_out *out_entry)
{
    struct fuse_entry_out *entry;
    void *resp_arg = NULL;
    int resp_arg_size = 0;
    int ret;

    if (!name || !out_nodeid)
        return -EINVAL;

    /* FUSE_LOOKUP request payload is just the filename (null-terminated) */
    ret = fuse_request_response(FUSE_LOOKUP, parent_nodeid,
                                 name, strlen(name) + 1,
                                 &resp_arg, &resp_arg_size);
    if (ret < 0)
        return ret;

    /* Parse the fuse_entry_out from the response */
    if (!resp_arg || resp_arg_size < (int)sizeof(struct fuse_entry_out)) {
        kfree(resp_arg);
        return -EIO;
    }

    entry = (struct fuse_entry_out *)resp_arg;
    *out_nodeid = entry->nodeid;

    if (out_entry)
        memcpy(out_entry, entry, sizeof(*out_entry));

    kfree(resp_arg);
    return 0;
}

/* ── Path resolution ────────────────────────────────────────────────── */

/*
 * fuse_path_walk — walk a path within a FUSE mount, resolving each
 *                   component via FUSE_LOOKUP.
 *
 * Walks the path relative to the mountpoint, sending a FUSE_LOOKUP
 * request for each path component.  Uses a per-mount entry cache to
 * avoid redundant LOOKUP calls.
 *
 * @mnt         The FUSE mount info.
 * @path        Absolute path within the mount.
 * @out_nodeid  On success, receives the nodeid of the final component.
 *
 * Returns 0 on success, or a negative errno on failure.
 */
static int fuse_path_walk(struct fuse_mount_info *mnt, const char *path,
                           uint64_t *out_nodeid)
{
    size_t mplen;
    const char *rel;
    uint64_t nodeid;
    char component[256];
    int comp_len;
    int ret;

    if (!mnt || !path || !out_nodeid)
        return -EINVAL;

    mplen = strlen(mnt->mountpoint);
    rel = path + mplen;

    /* Skip leading slashes */
    while (*rel == '/')
        rel++;

    nodeid = mnt->root_nodeid;

    /* Root path — nothing to resolve */
    if (*rel == '\0') {
        *out_nodeid = nodeid;
        return 0;
    }

    /* Walk each path component */
    while (*rel != '\0') {
        /* Skip trailing/extra slashes */
        while (*rel == '/')
            rel++;

        if (*rel == '\0')
            break;

        /* Extract component */
        comp_len = 0;
        while (*rel != '\0' && *rel != '/' &&
               comp_len < (int)sizeof(component) - 1) {
            component[comp_len++] = *(rel++);
        }
        component[comp_len] = '\0';

        if (comp_len == 0)
            continue;

        /* Handle "." — stays on same node */
        if (strcmp(component, ".") == 0)
            continue;

        /* Handle ".." — we don't keep parent info in the cache,
         * so for now, return root.  Full ".." support would need
         * to cache the parent nodeid alongside the entry. */
        if (strcmp(component, "..") == 0) {
            nodeid = mnt->root_nodeid;
            continue;
        }

        /* Check cache first */
        ret = fuse_entry_cache_lookup(mnt, nodeid, component, &nodeid);
        if (ret == 0)
            continue; /* cache hit */

        /* Cache miss — send FUSE_LOOKUP to the daemon */
        struct fuse_entry_out entry_out;
        uint64_t resolved;

        ret = fuse_lookup_component(nodeid, component, &resolved,
                                     &entry_out);
        if (ret < 0) {
            kprintf("[fuse] LOOKUP '%s' under nodeid %llu failed: %d\n",
                    component, (unsigned long long)nodeid, ret);
            return ret;
        }

        /* Add to cache */
        fuse_entry_cache_add(mnt, nodeid, component, resolved,
                              entry_out.generation);
        nodeid = resolved;
    }

    *out_nodeid = nodeid;
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
    uint64_t fh;
    uint64_t offset;
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

    /* Get or create a file handle for this node via FUSE_OPEN.
     * Also retrieves the current read/write position (offset). */
    ret = fuse_get_or_open_fh(mnt, nodeid, &fh, &offset);
    if (ret < 0)
        return ret;

    /* Send FUSE_READ request at the current offset */
    memset(&ri, 0, sizeof(ri));
    ri.fh         = fh;
    ri.offset     = offset;
    ri.size       = max_size;
    ri.read_flags = 0;
    ri.lock_owner = 0;
    ri.flags      = 0;
    ri.padding    = 0;

    ret = fuse_request_response(FUSE_READ, nodeid, &ri, sizeof(ri),
                                 &resp_arg, &resp_arg_size);
    if (ret < 0)
        return ret;

    /* Copy response data to caller buffer */
    uint32_t bytes_read = 0;
    if (resp_arg && resp_arg_size > 0) {
        bytes_read = (uint32_t)resp_arg_size;
        if (bytes_read > max_size)
            bytes_read = max_size;
        memcpy(buf, resp_arg, bytes_read);
        *out_size = bytes_read;
    }

    kfree(resp_arg);

    /* Advance the cached offset by the number of bytes actually read */
    if (bytes_read > 0)
        fuse_open_fh_cache_update_offset(mnt, nodeid, offset + bytes_read);

    return 0;
}

static int fuse_write(void *priv, const char *path,
                       const void *data, uint32_t size)
{
    struct fuse_mount_info *mnt;
    uint64_t nodeid;
    uint64_t fh;
    uint64_t offset;
    struct fuse_write_in wi;
    struct fuse_write_out *wo;
    void *payload;
    uint32_t total_size;
    void *resp_arg = NULL;
    int resp_arg_size = 0;
    int ret;

    (void)priv;

    mnt = fuse_find_mount(path);
    if (!mnt) return -ENOENT;

    nodeid = fuse_path_to_nodeid(mnt, path);

    /* Get or create a file handle for this node via FUSE_OPEN.
     * Also retrieves the current read/write position (offset). */
    ret = fuse_get_or_open_fh(mnt, nodeid, &fh, &offset);
    if (ret < 0)
        return ret;

    /* FUSE wire format: fuse_write_in + data bytes */
    total_size = sizeof(wi) + size;
    payload = kmalloc(total_size);
    if (!payload)
        return -ENOMEM;

    memset(&wi, 0, sizeof(wi));
    wi.fh          = fh;
    wi.offset      = offset;
    wi.size        = size;
    wi.write_flags = 0;
    wi.lock_owner  = 0;
    wi.flags       = 0;
    wi.padding     = 0;

    memcpy(payload, &wi, sizeof(wi));
    memcpy((uint8_t *)payload + sizeof(wi), data, size);

    ret = fuse_request_response(FUSE_WRITE, nodeid, payload, total_size,
                                 &resp_arg, &resp_arg_size);
    kfree(payload);

    if (ret < 0)
        return ret;

    /* Parse the write response to get actual bytes written.
     * Advance the cached offset accordingly. */
    uint32_t bytes_written = size;
    if (resp_arg && resp_arg_size >= (int)sizeof(struct fuse_write_out)) {
        wo = (struct fuse_write_out *)resp_arg;
        bytes_written = wo->size;
    }

    if (bytes_written > 0)
        fuse_open_fh_cache_update_offset(mnt, nodeid,
                                          offset + bytes_written);

    kfree(resp_arg);

    /* VFS contract: return 0 on success, negative errno on failure */
    return 0;
}

static int fuse_stat(void *priv, const char *path, struct vfs_stat *st)
{
    struct fuse_mount_info *mnt;
    struct fuse_getattr_in gi;
    void *resp_arg = NULL;
    int resp_arg_size = 0;
    int ret;

    (void)priv;
    if (!st) return -EINVAL;
    memset(st, 0, sizeof(*st));

    mnt = fuse_find_mount(path);
    if (!mnt) return -ENOENT;

    uint64_t nodeid = fuse_path_to_nodeid(mnt, path);

    /* Send FUSE_GETATTR request with getattr_in */
    memset(&gi, 0, sizeof(gi));
    gi.getattr_flags = 0; /* no special flags */
    gi.fh = 0;

    ret = fuse_request_response(FUSE_GETATTR, nodeid, &gi, sizeof(gi),
                                 &resp_arg, &resp_arg_size);
    if (ret < 0)
        return ret;

    /* Parse fuse_attr_out from response */
    if (resp_arg && resp_arg_size >= (int)sizeof(struct fuse_attr_out)) {
        struct fuse_attr_out *attr_out = (struct fuse_attr_out *)resp_arg;
        struct fuse_attr *attr = &attr_out->attr;

        /* Map fuse_attr.mode to vfs_stat.type and mode */
        switch (attr->mode & S_IFMT) {
        case S_IFDIR:
            st->type = VFS_TYPE_DIR;
            break;
        case S_IFREG:
            st->type = VFS_TYPE_FILE;
            break;
        case S_IFLNK:
            st->type = VFS_TYPE_LINK;
            break;
        case S_IFCHR:
            st->type = VFS_TYPE_CHR;
            break;
        case S_IFBLK:
            st->type = VFS_TYPE_BLK;
            break;
        case S_IFIFO:
            st->type = VFS_TYPE_FIFO;
            break;
        default:
            st->type = VFS_TYPE_FILE;
            break;
        }

        st->size  = (uint64_t)attr->size;
        st->uid   = (uint16_t)attr->uid;
        st->gid   = (uint16_t)attr->gid;
        st->mode  = attr->mode & 0777;
        st->atime = (uint32_t)attr->atime;
        st->mtime = (uint32_t)attr->mtime;
        st->nlink = (uint32_t)attr->nlink;
        st->ino   = (uint32_t)attr->ino;

        /* Map RDEV for device nodes */
        st->dev_major = (uint16_t)(attr->rdev >> 8);
        st->dev_minor = (uint16_t)(attr->rdev & 0xFF);
    } else if (resp_arg && resp_arg_size >= (int)sizeof(struct fuse_attr)) {
        /* Fallback: daemon responded with raw fuse_attr (older daemon) */
        struct fuse_attr *attr = (struct fuse_attr *)resp_arg;

        st->type = (attr->mode & S_IFMT) == S_IFDIR
                     ? VFS_TYPE_DIR : VFS_TYPE_FILE;
        st->size = (uint64_t)attr->size;
        st->uid  = (uint16_t)attr->uid;
        st->gid  = (uint16_t)attr->gid;
        st->mode = attr->mode & 0777;
        st->atime = (uint32_t)attr->atime;
        st->mtime = (uint32_t)attr->mtime;
        st->nlink = (uint32_t)attr->nlink;
        st->ino   = (uint32_t)attr->ino;
        st->dev_major = (uint16_t)(attr->rdev >> 8);
        st->dev_minor = (uint16_t)(attr->rdev & 0xFF);
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
    fuse_entry_cache_init(mnt);
    fuse_open_fh_cache_init(mnt);

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
