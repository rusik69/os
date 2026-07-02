#ifndef NFS_PROC_H
#define NFS_PROC_H

#include "types.h"
#include "vfs.h"

/* ── NFSv3 protocol constants ─────────────────────────────────── */

#define NFS3_PROGRAM      100003
#define NFS3_VERSION      3
#define MOUNT3_PROGRAM    100005
#define MOUNT3_VERSION    3

#define NFSPROC3_NULL      0
#define NFSPROC3_GETATTR   1
#define NFSPROC3_SETATTR   2
#define NFSPROC3_LOOKUP    3
#define NFSPROC3_ACCESS    4
#define NFSPROC3_READLINK  5
#define NFSPROC3_READ      6
#define NFSPROC3_WRITE     7
#define NFSPROC3_CREATE    9
#define NFSPROC3_REMOVE   10
#define NFSPROC3_RENAME   11
#define NFSPROC3_LINK     12
#define NFSPROC3_SYMLINK  13
#define NFSPROC3_MKDIR    14
#define NFSPROC3_RMDIR    15
#define NFSPROC3_READDIR  16
#define NFSPROC3_READDIRPLUS 17
#define NFSPROC3_FSSTAT   18
#define NFSPROC3_FSINFO   19
#define NFSPROC3_PATHCONF 20
#define NFSPROC3_COMMIT   21

/* MOUNT procedures */
#define MOUNTPROC3_NULL    0
#define MOUNTPROC3_MNT     1
#define MOUNTPROC3_DUMP    2
#define MOUNTPROC3_UMNT    3
#define MOUNTPROC3_UMNTALL 4
#define MOUNTPROC3_EXPORT  5

/* NFS3 status codes (RFC 1813) */
#define NFS3_OK            0
#define NFS3ERR_PERM       1
#define NFS3ERR_NOENT      2
#define NFS3ERR_IO         5
#define NFS3ERR_NXIO       6
#define NFS3ERR_ACCES      13
#define NFS3ERR_EXIST      17
#define NFS3ERR_NODEV      19
#define NFS3ERR_NOTDIR     20
#define NFS3ERR_ISDIR      21
#define NFS3ERR_FBIG       27
#define NFS3ERR_NOSPC      28
#define NFS3ERR_ROFS       30
#define NFS3ERR_NAMETOOLONG 63
#define NFS3ERR_NOTEMPTY   66
#define NFS3ERR_DQUOT      69
#define NFS3ERR_STALE      70
#define NFS3ERR_BADHANDLE  10001
#define NFS3ERR_SERVERFAULT 10006

/* NFS3 file type values */
#define NFS3_FTYPE_REG   1
#define NFS3_FTYPE_DIR   2
#define NFS3_FTYPE_LNK   5

/* RPC protocol constants */
#define RPC_CALL      0
#define RPC_REPLY     1
#define RPC_MSG_ACCEPTED 0
#define RPC_MSG_DENIED   1
#define RPC_AUTH_NONE    0
#define RPC_AUTH_SYS     1
#define RPC_AUTH_NULL    0
#define RPC_SUCCESS      0
#define RPC_PROG_UNAVAIL 1
#define RPC_PROG_MISMATCH 2
#define RPC_PROC_UNAVAIL 3
#define RPC_GARBAGE_ARGS 4
#define RPC_SYSTEM_ERR   5

#define NFS_PORT      2049
#define MOUNT_PORT    635
#define NFS_MAX_PATH  1024
#define NFS_MAX_DATA  8192
#define NFS_MAX_FH_SIZE 64

/* ── NFSv3 data structures ────────────────────────────────────── */

/* NFSv3 file handle (variable-length, opaque) */
struct nfs3_fh {
    uint32_t len;
    uint8_t  data[NFS_MAX_FH_SIZE];
};

/* NFSv3 file attributes (w3_fattr) as encoded on the wire */
struct nfs3_fattr {
    uint32_t type;       /* NFS3_FTYPE_* */
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t used;
    uint32_t rdev_specdata1;
    uint32_t rdev_specdata2;
    uint64_t fsid;
    uint64_t fileid;
    uint64_t atime;
    uint32_t atime_nsec;
    uint64_t mtime;
    uint32_t mtime_nsec;
    uint64_t ctime;
    uint32_t ctime_nsec;
};

/* NFSv3 diropargs3 (directory + filename) */
struct nfs3_diropargs {
    struct nfs3_fh dir;
    char    name[256];
};

/* NFSv3 sattr3 (set file attributes) */
struct nfs3_sattr {
    uint32_t set_mode;
    uint32_t mode;
    uint32_t set_uid;
    uint32_t uid;
    uint32_t set_gid;
    uint32_t gid;
    uint32_t set_size;
    uint64_t size;
    uint32_t set_atime;
    uint32_t atime_how;  /* 0=DONT_CHANGE, 1=SET_TO_SERVER, 2=SET_TO_CLIENT */
    uint64_t atime;
    uint32_t atime_nsec;
    uint32_t set_mtime;
    uint32_t mtime_how;
    uint64_t mtime;
    uint32_t mtime_nsec;
};

/* ── XDR marshaling API ────────────────────────────────────────── */

/* Put/Get big-endian 32-bit */
void nfs_xdr_put_u32(uint8_t **p, uint32_t v);
uint32_t nfs_xdr_get_u32(const uint8_t **p);

/* Put/Get raw bytes with 4-byte alignment padding */
void nfs_xdr_put_bytes(uint8_t **p, const void *data, uint32_t len);
void nfs_xdr_get_bytes(const uint8_t **p, void *dst, uint32_t len);

/* Put/Get NUL-terminated string (XDR string: length + data + padding) */
void nfs_xdr_put_string(uint8_t **p, const char *s);
uint32_t nfs_xdr_get_string(const uint8_t **p, char *buf, uint32_t maxlen);

/* Put/Get 64-bit integer (two 32-bit big-endian words) */
void nfs_xdr_put_uint64(uint8_t **p, uint64_t v);
uint64_t nfs_xdr_get_uint64(const uint8_t **p);

/* NFSv3 file handle marshaling */
void nfs_xdr_put_fh(uint8_t **p, const struct nfs3_fh *fh);
void nfs_xdr_get_fh(const uint8_t **p, struct nfs3_fh *fh);

/* NFSv3 file attributes marshaling */
void nfs_xdr_encode_fattr(uint8_t **p, const struct nfs3_fattr *fa);
void nfs_xdr_decode_fattr(const uint8_t **p, struct nfs3_fattr *fa);

/* NFSv3 fattr from vfs_stat */
void nfs3_fattr_from_vfs_stat(struct nfs3_fattr *fa, const struct vfs_stat *st);

/* RPC header building */
void nfs_rpc_build_reply_header(uint8_t **p, uint32_t xid,
                                 uint32_t accept_stat);

/* RPC reply sanity check */
int nfs_rpc_check_reply(const uint8_t *reply, uint32_t reply_len);

/* Build MOUNTPROC3_MNT arguments (export path) */
uint32_t nfs_build_mnt_args(uint8_t *buf, uint32_t bufsize,
                             const char *export_path);

/* Parse MOUNTPROC3_MNT reply, extract file handle and auth flavors */
int nfs_parse_mnt_reply(const uint8_t *reply, uint32_t reply_len,
                         struct nfs3_fh *fh, uint32_t *auth_flavors,
                         uint32_t *num_auth_flavors);

/* ── Error translation ─────────────────────────────────────────── */

/* Convert NFS3 status code to errno */
int nfs3err_to_errno(uint32_t nfs3_status);

#endif /* NFS_PROC_H */
