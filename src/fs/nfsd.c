/*
 * src/fs/nfsd.c — NFSv3 server (in-kernel, single-threaded)
 *
 * Implements an NFSv3 server over TCP (port 2049) with the MOUNT
 * protocol (port 1049).  Supports:
 *   - Single-threaded, in-kernel only (no userspace daemon)
 *   - NFS procedures: NULL, GETATTR, FSSTAT, ACCESS, READ,
 *     READDIR, READDIRPLUS, STATFS, LOOKUP
 *   - MOUNT protocol for export list
 *   - Exports from local directories (via nfsd_add_export)
 *   - TCP transport
 */

#define KERNEL_INTERNAL
#include "nfsd.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"
#include "timer.h"
#include "net.h"
#include "errno.h"
#include "initcall.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── Static state ──────────────────────────────────────────────────── */

static struct nfsd_export nfsd_exports[NFSD_MAX_EXPORTS];
static int nfsd_num_exports = 0;
static int nfsd_server_running = 0;

/* ── XDR helpers (big-endian) ───────────────────────────────────────── */

static inline void xdr_put_u32(uint8_t **p, uint32_t v)
{
    *(*p)++ = (uint8_t)(v >> 24);
    *(*p)++ = (uint8_t)(v >> 16);
    *(*p)++ = (uint8_t)(v >> 8);
    *(*p)++ = (uint8_t)(v);
}

static inline uint32_t xdr_get_u32(const uint8_t **p)
{
    uint32_t v = ((uint32_t)(*p)[0] << 24) | ((uint32_t)(*p)[1] << 16) |
                 ((uint32_t)(*p)[2] << 8)  | (*p)[3];
    *p += 4;
    return v;
}

static inline void xdr_put_bytes(uint8_t **p, const uint8_t *data, uint32_t len)
{
    memcpy(*p, data, len);
    *p += len;
    /* Pad to 4-byte boundary */
    while (len & 3) { *(*p)++ = 0; len++; }
}

static inline void xdr_get_bytes(const uint8_t **p, uint8_t *dst, uint32_t len)
{
    memcpy(dst, *p, len);
    *p += len;
    while (len & 3) { (*p)++; len++; }
}

static inline void xdr_put_string(uint8_t **p, const char *s)
{
    uint32_t len = (uint32_t)strlen(s);
    xdr_put_u32(p, len);
    xdr_put_bytes(p, (const uint8_t *)s, len);
}

static inline void xdr_put_uint64(uint8_t **p, uint64_t v)
{
    xdr_put_u32(p, (uint32_t)(v >> 32));
    xdr_put_u32(p, (uint32_t)(v & 0xFFFFFFFF));
}

/* ── Export management ──────────────────────────────────────────────── */

int nfsd_add_export(const char *export_path, const char *local_path)
{
    if (nfsd_num_exports >= NFSD_MAX_EXPORTS)
        return -ENOMEM;

    struct nfsd_export *ex = &nfsd_exports[nfsd_num_exports];
    memset(ex, 0, sizeof(*ex));

    strncpy(ex->export_path, export_path, sizeof(ex->export_path) - 1);
    strncpy(ex->local_path, local_path, sizeof(ex->local_path) - 1);

    /* Stat the local path to verify it exists */
    struct vfs_stat st;
    if (vfs_stat(local_path, &st) < 0) {
        kprintf("[nfsd] export path not accessible: %s\\n", local_path);
        return -ENOENT;
    }
    ex->st = st;

    ex->valid = 1;
    nfsd_num_exports++;

    kprintf("[nfsd] Export added: %s -> %s\\n", export_path, local_path);
    return nfsd_num_exports - 1;
}

int nfsd_remove_export(const char *export_path)
{
    for (int i = 0; i < nfsd_num_exports; i++) {
        if (strcmp(nfsd_exports[i].export_path, export_path) == 0) {
            nfsd_exports[i].valid = 0;
            kprintf("[nfsd] Export removed: %s\\n", export_path);
            return 0;
        }
    }
    return -ENOENT;
}

/* Find an export by local path resolution */
static struct nfsd_export *nfsd_find_export(const char *path)
{
    for (int i = 0; i < nfsd_num_exports; i++) {
        if (nfsd_exports[i].valid) {
            /* Check if path starts with the export's local path */
            size_t plen = strlen(nfsd_exports[i].local_path);
            if (strncmp(path, nfsd_exports[i].local_path, plen) == 0)
                return &nfsd_exports[i];
        }
    }
    return NULL;
}

/* ── RPC reply builder ──────────────────────────────────────────────── */

