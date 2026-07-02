// SPDX-License-Identifier: GPL-2.0-only
/*
 * nfs_proc.c — Shared NFSv3 procedure infrastructure
 *
 * Provides XDR marshaling/unmarshaling for NFSv3 data types used by
 * both the NFS client (nfs.c) and NFS server (nfsd.c).
 * Includes MOUNTv3 protocol helpers.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "nfs_proc.h"

/* ── XDR primitives (big-endian wire format) ──────────────────── */

void nfs_xdr_put_u32(uint8_t **p, uint32_t v)
{
    *(*p)++ = (uint8_t)(v >> 24);
    *(*p)++ = (uint8_t)(v >> 16);
    *(*p)++ = (uint8_t)(v >> 8);
    *(*p)++ = (uint8_t)(v);
}

uint32_t nfs_xdr_get_u32(const uint8_t **p)
{
    uint32_t v = ((uint32_t)(*p)[0] << 24) | ((uint32_t)(*p)[1] << 16) |
                 ((uint32_t)(*p)[2] << 8)  | (*p)[3];
    *p += 4;
    return v;
}

void nfs_xdr_put_bytes(uint8_t **p, const void *data, uint32_t len)
{
    memcpy(*p, data, len);
    *p += len;
    /* Pad to 4-byte boundary */
    while (len & 3) {
        *(*p)++ = 0;
        len++;
    }
}

void nfs_xdr_get_bytes(const uint8_t **p, void *dst, uint32_t len)
{
    memcpy(dst, *p, len);
    *p += len;
    /* Skip padding */
    while (len & 3) {
        (*p)++;
        len++;
    }
}

void nfs_xdr_put_string(uint8_t **p, const char *s)
{
    uint32_t len = (uint32_t)strlen(s);
    nfs_xdr_put_u32(p, len);
    nfs_xdr_put_bytes(p, (const uint8_t *)s, len);
}

uint32_t nfs_xdr_get_string(const uint8_t **p, char *buf, uint32_t maxlen)
{
    uint32_t len = nfs_xdr_get_u32(p);
    if (len >= maxlen)
        len = maxlen - 1;
    nfs_xdr_get_bytes(p, (uint8_t *)buf, len);
    buf[len] = '\0';
    return len;
}

void nfs_xdr_put_uint64(uint8_t **p, uint64_t v)
{
    nfs_xdr_put_u32(p, (uint32_t)(v >> 32));
    nfs_xdr_put_u32(p, (uint32_t)(v & 0xFFFFFFFF));
}

uint64_t nfs_xdr_get_uint64(const uint8_t **p)
{
    uint32_t hi = nfs_xdr_get_u32(p);
    uint32_t lo = nfs_xdr_get_u32(p);
    return ((uint64_t)hi << 32) | lo;
}

/* ── NFSv3 file handle marshaling ─────────────────────────────── */

void nfs_xdr_put_fh(uint8_t **p, const struct nfs3_fh *fh)
{
    uint32_t len = fh->len;
    if (len > NFS_MAX_FH_SIZE)
        len = NFS_MAX_FH_SIZE;
    nfs_xdr_put_u32(p, len);
    nfs_xdr_put_bytes(p, fh->data, len);
}

void nfs_xdr_get_fh(const uint8_t **p, struct nfs3_fh *fh)
{
    fh->len = nfs_xdr_get_u32(p);
    if (fh->len > NFS_MAX_FH_SIZE)
        fh->len = NFS_MAX_FH_SIZE;
    nfs_xdr_get_bytes(p, fh->data, fh->len);
}

/* ── NFSv3 file attributes marshaling ─────────────────────────── */

void nfs_xdr_encode_fattr(uint8_t **p, const struct nfs3_fattr *fa)
{
    nfs_xdr_put_u32(p, fa->type);
    nfs_xdr_put_u32(p, fa->mode);
    nfs_xdr_put_u32(p, fa->nlink);
    nfs_xdr_put_u32(p, fa->uid);
    nfs_xdr_put_u32(p, fa->gid);
    nfs_xdr_put_uint64(p, fa->size);
    nfs_xdr_put_uint64(p, fa->used);
    nfs_xdr_put_u32(p, fa->rdev_specdata1);
    nfs_xdr_put_u32(p, fa->rdev_specdata2);
    nfs_xdr_put_uint64(p, fa->fsid);
    nfs_xdr_put_uint64(p, fa->fileid);
    nfs_xdr_put_uint64(p, fa->atime);
    nfs_xdr_put_u32(p, fa->atime_nsec);
    nfs_xdr_put_uint64(p, fa->mtime);
    nfs_xdr_put_u32(p, fa->mtime_nsec);
    nfs_xdr_put_uint64(p, fa->ctime);
    nfs_xdr_put_u32(p, fa->ctime_nsec);
}

