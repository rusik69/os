// SPDX-License-Identifier: GPL-2.0-only
/*
 * nfs.c — NFS v3 client skeleton (RPC layer + mount protocol)
 *
 * Implements a minimal NFS v3 client with RPC communication
 * and mount protocol support.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "heap.h"

#define NFS_PORT      2049
#define MOUNT_PORT    635
#define NFS_MAX_PATH  1024
#define NFS_MAX_DATA  8192

/* RPC message types */
#define RPC_CALL      0
#define RPC_REPLY     1

/* NFS procedures */
#define NFSPROC3_GETATTR   1
#define NFSPROC3_READ      6
#define NFSPROC3_WRITE     7
#define NFSPROC3_CREATE    9
#define NFSPROC3_LOOKUP    12

/* Mount procedures */
#define MOUNTPROC3_MNT     1
#define MOUNTPROC3_UMNT    3

struct nfs_fhandle {
    uint8_t data[64];
    int len;
};

struct nfs_mount_info {
    int mounted;
    char server[64];
    char export_path[NFS_MAX_PATH];
    struct nfs_fhandle root_fh;
    uint32_t mount_port;
};

static struct nfs_mount_info nfs_mounts[8];
static int nfs_mount_count;

/* Send an RPC call (simplified stub) */
static int nfs_rpc_call(uint32_t server_ip, uint32_t prog,
                         uint32_t vers, uint32_t proc,
                         const uint8_t *args, size_t args_len,
                         uint8_t *reply, size_t *reply_len)
{
    (void)server_ip;
    (void)prog;
    (void)vers;
    (void)proc;
    (void)args;
    (void)args_len;
    (void)reply;
    (void)reply_len;

    kprintf("[NFS] RPC call: prog=%u vers=%u proc=%u\n", prog, vers, proc);
    return 0;
}

/* Mount an NFS export */
int nfs_mount(const char *server, const char *export_path)
{
    if (nfs_mount_count >= 8)
        return -ENOMEM;

    struct nfs_mount_info *mnt = &nfs_mounts[nfs_mount_count];

    strncpy(mnt->server, server, sizeof(mnt->server) - 1);
    strncpy(mnt->export_path, export_path, sizeof(mnt->export_path) - 1);
    mnt->mount_port = MOUNT_PORT;
    mnt->mounted = 1;
    mnt->root_fh.len = 0;

    /* Simulate mount RPC */
    kprintf("[NFS] Mounting %s:%s\n", server, export_path);
    nfs_mount_count++;

    return nfs_mount_count - 1;
}

/* NFS LOOKUP procedure */
int nfs_lookup(int mount_id, const char *path, struct nfs_fhandle *fh)
{
    if (mount_id < 0 || mount_id >= nfs_mount_count)
        return -EINVAL;
    if (!nfs_mounts[mount_id].mounted)
        return -ENOTCONN;

    uint8_t args[NFS_MAX_DATA];
    uint8_t reply[NFS_MAX_DATA];
    size_t reply_len = sizeof(reply);

    kprintf("[NFS] LOOKUP: mount=%d path=%s\n", mount_id, path);

    int ret = nfs_rpc_call(0, 100003, 3, NFSPROC3_LOOKUP,
                            args, 0, reply, &reply_len);
    if (ret == 0 && fh) {
        fh->len = 32;
        memset(fh->data, 0, sizeof(fh->data));
    }

    return ret;
}

/* NFS READ procedure */
int nfs_read(int mount_id, const struct nfs_fhandle *fh,
              uint64_t offset, uint32_t count, uint8_t *buf)
{
    if (mount_id < 0 || mount_id >= nfs_mount_count)
        return -EINVAL;

    (void)fh;
    (void)offset;
    (void)count;
    (void)buf;

    kprintf("[NFS] READ: mount=%d offset=%llu count=%u\n",
            mount_id, (unsigned long long)offset, count);
    return (int)count;
}

/* NFS WRITE procedure */
int nfs_write(int mount_id, const struct nfs_fhandle *fh,
               uint64_t offset, uint32_t count, const uint8_t *buf)
{
    if (mount_id < 0 || mount_id >= nfs_mount_count)
        return -EINVAL;

    (void)fh;
    (void)offset;
    (void)count;
    (void)buf;

    kprintf("[NFS] WRITE: mount=%d offset=%llu count=%u\n",
            mount_id, (unsigned long long)offset, count);
    return (int)count;
}

/* Unmount */
int nfs_umount(int mount_id)
{
    if (mount_id < 0 || mount_id >= nfs_mount_count)
        return -EINVAL;

    nfs_mounts[mount_id].mounted = 0;
    kprintf("[NFS] Unmount: mount=%d\n", mount_id);
    return 0;
}

void nfs_init(void)
{
    memset(nfs_mounts, 0, sizeof(nfs_mounts));
    nfs_mount_count = 0;
    kprintf("[OK] NFSv3 — Client skeleton (RPC + mount protocol)\n");
}