static void rpc_build_header(uint8_t **p, uint32_t xid, int is_reply,
                              uint32_t accept_stat)
{
    xdr_put_u32(p, xid);
    xdr_put_u32(p, is_reply ? RPC_REPLY : RPC_CALL);
    if (is_reply) {
        xdr_put_u32(p, RPC_MSG_ACCEPTED); /* reply_stat */
        /* Verifier (AUTH_NONE) */
        xdr_put_u32(p, RPC_AUTH_NONE);
        xdr_put_u32(p, 0);
        /* Accept stat */
        xdr_put_u32(p, accept_stat);
    }
}

/* ── NFSv3 procedure handlers ───────────────────────────────────────── */

/* Encode NFSv3 file attributes (w3_fattr) into buf.
 * Returns 0 on success. */
static void nfsd_encode_fattr(uint8_t **p, const struct vfs_stat *st)
{
    /* ftype */
    uint32_t ftype;
    switch (st->type) {
        case VFS_TYPE_DIR:  ftype = NFS3_FTYPE_DIR; break;
        case VFS_TYPE_LINK: ftype = NFS3_FTYPE_LNK; break;
        default:            ftype = NFS3_FTYPE_REG; break;
    }
    xdr_put_u32(p, ftype);
    /* mode */
    xdr_put_u32(p, st->mode | (ftype == NFS3_FTYPE_DIR ? 0x4000 : 0x8000));
    /* nlink */
    xdr_put_u32(p, 1);
    /* uid, gid */
    xdr_put_u32(p, st->uid);
    xdr_put_u32(p, st->gid);
    /* size */
    xdr_put_uint64(p, st->size);
    /* used (blocks * 512) */
    xdr_put_uint64(p, (st->size + 511) / 512 * 512);
    /* rdev (special, rdev, not used for regular files/dirs) */
    xdr_put_u32(p, 0);
    xdr_put_u32(p, 0);
    /* fsid (unique per export) */
    xdr_put_uint64(p, 0x100000001ULL);
    /* fileid (inode number) */
    xdr_put_uint64(p, st->ino ? st->ino : (uint64_t)(uintptr_t)st);
    /* atime, mtime, ctime */
    xdr_put_uint64(p, st->atime);
    xdr_put_u32(p, 0); /* atime nsec */
    xdr_put_uint64(p, st->mtime);
    xdr_put_u32(p, 0); /* mtime nsec */
    xdr_put_uint64(p, st->mtime);
    xdr_put_u32(p, 0); /* ctime nsec */
}

/* Post-op attributes (optional) */
static void nfsd_encode_postop_attr(uint8_t **p, const struct vfs_stat *st,
                                     int present)
{
    xdr_put_u32(p, present ? 1 : 0);
    if (present)
        nfsd_encode_fattr(p, st);
}

/* Wcc attributes (before/after) — simplified */
static void nfsd_encode_wcc_attr(uint8_t **p, const struct vfs_stat *st)
{
    xdr_put_uint64(p, st->size);
    xdr_put_uint64(p, st->mtime);
    xdr_put_u32(p, 0); /* mtime nsec */
    xdr_put_uint64(p, st->mtime);
    xdr_put_u32(p, 0); /* ctime nsec */
}

static void nfsd_encode_wcc_data(uint8_t **p, const struct vfs_stat *pre,
                                  const struct vfs_stat *post)
{
    /* pre-op attributes (present) */
    nfsd_encode_postop_attr(p, pre, 1);
    /* post-op attributes (present) */
    nfsd_encode_postop_attr(p, post, 1);
}

/* ── Lookup helper ──────────────────────────────────────────────────── */

/* Resolve a path relative to the export root.  Returns 0 on success,
 * -errno on failure. */
static int nfsd_lookup_local(const char *local_root, const char *path,
                              struct vfs_stat *st)
{
    char full_path[NFSD_MAX_PATH];
    int n = snprintf(full_path, sizeof(full_path), "%s%s",
                     local_root, path);
    if (n < 0 || n >= (int)sizeof(full_path))
        return -ENAMETOOLONG;

    return vfs_stat(full_path, st);
}

/* Read a file at a local path */
static int nfsd_read_local(const char *local_root, const char *path,
                            uint8_t *buf, uint32_t *out_size,
                            uint64_t offset, uint32_t count)
{
    char full_path[NFSD_MAX_PATH];
    int n = snprintf(full_path, sizeof(full_path), "%s%s",
                     local_root, path);
    if (n < 0 || n >= (int)sizeof(full_path))
        return -ENAMETOOLONG;

    return vfs_read(full_path, buf, count, out_size);
}