void nfs_xdr_decode_fattr(const uint8_t **p, struct nfs3_fattr *fa)
{
    fa->type        = nfs_xdr_get_u32(p);
    fa->mode        = nfs_xdr_get_u32(p);
    fa->nlink       = nfs_xdr_get_u32(p);
    fa->uid         = nfs_xdr_get_u32(p);
    fa->gid         = nfs_xdr_get_u32(p);
    fa->size        = nfs_xdr_get_uint64(p);
    fa->used        = nfs_xdr_get_uint64(p);
    fa->rdev_specdata1 = nfs_xdr_get_u32(p);
    fa->rdev_specdata2 = nfs_xdr_get_u32(p);
    fa->fsid        = nfs_xdr_get_uint64(p);
    fa->fileid      = nfs_xdr_get_uint64(p);
    fa->atime       = nfs_xdr_get_uint64(p);
    fa->atime_nsec  = nfs_xdr_get_u32(p);
    fa->mtime       = nfs_xdr_get_uint64(p);
    fa->mtime_nsec  = nfs_xdr_get_u32(p);
    fa->ctime       = nfs_xdr_get_uint64(p);
    fa->ctime_nsec  = nfs_xdr_get_u32(p);
}

/* Convert a vfs_stat to an NFSv3 fattr structure */
void nfs3_fattr_from_vfs_stat(struct nfs3_fattr *fa, const struct vfs_stat *st)
{
    memset(fa, 0, sizeof(*fa));

    switch (st->type) {
    case VFS_TYPE_DIR:
        fa->type = NFS3_FTYPE_DIR;
        break;
    case VFS_TYPE_LINK:
        fa->type = NFS3_FTYPE_LNK;
        break;
    default:
        fa->type = NFS3_FTYPE_REG;
        break;
    }

    fa->mode   = st->mode;
    fa->nlink  = 1;
    fa->uid    = st->uid;
    fa->gid    = st->gid;
    fa->size   = st->size;
    fa->used   = (st->size + 511) / 512 * 512;
    fa->fsid   = 0x100000001ULL;
    fa->fileid = st->ino ? st->ino : (uint64_t)(uintptr_t)st;
    fa->atime  = st->atime;
    fa->mtime  = st->mtime;
    fa->ctime  = st->mtime;
}

/* ── RPC reply header builder ─────────────────────────────────── */

void nfs_rpc_build_reply_header(uint8_t **p, uint32_t xid,
                                 uint32_t accept_stat)
{
    nfs_xdr_put_u32(p, xid);           /* xid */
    nfs_xdr_put_u32(p, RPC_REPLY);      /* msg_type */
    nfs_xdr_put_u32(p, RPC_MSG_ACCEPTED); /* reply_stat */
    nfs_xdr_put_u32(p, RPC_AUTH_NONE);   /* verifier flavor */
    nfs_xdr_put_u32(p, 0);               /* verifier length */
    nfs_xdr_put_u32(p, accept_stat);     /* accept_stat */
}

/* ── RPC reply sanity check ───────────────────────────────────── */

int nfs_rpc_check_reply(const uint8_t *reply, uint32_t reply_len)
{
    const uint8_t *p = reply;
    uint32_t remaining = reply_len;

    /* Need at least xid(4) + msg_type(4) = 8 bytes */
    if (remaining < 8)
        return -EIO;

    /* Skip xid */
    (void)nfs_xdr_get_u32(&p);
    remaining -= 4;

    uint32_t msg_type = nfs_xdr_get_u32(&p);
    remaining -= 4;
    if (msg_type != RPC_REPLY)
        return -EIO;

    /* Need reply_stat(4) + verifier_flavor(4) + verifier_len(4) */
    if (remaining < 12)
        return -EIO;

    uint32_t reply_stat = nfs_xdr_get_u32(&p);
    remaining -= 4;
    if (reply_stat == RPC_MSG_DENIED)
        return -EACCES;

    uint32_t verf_flavor = nfs_xdr_get_u32(&p);
    (void)verf_flavor;
    remaining -= 4;

    uint32_t verf_len = nfs_xdr_get_u32(&p);
    remaining -= 4;

    /* Skip verifier body */
    if (verf_len > remaining)
        return -EIO;
    p += verf_len;
    remaining -= verf_len;
    while (verf_len & 3) { p++; remaining--; verf_len++; }

    /* Need accept_stat(4) */
    if (remaining < 4)
        return -EIO;

    uint32_t accept_stat = nfs_xdr_get_u32(&p);
    remaining -= 4;
    if (accept_stat != RPC_SUCCESS)
        return -EIO;

    /* Return number of remaining bytes after header */
    return (int)remaining;
}

