#ifndef NFSD_H
#define NFSD_H

#include "types.h"
#include "vfs.h"
#include "errno.h"

/* NFS server constants */
#define NFSD_MAX_EXPORTS    16
#define NFSD_MAX_DATA       8192
#define NFSD_MAX_FH_SIZE    64
#define NFSD_MAX_PATH       1024
#define NFSD_PORT           2049
#define NFSD_MOUNT_PORT     1049

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

/* NFSv3 program and version */
#define NFS3_PROGRAM      100003
#define NFS3_VERSION      3

/* MOUNT protocol program and version */
#define MOUNT3_PROGRAM    100005
#define MOUNT3_VERSION    3

/* NFSv3 procedures */
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
#define NFSPROC3_FSSTAT   17
#define NFSPROC3_FSINFO   18
#define NFSPROC3_PATHCONF 19
#define NFSPROC3_COMMIT   21

/* MOUNT procedures */
#define MOUNTPROC3_NULL    0
#define MOUNTPROC3_MNT     1
#define MOUNTPROC3_DUMP    2
#define MOUNTPROC3_UMNT    3
#define MOUNTPROC3_UMNTALL 4
#define MOUNTPROC3_EXPORT  5

/* NFS3 status codes */
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

/* NFSv3 file handle (opaque) */
struct nfs3_fhandle {
    uint32_t len;
    uint8_t  data[NFSD_MAX_FH_SIZE];
};

/* Export entry — maps an NFS path to a local directory */
struct nfsd_export {
    int    valid;
    char   export_path[NFSD_MAX_PATH]; /* NFS export name, e.g. "/export" */
    char   local_path[NFSD_MAX_PATH];  /* local directory path */
    struct vfs_stat st;                /* cached stat of root */
};

/* NFSD RPC state (per TCP connection) */
struct nfsd_rpc_state {
    uint32_t xid;
    uint32_t program;
    uint32_t version;
    uint32_t procedure;
    uint8_t  auth_flavor;
    /* Call data */
    uint8_t  call_data[NFSD_MAX_DATA];
    uint32_t call_len;
};

/* Public API */
int  nfsd_add_export(const char *export_path, const char *local_path);
int  nfsd_remove_export(const char *export_path);
void nfsd_handle_rpc(struct nfsd_rpc_state *rpc,
                     uint8_t *reply_buf, uint32_t *reply_len);
void nfsd_server_task(void);
int  nfsd_init(void);

#endif /* NFSD_H */