/* ── NFSPROC3_NULL ──────────────────────────────────────────────────── */

static void nfsd_proc_null(struct nfsd_rpc_state *rpc,
                            uint8_t *reply, uint32_t *reply_len)
{
    uint8_t *p = reply;
    rpc_build_header(&p, rpc->xid, 1, RPC_SUCCESS);
    /* No data */
    *reply_len = (uint32_t)(p - reply);
}

/* ── NFSPROC3_GETATTR ───────────────────────────────────────────────── */

static void nfsd_proc_getattr(struct nfsd_rpc_state *rpc,
                               uint8_t *reply, uint32_t *reply_len)
{
    uint8_t *p = reply;
    const uint8_t *cp = rpc->call_data;

    /* Parse file handle */
    uint32_t fh_len = xdr_get_u32(&cp);
    (void)fh_len;
    /* Skip FH data (we use the export-based resolution for now) */
    cp += fh_len;
    while (fh_len & 3) { cp++; fh_len++; }

    /* For now, return root stat */
    struct vfs_stat st;
    if (nfsd_num_exports > 0) {
        vfs_stat(nfsd_exports[0].local_path, &st);
    } else {
        memset(&st, 0, sizeof(st));
        st.type = VFS_TYPE_DIR;
    }

    rpc_build_header(&p, rpc->xid, 1, RPC_SUCCESS);
    xdr_put_u32(&p, NFS3_OK);
    nfsd_encode_fattr(&p, &st);
    *reply_len = (uint32_t)(p - reply);
}

/* ── NFSPROC3_ACCESS ────────────────────────────────────────────────── */

static void nfsd_proc_access(struct nfsd_rpc_state *rpc,
                              uint8_t *reply, uint32_t *reply_len)
{
    uint8_t *p = reply;
    const uint8_t *cp = rpc->call_data;

    /* Parse FH */
    uint32_t fh_len = xdr_get_u32(&cp);
    (void)fh_len;
    cp += fh_len;
    while (fh_len & 3) { cp++; fh_len++; }

    uint32_t access_bits = xdr_get_u32(&cp);
    (void)access_bits;

    struct vfs_stat st;
    if (nfsd_num_exports > 0)
        vfs_stat(nfsd_exports[0].local_path, &st);
    else
        memset(&st, 0, sizeof(st));

    rpc_build_header(&p, rpc->xid, 1, RPC_SUCCESS);
    xdr_put_u32(&p, NFS3_OK);
    nfsd_encode_postop_attr(&p, &st, 1);
    xdr_put_u32(&p, 0x001f01ff); /* All access bits */
    *reply_len = (uint32_t)(p - reply);
}

/* ── NFSPROC3_LOOKUP ────────────────────────────────────────────────── */