/* ── MOUNT protocol helpers ───────────────────────────────────── */

/* Build MOUNTPROC3_MNT arguments: just the export path string */
uint32_t nfs_build_mnt_args(uint8_t *buf, uint32_t bufsize,
                             const char *export_path)
{
    uint8_t *p = buf;
    uint8_t *end = buf + bufsize;

    nfs_xdr_put_string(&p, export_path);
    if ((uint32_t)(p - buf) > bufsize)
        return 0;
    (void)end;
    return (uint32_t)(p - buf);
}

/* Parse MOUNTPROC3_MNT reply, extracting file handle and auth flavors.
 * Returns 0 on success, negative errno on failure. */
int nfs_parse_mnt_reply(const uint8_t *reply, uint32_t reply_len,
                         struct nfs3_fh *fh, uint32_t *auth_flavors,
                         uint32_t *num_auth_flavors)
{
    const uint8_t *p = reply;
    uint32_t remaining = reply_len;

    /* Need at least status(4) */
    if (remaining < 4)
        return -EIO;

    uint32_t status = nfs_xdr_get_u32(&p);
    remaining -= 4;

    if (status == NFS3ERR_NOENT)
        return -ENOENT;
    if (status == NFS3ERR_ACCES)
        return -EACCES;
    if (status != NFS3_OK)
        return -EIO;

    /* Read file handle */
    if (remaining < 4)
        return -EIO;
    fh->len = nfs_xdr_get_u32(&p);
    remaining -= 4;
    if (fh->len > NFS_MAX_FH_SIZE)
        fh->len = NFS_MAX_FH_SIZE;
    if (fh->len > remaining)
        return -EIO;
    nfs_xdr_get_bytes(&p, fh->data, fh->len);
    remaining -= fh->len;
    while (fh->len & 3) { p++; remaining--; fh->len++; }

    /* Read auth flavor list */
    if (remaining < 4) {
        *num_auth_flavors = 0;
        return 0;
    }
    uint32_t nflavors = nfs_xdr_get_u32(&p);
    if (nflavors > 16)
        nflavors = 16;
    if (num_auth_flavors)
        *num_auth_flavors = nflavors;
    for (uint32_t i = 0; i < nflavors && auth_flavors; i++) {
        if (remaining >= 4) {
            auth_flavors[i] = nfs_xdr_get_u32(&p);
            remaining -= 4;
        }
    }

    return 0;
}

/* ── NFS3 error translation ───────────────────────────────────── */

int nfs3err_to_errno(uint32_t nfs3_status)
{
    switch (nfs3_status) {
    case NFS3_OK:            return 0;
    case NFS3ERR_PERM:       return -EPERM;
    case NFS3ERR_NOENT:      return -ENOENT;
    case NFS3ERR_IO:         return -EIO;
    case NFS3ERR_NXIO:       return -ENXIO;
    case NFS3ERR_ACCES:      return -EACCES;
    case NFS3ERR_EXIST:      return -EEXIST;
    case NFS3ERR_NODEV:      return -ENODEV;
    case NFS3ERR_NOTDIR:     return -ENOTDIR;
    case NFS3ERR_ISDIR:      return -EISDIR;
    case NFS3ERR_FBIG:       return -EFBIG;
    case NFS3ERR_NOSPC:      return -ENOSPC;
    case NFS3ERR_ROFS:       return -EROFS;
    case NFS3ERR_NAMETOOLONG: return -ENAMETOOLONG;
    case NFS3ERR_NOTEMPTY:   return -ENOTEMPTY;
    case NFS3ERR_DQUOT:      return -EDQUOT;
    case NFS3ERR_STALE:      return -ESTALE;
    case NFS3ERR_BADHANDLE:  return -ESTALE;
    case NFS3ERR_SERVERFAULT: return -EREMOTEIO;
    default:                 return -EIO;
    }
}

/* ── Module info (for when compiled as module) ────────────────── */
#ifdef MODULE
#include "module.h"
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Shared NFSv3 procedure infrastructure");
MODULE_VERSION("1.0");
#endif