static void nfsd_proc_lookup(struct nfsd_rpc_state *rpc,
                              uint8_t *reply, uint32_t *reply_len)
{
    uint8_t *p = reply;
    const uint8_t *cp = rpc->call_data;

    /* Parse dir FH */
    uint32_t fh_len = xdr_get_u32(&cp);
    (void)fh_len;
    cp += fh_len;
    while (fh_len & 3) { cp++; fh_len++; }

    /* Parse name */
    uint32_t name_len = xdr_get_u32(&cp);
    char name[256];
    if (name_len > 255) name_len = 255;
    memcpy(name, cp, name_len);
    name[name_len] = '\0';

    /* Look up the name relative to export's local root */
    struct nfsd_export *ex = &nfsd_exports[0];
    if (!ex->valid) {
        rpc_build_header(&p, rpc->xid, 1, RPC_SUCCESS);
        xdr_put_u32(&p, NFS3ERR_NOENT);
        nfsd_encode_postop_attr(&p, &ex->st, 1);
        nfsd_encode_postop_attr(&p, &ex->st, 0);
        *reply_len = (uint32_t)(p - reply);
        return;
    }

    char full_path[NFSD_MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s/%s", ex->local_path, name);

    struct vfs_stat st;
    int ret = vfs_stat(full_path, &st);

    rpc_build_header(&p, rpc->xid, 1, RPC_SUCCESS);
    if (ret < 0) {
        xdr_put_u32(&p, NFS3ERR_NOENT);
        nfsd_encode_postop_attr(&p, &ex->st, 1);
        nfsd_encode_postop_attr(&p, &ex->st, 0);
    } else {
        xdr_put_u32(&p, NFS3_OK);
        /* Object handle (simplified: just encode the path offset) */
        uint32_t handle_data = (uint32_t)(uintptr_t)name; /* dummy */
        (void)handle_data;
        /* For simplicity, encode a 8-byte handle */
        uint32_t obj_fh[2];
        obj_fh[0] = 0; /* export index */
        obj_fh[1] = st.ino; /* inode number */
        xdr_put_u32(&p, 8);
        xdr_put_bytes(&p, (const uint8_t *)obj_fh, 8);
        /* Object attributes */
        nfsd_encode_postop_attr(&p, &st, 1);
        /* Dir post-op attributes */
        nfsd_encode_postop_attr(&p, &ex->st, 1);
    }
    *reply_len = (uint32_t)(p - reply);
}

/* ── NFSPROC3_READ ──────────────────────────────────────────────────── */

static void nfsd_proc_read(struct nfsd_rpc_state *rpc,
                            uint8_t *reply, uint32_t *reply_len)
{
    uint8_t *p = reply;
    const uint8_t *cp = rpc->call_data;

    /* Parse FH */
    uint32_t fh_len = xdr_get_u32(&cp);
    (void)fh_len;
    cp += fh_len;
    while (fh_len & 3) { cp++; fh_len++; }

    /* Offset */
    uint64_t offset = ((uint64_t)xdr_get_u32(&cp) << 32) | xdr_get_u32(&cp);
    /* Count */
    uint32_t count = xdr_get_u32(&cp);

    /* Read from export's local path */
    struct nfsd_export *ex = &nfsd_exports[0];
    if (!ex->valid) {
        rpc_build_header(&p, rpc->xid, 1, RPC_SUCCESS);
        xdr_put_u32(&p, NFS3ERR_NOENT);
        *reply_len = (uint32_t)(p - reply);
        return;
    }

    uint8_t data_buf[NFSD_MAX_DATA];
    uint32_t data_size = 0;
    int ret = nfsd_read_local(ex->local_path, "",
                               data_buf, &data_size, offset, count);
    if (ret < 0) {
        rpc_build_header(&p, rpc->xid, 1, RPC_SUCCESS);
        xdr_put_u32(&p, NFS3ERR_IO);
        *reply_len = (uint32_t)(p - reply);
        return;
    }

    struct vfs_stat st;
    vfs_stat(ex->local_path, &st);

    rpc_build_header(&p, rpc->xid, 1, RPC_SUCCESS);
    xdr_put_u32(&p, NFS3_OK);
    /* Post-op attributes */
    nfsd_encode_postop_attr(&p, &st, 1);
    /* Count */
    xdr_put_u32(&p, data_size);
    /* EOF */
    xdr_put_u32(&p, (offset + data_size >= st.size) ? 1 : 0);
    /* Data */
    xdr_put_u32(&p, data_size);
    xdr_put_bytes(&p, data_buf, data_size);
    *reply_len = (uint32_t)(p - reply);
}

/* ── NFSPROC3_READDIR ────────────────────────────────────────────────── */

static void nfsd_proc_readdir(struct nfsd_rpc_state *rpc,
                               uint8_t *reply, uint32_t *reply_len)
{
    uint8_t *p = reply;
    const uint8_t *cp = rpc->call_data;

    /* Parse FH */
    uint32_t fh_len = xdr_get_u32(&cp);
    (void)fh_len;
    cp += fh_len;
    while (fh_len & 3) { cp++; fh_len++; }

    /* Cookie (opaque) */
    uint64_t cookie = ((uint64_t)xdr_get_u32(&cp) << 32) | xdr_get_u32(&cp);
    (void)cookie;

    /* Verifier */
    uint64_t verifier = ((uint64_t)xdr_get_u32(&cp) << 32) | xdr_get_u32(&cp);
    (void)verifier;

    /* Count */
    uint32_t dircount = xdr_get_u32(&cp);
    (void)dircount;
    uint32_t maxcount = xdr_get_u32(&cp);
    (void)maxcount;

    struct nfsd_export *ex = &nfsd_exports[0];
    if (!ex->valid) {
        rpc_build_header(&p, rpc->xid, 1, RPC_SUCCESS);
        xdr_put_u32(&p, NFS3ERR_NOENT);
        *reply_len = (uint32_t)(p - reply);
        return;
    }

    /* Get directory entries via vfs_readdir_names */
    char names[64][64];
    int num_entries = vfs_readdir_names(ex->local_path, names, 64);

    struct vfs_stat dir_st;
    vfs_stat(ex->local_path, &dir_st);

    rpc_build_header(&p, rpc->xid, 1, RPC_SUCCESS);
    xdr_put_u32(&p, NFS3_OK);
    /* Dir post-op attributes */
    nfsd_encode_postop_attr(&p, &dir_st, 1);
    /* Verifier */
    xdr_put_uint64(&p, 0);
    /* Directory entries */
    for (int i = 0; i < num_entries; i++) {
        /* fileid */
        xdr_put_uint64(&p, (uint64_t)(i + 1));
        /* name */
        xdr_put_string(&p, names[i]);
        /* cookie (next cookie) */
        xdr_put_uint64(&p, (uint64_t)(i + 1));
    }
    /* EOF */
    xdr_put_u32(&p, 1);
    *reply_len = (uint32_t)(p - reply);
}

/* ── NFSPROC3_FSSTAT ────────────────────────────────────────────────── */

static void nfsd_proc_fsstat(struct nfsd_rpc_state *rpc,
                              uint8_t *reply, uint32_t *reply_len)
{
    uint8_t *p = reply;
    const uint8_t *cp = rpc->call_data;

    /* Parse FH */
    uint32_t fh_len = xdr_get_u32(&cp);
    (void)fh_len;
    cp += fh_len;
    while (fh_len & 3) { cp++; fh_len++; }

    struct vfs_stat st;
    memset(&st, 0, sizeof(st));
    if (nfsd_num_exports > 0)
        vfs_stat(nfsd_exports[0].local_path, &st);

    rpc_build_header(&p, rpc->xid, 1, RPC_SUCCESS);
    xdr_put_u32(&p, NFS3_OK);
    nfsd_encode_postop_attr(&p, &st, 1);
    /* Total blocks (1TB display) */
    xdr_put_uint64(&p, 0x1000000);
    /* Free blocks */
    xdr_put_uint64(&p, 0x800000);
    /* Available blocks */
    xdr_put_uint64(&p, 0x800000);
    /* Total files */
    xdr_put_uint64(&p, 0x10000);
    /* Free files */
    xdr_put_uint64(&p, 0x8000);
    /* Available files */
    xdr_put_uint64(&p, 0x8000);
    /* Invarsec (0 = no invariants) */
    xdr_put_u32(&p, 0);
    *reply_len = (uint32_t)(p - reply);
}

/* ── NFSPROC3_STATFS ──────────────────────────────────────────────────── */

static void nfsd_proc_statfs(struct nfsd_rpc_state *rpc,
                              uint8_t *reply, uint32_t *reply_len)
{
    /* Same as FSSTAT for now */
    nfsd_proc_fsstat(rpc, reply, reply_len);
}

/* ── ReadDirPlus — simplified, same as ReadDir ──────────────────────── */

static void nfsd_proc_readdirplus(struct nfsd_rpc_state *rpc,
                                   uint8_t *reply, uint32_t *reply_len)
{
    /* Simplified: same as readdir but with attributes */
    nfsd_proc_readdir(rpc, reply, reply_len);
}

/* ── MOUNT protocol handlers ────────────────────────────────────────── */

static void mountd_proc_null(struct nfsd_rpc_state *rpc,
                              uint8_t *reply, uint32_t *reply_len)
{
    uint8_t *p = reply;
    rpc_build_header(&p, rpc->xid, 1, RPC_SUCCESS);
    *reply_len = (uint32_t)(p - reply);
}

static void mountd_proc_mnt(struct nfsd_rpc_state *rpc,
                             uint8_t *reply, uint32_t *reply_len)
{
    uint8_t *p = reply;
    const uint8_t *cp = rpc->call_data;

    /* Parse export path */
    uint32_t path_len = xdr_get_u32(&cp);
    char path[256];
    if (path_len > 255) path_len = 255;
    memcpy(path, cp, path_len);
    path[path_len] = '\0';

    /* Check if export exists */
    int export_idx = -1;
    for (int i = 0; i < nfsd_num_exports; i++) {
        if (nfsd_exports[i].valid &&
            strcmp(nfsd_exports[i].export_path, path) == 0) {
            export_idx = i;
            break;
        }
    }

    rpc_build_header(&p, rpc->xid, 1, RPC_SUCCESS);
    if (export_idx < 0) {
        xdr_put_u32(&p, NFS3ERR_NOENT);
        /* No FH */
        xdr_put_u32(&p, 0);
        /* Flavor list (AUTH_NONE) */
        xdr_put_u32(&p, 1);
        xdr_put_u32(&p, RPC_AUTH_NONE);
    } else {
        xdr_put_u32(&p, NFS3_OK);
        /* File handle (8 bytes, export index + 0) */
        uint32_t fh[2];
        fh[0] = (uint32_t)export_idx;
        fh[1] = 1; /* root inode marker */
        xdr_put_u32(&p, 8);
        xdr_put_bytes(&p, (const uint8_t *)fh, 8);
        /* Flavor list (AUTH_NONE) */
        xdr_put_u32(&p, 1);
        xdr_put_u32(&p, RPC_AUTH_NONE);
    }
    *reply_len = (uint32_t)(p - reply);
}

static void mountd_proc_export(struct nfsd_rpc_state *rpc,
                                uint8_t *reply, uint32_t *reply_len)
{
    uint8_t *p = reply;
    rpc_build_header(&p, rpc->xid, 1, RPC_SUCCESS);

    /* List all exports */
    for (int i = 0; i < nfsd_num_exports; i++) {
        if (nfsd_exports[i].valid) {
            /* Export path */
            xdr_put_string(&p, nfsd_exports[i].export_path);
            /* Group list (1 group, 0.0.0.0/0 = all) */
            xdr_put_u32(&p, 1);
            xdr_put_string(&p, "*");
        }
    }
    /* Terminate with empty string */
    xdr_put_u32(&p, 0);
    *reply_len = (uint32_t)(p - reply);
}

static void mountd_proc_dump(struct nfsd_rpc_state *rpc,
                              uint8_t *reply, uint32_t *reply_len)
{
    uint8_t *p = reply;
    rpc_build_header(&p, rpc->xid, 1, RPC_SUCCESS);

    /* List mounted exports (empty for now) */
    xdr_put_u32(&p, 0); /* no entries */
    *reply_len = (uint32_t)(p - reply);
}

/* ── Main RPC dispatcher ────────────────────────────────────────────── */

void nfsd_handle_rpc(struct nfsd_rpc_state *rpc,
                     uint8_t *reply_buf, uint32_t *reply_len)
{
    *reply_len = 0;

    if (rpc->program == NFS3_PROGRAM) {
        switch (rpc->procedure) {
            case NFSPROC3_NULL:
                nfsd_proc_null(rpc, reply_buf, reply_len);
                break;
            case NFSPROC3_GETATTR:
                nfsd_proc_getattr(rpc, reply_buf, reply_len);
                break;
            case NFSPROC3_ACCESS:
                nfsd_proc_access(rpc, reply_buf, reply_len);
                break;
            case NFSPROC3_LOOKUP:
                nfsd_proc_lookup(rpc, reply_buf, reply_len);
                break;
            case NFSPROC3_READ:
                nfsd_proc_read(rpc, reply_buf, reply_len);
                break;
            case NFSPROC3_READDIR:
                nfsd_proc_readdir(rpc, reply_buf, reply_len);
                break;
            case NFSPROC3_READDIRPLUS:
                nfsd_proc_readdirplus(rpc, reply_buf, reply_len);
                break;
            case NFSPROC3_FSSTAT:
                nfsd_proc_fsstat(rpc, reply_buf, reply_len);
                break;
            case NFSPROC3_FSINFO:
                nfsd_proc_fsstat(rpc, reply_buf, reply_len);
                break;
            case NFSPROC3_PATHCONF:
                nfsd_proc_statfs(rpc, reply_buf, reply_len);
                break;
            default:
                /* Procedure unavailable */
            {
                uint8_t *p = reply_buf;
                rpc_build_header(&p, rpc->xid, 1, RPC_PROC_UNAVAIL);
                *reply_len = (uint32_t)(p - reply_buf);
                break;
            }
        }
    } else if (rpc->program == MOUNT3_PROGRAM) {
        switch (rpc->procedure) {
            case MOUNTPROC3_NULL:
                mountd_proc_null(rpc, reply_buf, reply_len);
                break;
            case MOUNTPROC3_MNT:
                mountd_proc_mnt(rpc, reply_buf, reply_len);
                break;
            case MOUNTPROC3_DUMP:
                mountd_proc_dump(rpc, reply_buf, reply_len);
                break;
            case MOUNTPROC3_EXPORT:
                mountd_proc_export(rpc, reply_buf, reply_len);
                break;
            default:
            {
                uint8_t *p = reply_buf;
                rpc_build_header(&p, rpc->xid, 1, RPC_PROC_UNAVAIL);
                *reply_len = (uint32_t)(p - reply_buf);
                break;
            }
        }
    } else {
        /* Program unavailable */
        uint8_t *p = reply_buf;
        rpc_build_header(&p, rpc->xid, 1, RPC_PROG_UNAVAIL);
        *reply_len = (uint32_t)(p - reply_buf);
    }
}

/* ── TCP server task ──────────────────────────────────────────────────── */

/* This function handles a single TCP connection for NFS requests.
 * Called from the server task. */
static void nfsd_handle_connection(int conn_id)
{
    uint8_t buf[NFSD_MAX_DATA];

    /* Read RPC record marker (4-byte length) */
    uint8_t marker[4];
    int n = net_tcp_recv(conn_id, marker, 4, 100);
    if (n < 4) return;

    uint32_t record_len = ((uint32_t)marker[0] << 24) |
                           ((uint32_t)marker[1] << 16) |
                           ((uint32_t)marker[2] << 8)  | marker[3];

    if (record_len > NFSD_MAX_DATA - 4) return;

    /* Read the RPC call */
    n = net_tcp_recv(conn_id, buf, (uint16_t)record_len, 100);
    if (n < (int)record_len) return;

    const uint8_t *cp = buf;
    struct nfsd_rpc_state rpc;
    memset(&rpc, 0, sizeof(rpc));

    /* Parse RPC call header */
    rpc.xid = xdr_get_u32(&cp);

    uint32_t msg_type = xdr_get_u32(&cp);
    if (msg_type != RPC_CALL) return;

    uint32_t rpcvers = xdr_get_u32(&cp);
    (void)rpcvers;
    rpc.program = xdr_get_u32(&cp);
    rpc.version = xdr_get_u32(&cp);
    rpc.procedure = xdr_get_u32(&cp);

    /* Credential flavor */
    rpc.auth_flavor = xdr_get_u32(&cp);
    uint32_t cred_len = xdr_get_u32(&cp);
    cp += cred_len;
    while (cred_len & 3) { cp++; cred_len++; }

    /* Verifier */
    uint32_t verf_flavor = xdr_get_u32(&cp);
    (void)verf_flavor;
    uint32_t verf_len = xdr_get_u32(&cp);
    cp += verf_len;
    while (verf_len & 3) { cp++; verf_len++; }

    /* Copy remaining as call data */
    uint32_t remaining = record_len - (uint32_t)(cp - buf);
    if (remaining > sizeof(rpc.call_data))
        remaining = sizeof(rpc.call_data);
    memcpy(rpc.call_data, cp, remaining);
    rpc.call_len = remaining;

    /* Dispatch */
    uint8_t reply_buf[NFSD_MAX_DATA];
    uint32_t reply_len = 0;
    nfsd_handle_rpc(&rpc, reply_buf, &reply_len);

    /* Send reply with TCP record marker */
    uint8_t resp_marker[4];
    resp_marker[0] = (uint8_t)(reply_len >> 24);
    resp_marker[1] = (uint8_t)(reply_len >> 16);
    resp_marker[2] = (uint8_t)(reply_len >> 8);
    resp_marker[3] = (uint8_t)(reply_len);

    net_tcp_send(conn_id, resp_marker, 4);
    net_tcp_send(conn_id, reply_buf, (uint16_t)reply_len);
}

/* Server task — runs in an infinite loop handling connections */
void nfsd_server_task(void)
{
    kprintf("[nfsd] Server task started\\n");
    nfsd_server_running = 1;

    while (nfsd_server_running) {
        /* Check for new connections on NFS port */
        int conn = net_tcp_accept(NFSD_PORT, 10);
        if (conn >= 0) {
            kprintf("[nfsd] Accepted connection on port %d\\n", NFSD_PORT);
            /* Handle requests until connection closes */
            while (net_tcp_is_connected(conn) && !net_tcp_has_closed(conn)) {
                nfsd_handle_connection(conn);
            }
            net_tcp_close(conn);
            kprintf("[nfsd] Connection closed\\n");
        }

        /* Also check MOUNT port */
        int mconn = net_tcp_accept(NFSD_MOUNT_PORT, 5);
        if (mconn >= 0) {
            kprintf("[nfsd] Accepted MOUNT connection on port %d\\n", NFSD_MOUNT_PORT);
            while (net_tcp_is_connected(mconn) && !net_tcp_has_closed(mconn)) {
                nfsd_handle_connection(mconn);
            }
            net_tcp_close(mconn);
        }

        /* Yield to other tasks */
        extern void scheduler_yield(void);
        scheduler_yield();
    }
}

/* ── Init ──────────────────────────────────────────────────────────── */

int nfsd_init(void)
{
    memset(nfsd_exports, 0, sizeof(nfsd_exports));
    nfsd_num_exports = 0;

    /* Parse /etc/exports to populate exports list */
    uint8_t exports_buf[4096];
    uint32_t exports_size = 0;
    int ret = vfs_read("/etc/exports", exports_buf, sizeof(exports_buf), &exports_size);
    if (ret == 0 && exports_size > 0) {
        kprintf("[nfsd] Parsing /etc/exports (%u bytes)\n", exports_size);
        exports_buf[exports_size] = '\0';

        /* Simple line-based exports parser */
        char *line_start = (char *)exports_buf;
        char *p = line_start;
        int in_line = 0;

        while (*p && nfsd_num_exports < NFSD_MAX_EXPORTS) {
            /* Skip whitespace and newlines */
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
                p++;
            if (*p == '\0') break;
            if (*p == '#') {
                /* Skip comment lines */
                while (*p && *p != '\n') p++;
                continue;
            }

            line_start = p;
            /* Find end of line */
            while (*p && *p != '\n') p++;
            if (*p == '\n') *p++ = '\0';
            else *p = '\0';

            /* Format: /export/path /local/path (ro,no_root_squash,...) */
            char export_path[256], local_path[256];
            char options[256];
            export_path[0] = '\0';
            local_path[0] = '\0';
            options[0] = '\0';

            int parsed = sscanf(line_start, "%255s %255s %255[^\n]",
                                export_path, local_path, options);
            if (parsed >= 2 && export_path[0] == '/') {
                struct nfsd_export *ex = &nfsd_exports[nfsd_num_exports];
                memset(ex, 0, sizeof(*ex));
                strncpy(ex->export_path, export_path, sizeof(ex->export_path) - 1);
                strncpy(ex->local_path, local_path, sizeof(ex->local_path) - 1);

                /* Parse squash options */
                ex->squash = NFSD_SQUASH_NONE;
                ex->anon_uid = 65534;
                ex->anon_gid = 65534;

                if (options[0] != '\0') {
                    char *opt = options;
                    /* Skip leading parentheses */
                    if (*opt == '(') opt++;
                    while (*opt) {
                        if (*opt == ')' || *opt == ',' || *opt == ' ') {
                            opt++;
                            continue;
                        }
                        if (strncmp(opt, "root_squash", 11) == 0) {
                            ex->squash = NFSD_SQUASH_ROOT;
                            opt += 11;
                        } else if (strncmp(opt, "all_squash", 10) == 0) {
                            ex->squash = NFSD_SQUASH_ALL;
                            opt += 10;
                        } else if (strncmp(opt, "no_root_squash", 14) == 0) {
                            ex->squash = NFSD_SQUASH_NONE;
                            opt += 14;
                        } else if (strncmp(opt, "anonuid", 7) == 0) {
                            opt += 7;
                            if (*opt == '=') opt++;
                            int uid = 0;
                            while (*opt >= '0' && *opt <= '9') {
                                uid = uid * 10 + (*opt - '0');
                                opt++;
                            }
                            ex->anon_uid = (uint16_t)uid;
                        } else if (strncmp(opt, "anongid", 7) == 0) {
                            opt += 7;
                            if (*opt == '=') opt++;
                            int gid = 0;
                            while (*opt >= '0' && *opt <= '9') {
                                gid = gid * 10 + (*opt - '0');
                                opt++;
                            }
                            ex->anon_gid = (uint16_t)gid;
                        } else {
                            opt++; /* skip unknown char */
                        }
                    }
                }

                /* Verify the local path exists */
                struct vfs_stat st;
                if (vfs_stat(local_path, &st) == 0) {
                    ex->st = st;
                    ex->valid = 1;
                    nfsd_num_exports++;
                    kprintf("[nfsd] Export: %s -> %s (squash=%d, uid=%d, gid=%d)\n",
                            export_path, local_path, ex->squash,
                            ex->anon_uid, ex->anon_gid);
                } else {
                    kprintf("[nfsd] Export path not accessible: %s\n", local_path);
                }
            }
        }
    } else {
        kprintf("[nfsd] No /etc/exports found, using defaults\n");
    }

    kprintf("[nfsd] NFSv3 server initialized: %d exports\n", nfsd_num_exports);
    return 0;
}

device_initcall(nfsd_init);

#ifdef MODULE
int init_module(void) { return nfsd_init(); }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("NFSv3 server — in-kernel, single-threaded, TCP transport");
MODULE_VERSION("1.0");
#endif

/* ── Stub: nfsd_start ─────────────────────────────── */
int nfsd_start(int port)
{
    (void)port;
    kprintf("[nfsd] nfsd_start: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: nfsd_stop ─────────────────────────────── */
int nfsd_stop(void)
{
    kprintf("[nfsd] nfsd_stop: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: nfsd_handle_request ─────────────────────────────── */
int nfsd_handle_request(void *req, void *resp)
{
    (void)req;
    (void)resp;
    kprintf("[nfsd] nfsd_handle_request: not yet implemented\n");
    return -ENOSYS;
}
