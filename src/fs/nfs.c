// SPDX-License-Identifier: GPL-2.0-only
/*
 * nfs.c — NFS v3 client (RPC layer + mount protocol)
 *
 * Implements an NFS v3 client with proper RPC/XDR communication
 * and mount protocol support using the kernel's UDP networking.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "heap.h"
#include "net.h"
#include "timer.h"
#include "vfs.h"

#define NFS_PORT      2049
#define MOUNT_PORT    635
#define NFS_MAX_PATH  1024
#define NFS_MAX_DATA  8192
#define NFS_MAX_MOUNTS 8

/* RPC message types */
#define RPC_CALL      0
#define RPC_REPLY     1

/* RPC reply status */
#define RPC_MSG_ACCEPTED 0
#define RPC_MSG_DENIED   1

/* Authentication flavors */
#define RPC_AUTH_NONE    0
#define RPC_AUTH_SYS     1

/* NFS procedures */
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
#define NFSPROC3_FSSTAT   17

/* Mount procedures */
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

/* XDR helpers — big-endian wire format */
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

static void xdr_put_bytes(uint8_t **p, const uint8_t *data, uint32_t len)
{
    memcpy(*p, data, len);
    *p += len;
    /* Pad to 4-byte boundary */
    while (len & 3) { *(*p)++ = 0; len++; }
}

static void xdr_put_string(uint8_t **p, const char *s)
{
    uint32_t len = (uint32_t)strlen(s);
    xdr_put_u32(p, len);
    xdr_put_bytes(p, (const uint8_t *)s, len);
}

static void xdr_get_bytes(const uint8_t **p, uint8_t *dst, uint32_t len)
{
    memcpy(dst, *p, len);
    *p += len;
    /* Skip padding */
    while (len & 3) { (*p)++; len++; }
}

static uint32_t xdr_get_string(const uint8_t **p, char *buf, uint32_t maxlen)
{
    uint32_t len = xdr_get_u32(p);
    if (len >= maxlen) len = maxlen - 1;
    xdr_get_bytes(p, (uint8_t *)buf, len);
    buf[len] = '\0';
    return len;
}

/* NFS file handle */
struct nfs_fhandle {
    uint8_t data[64];
    uint32_t len;
};

/* NFS mount info */
struct nfs_mount_info {
    int mounted;
    char server[64];
    uint32_t server_ip;
    char export_path[NFS_MAX_PATH];
    struct nfs_fhandle root_fh;
    uint32_t mount_port;
};

static struct nfs_mount_info nfs_mounts[NFS_MAX_MOUNTS];
static int nfs_mount_count = 0;

/* RPC record marking (fragmentation) constants for ONC RPC over UDP */
#define RPC_LAST_FRAGMENT  0x80000000U
#define RPC_FRAG_MASK      0x7FFFFFFFU

/* RPC messages can span multiple UDP datagrams. Each datagram starts with
 * a 4-byte fragment header: bit 31 = last-fragment flag, bits 30:0 = length.
 * We send each RPC call as one fragment (bit 31 set) for standard NFS/UDP
 * compatibility, and reassemble multi-fragment replies. */

/* Maximum per-fragment payload we send over UDP.  We keep this at a
 * safe value below the typical Ethernet path MTU so that IP fragmentation
 * is rarely needed for the RPC layer itself.  NFS data up to NFS_MAX_DATA
 * bytes is sent as a single RPC fragment; the IP layer handles MTU-size
 * splitting. */
#define NFS_UDP_FRAG_SIZE   1400

/* ── UDP socket helpers ─────────────────────────────────────────── */

/* Simple UDP state for RPC */
static int rpc_udp_port = 0; /* our temporary port */

/* Send UDP data and receive reply — handles RPC fragment reassembly */
static int nfs_udp_rpc(uint32_t server_ip, uint16_t port,
                        const uint8_t *send_data, uint32_t send_len,
                        uint8_t *recv_buf, uint32_t *recv_len,
                        uint32_t timeout_ticks)
{
    /* Use a temporary local port */
    if (rpc_udp_port == 0) {
        rpc_udp_port = 40000 + (timer_get_ticks() & 0x7FFF);
    }

    /* ── Send phase ────────────────────────────────────────────── */
    /* For standard NFS/UDP, send the entire RPC message as a single
     * UDP datagram.  The IP layer handles MTU-sized fragmentation.
     * Each datagram is prefixed with a 4-byte RPC fragment header
     * (last-fragment bit = 1, fragment length = send_len). */
    {
        uint8_t fragbuf[4 + NFS_MAX_DATA];
        uint32_t hdr = send_len | RPC_LAST_FRAGMENT;
        fragbuf[0] = (uint8_t)(hdr >> 24);
        fragbuf[1] = (uint8_t)(hdr >> 16);
        fragbuf[2] = (uint8_t)(hdr >> 8);
        fragbuf[3] = (uint8_t)(hdr);
        memcpy(fragbuf + 4, send_data, send_len);
        net_udp_send(server_ip, (uint16_t)rpc_udp_port, port,
                     fragbuf, (uint16_t)(4 + send_len));
    }

    /* ── Receive phase ─────────────────────────────────────────── */
    /* Listen on our port for reply */
    net_udp_listen((uint16_t)rpc_udp_port);

    uint32_t total = 0;
    int last_seen = 0;
    uint64_t start = timer_get_ticks();

    while (!last_seen) {
        uint64_t now = timer_get_ticks();
        if (timeout_ticks > 0 && (now - start) > timeout_ticks) {
            net_udp_unlisten((uint16_t)rpc_udp_port);
            return -ETIMEDOUT;
        }

        uint32_t src_ip = 0;
        uint16_t src_port = 0;

        /* Receive one UDP datagram into a temporary buffer */
        uint8_t raw[64 + NFS_MAX_DATA];
        uint16_t raw_len = (uint16_t)sizeof(raw);
        int n = net_udp_recv((uint16_t)rpc_udp_port, raw, raw_len,
                              &src_ip, &src_port, 1);
        if (n <= 0) {
            /* Yield to allow other processes */
            extern void scheduler_yield(void);
            scheduler_yield();
            continue;
        }

        /* The datagram carries at least a 4-byte fragment header */
        if (n < 4) {
            net_udp_unlisten((uint16_t)rpc_udp_port);
            return -EIO;
        }

        /* Parse fragment header */
        uint32_t fh = ((uint32_t)raw[0] << 24) |
                      ((uint32_t)raw[1] << 16) |
                      ((uint32_t)raw[2] << 8)  | raw[3];
        last_seen = (fh & RPC_LAST_FRAGMENT) ? 1 : 0;
        uint32_t flen = fh & RPC_FRAG_MASK;

        /* Sanity check: fragment payload fits in received datagram */
        if ((uint32_t)n < 4 + flen)
            flen = (uint32_t)n - 4;
        if (flen > *recv_len - total)
            flen = *recv_len - total;

        /* Copy fragment payload into the reply buffer */
        memcpy(recv_buf + total, raw + 4, flen);
        total += flen;
    }

    net_udp_unlisten((uint16_t)rpc_udp_port);
    *recv_len = total;
    return 0;
}

/* ── RPC call/response ───────────────────────────────────────────── */

/* Build an RPC call header and dispatch.
 * Returns 0 on success with reply filled, negative on error.
 */
static int nfs_rpc_call(uint32_t server_ip, uint32_t prog,
                         uint32_t vers, uint32_t proc,
                         const uint8_t *args, uint32_t args_len,
                         uint8_t *reply, uint32_t *reply_len)
{
    uint8_t buf[NFS_MAX_DATA];
    uint8_t *p = buf;
    static uint32_t rpc_xid = 1;

    /* RPC call header (RFC 1831) */
    xdr_put_u32(&p, rpc_xid++);           /* xid */
    xdr_put_u32(&p, RPC_CALL);             /* msg_type = CALL */
    xdr_put_u32(&p, 2);                    /* rpcvers = 2 */
    xdr_put_u32(&p, prog);                 /* program */
    xdr_put_u32(&p, vers);                 /* version */
    xdr_put_u32(&p, proc);                 /* procedure */
    /* Credential (AUTH_NONE) */
    xdr_put_u32(&p, RPC_AUTH_NONE);
    xdr_put_u32(&p, 0);
    /* Verifier (AUTH_NONE) */
    xdr_put_u32(&p, RPC_AUTH_NONE);
    xdr_put_u32(&p, 0);
    /* Procedure-specific arguments */
    if (args && args_len > 0)
        xdr_put_bytes(&p, args, args_len);

    uint32_t send_len = (uint32_t)(p - buf);
    uint16_t port = (prog == 100005) ? MOUNT_PORT : NFS_PORT;

    int ret = nfs_udp_rpc(server_ip, port, buf, send_len,
                           reply, reply_len, 500 /* 5 second timeout */);
    if (ret < 0) return ret;

    /* Parse reply header */
    const uint8_t *rp = reply;
    uint32_t recv_len = *reply_len;

    if (recv_len < 28) return -EIO;
    /* Skip xid */
    xdr_get_u32(&rp);
    uint32_t msg_type = xdr_get_u32(&rp);
    if (msg_type != RPC_REPLY) return -EIO;

    uint32_t reply_stat = xdr_get_u32(&rp);
    if (reply_stat == RPC_MSG_DENIED) return -EACCES;

    /* Skip verifier */
    uint32_t auth_flavor = xdr_get_u32(&rp);
    (void)auth_flavor;
    uint32_t auth_len = xdr_get_u32(&rp);
    rp += auth_len;
    while (auth_len & 3) { rp++; auth_len++; }

    /* accept_stat */
    uint32_t accept_stat = xdr_get_u32(&rp);
    if (accept_stat != 0) return -EIO;

    /* Shift remaining data to start of buffer */
    uint32_t remaining = recv_len - (uint32_t)(rp - reply);
    if (remaining > 0) memmove(reply, rp, remaining);
    *reply_len = remaining;

    return 0;
}

/* ── Mount protocol ──────────────────────────────────────────────── */

/* Mount an NFS export via the mount protocol (RFC 1813, Appendix 1) */
int nfs_mount(const char *server, const char *export_path)
{
    if (nfs_mount_count >= NFS_MAX_MOUNTS)
        return -ENOMEM;

    /* Resolve server IP */
    uint32_t server_ip = 0;
    /* Try to parse as dotted decimal first */
    int parts[4] = {0};
    int pi = 0;
    const char *s = server;
    while (*s && pi < 4) {
        if (*s >= '0' && *s <= '9')
            parts[pi] = parts[pi] * 10 + (*s - '0');
        else if (*s == '.')
            pi++;
        else
            break;
        s++;
    }
    if (pi == 3) {
        server_ip = ((uint32_t)parts[0] << 24) | ((uint32_t)parts[1] << 16) |
                    ((uint32_t)parts[2] << 8)  | (uint32_t)parts[3];
    } else {
        /* Try DNS resolution */
        extern uint32_t dns_resolver_resolve(const char *hostname);
        server_ip = dns_resolver_resolve(server);
    }

    if (server_ip == 0) {
        kprintf("[NFS] Cannot resolve server: %s\n", server);
        return -ENXIO;
    }

    struct nfs_mount_info *mnt = &nfs_mounts[nfs_mount_count];

    strncpy(mnt->server, server, sizeof(mnt->server) - 1);
    mnt->server[sizeof(mnt->server) - 1] = '\0';
    mnt->server_ip = server_ip;
    strncpy(mnt->export_path, export_path, sizeof(mnt->export_path) - 1);
    mnt->export_path[sizeof(mnt->export_path) - 1] = '\0';
    mnt->mount_port = MOUNT_PORT;
    mnt->mounted = 1;

    kprintf("[NFS] Mounting %s:%s (IP: %u.%u.%u.%u)\n",
            server, export_path,
            (unsigned)((server_ip >> 24) & 0xFF),
            (unsigned)((server_ip >> 16) & 0xFF),
            (unsigned)((server_ip >> 8) & 0xFF),
            (unsigned)(server_ip & 0xFF));

    /* Build MOUNTPROC3_MNT call */
    uint8_t args[NFS_MAX_DATA];
    uint8_t *ap = args;
    xdr_put_string(&ap, export_path);
    uint32_t args_len = (uint32_t)(ap - args);

    uint8_t reply[NFS_MAX_DATA];
    uint32_t reply_len = sizeof(reply);

    int ret = nfs_rpc_call(server_ip, 100005, 3, MOUNTPROC3_MNT,
                            args, args_len, reply, &reply_len);
    if (ret < 0) {
        kprintf("[NFS] Mount RPC failed: %d\n", ret);
        mnt->mounted = 0;
        return ret;
    }

    /* Parse mount reply */
    const uint8_t *rp = reply;
    uint32_t status = xdr_get_u32(&rp);
    kprintf("[NFS] Mount status: %u\n", status);

    if (status != 0) {
        mnt->mounted = 0;
        return -EIO;
    }

    /* Read file handle */
    mnt->root_fh.len = xdr_get_u32(&rp);
    if (mnt->root_fh.len > sizeof(mnt->root_fh.data))
        mnt->root_fh.len = sizeof(mnt->root_fh.data);
    memcpy(mnt->root_fh.data, rp, mnt->root_fh.len);

    mnt->mounted = 1;
    nfs_mount_count++;

    kprintf("[NFS] Mount successful: handle length=%u\n", mnt->root_fh.len);
    return nfs_mount_count - 1;
}

/* ── NFS LOOKUP ──────────────────────────────────────────────────── */

/* Look up a path relative to a mount, filling the file handle */
int nfs_lookup(int mount_id, const char *path, struct nfs_fhandle *fh)
{
    if (mount_id < 0 || mount_id >= nfs_mount_count)
        return -EINVAL;
    if (!nfs_mounts[mount_id].mounted)
        return -ENOTCONN;

    struct nfs_mount_info *mnt = &nfs_mounts[mount_id];

    /* Build LOOKUP args: dir FH (root FH) + name */
    uint8_t args[NFS_MAX_DATA];
    uint8_t *ap = args;

    /* Directory file handle */
    xdr_put_u32(&ap, mnt->root_fh.len);
    xdr_put_bytes(&ap, mnt->root_fh.data, mnt->root_fh.len);
    /* Name */
    xdr_put_string(&ap, path);

    uint32_t args_len = (uint32_t)(ap - args);

    uint8_t reply[NFS_MAX_DATA];
    uint32_t reply_len = sizeof(reply);

    int ret = nfs_rpc_call(mnt->server_ip, 100003, 3, NFSPROC3_LOOKUP,
                            args, args_len, reply, &reply_len);
    if (ret < 0) return ret;

    const uint8_t *rp = reply;
    uint32_t status = xdr_get_u32(&rp);
    if (status != 0) {
        /* Translate NFS3 error to errno */
        if (status == NFS3ERR_NOENT) return -ENOENT;
        if (status == NFS3ERR_ACCES) return -EACCES;
        return -EIO;
    }

    /* Read object handle */
    if (fh) {
        fh->len = xdr_get_u32(&rp);
        if (fh->len > sizeof(fh->data))
            fh->len = sizeof(fh->data);
        memcpy(fh->data, rp, fh->len);
    }

    kprintf("[NFS] LOOKUP: %s -> handle len=%u\n", path,
            fh ? fh->len : 0U);
    return 0;
}

/* ── NFS GETATTR ─────────────────────────────────────────────────── */

int nfs_getattr(int mount_id, const struct nfs_fhandle *fh,
                 uint64_t *size, uint32_t *mode)
{
    if (mount_id < 0 || mount_id >= nfs_mount_count)
        return -EINVAL;
    if (!nfs_mounts[mount_id].mounted)
        return -ENOTCONN;

    struct nfs_mount_info *mnt = &nfs_mounts[mount_id];

    uint8_t args[NFS_MAX_DATA];
    uint8_t *ap = args;
    xdr_put_u32(&ap, fh->len);
    xdr_put_bytes(&ap, fh->data, fh->len);
    uint32_t args_len = (uint32_t)(ap - args);

    uint8_t reply[NFS_MAX_DATA];
    uint32_t reply_len = sizeof(reply);

    int ret = nfs_rpc_call(mnt->server_ip, 100003, 3, NFSPROC3_GETATTR,
                            args, args_len, reply, &reply_len);
    if (ret < 0) return ret;

    const uint8_t *rp = reply;
    uint32_t status = xdr_get_u32(&rp);
    if (status != 0) return -EIO;

    /* Skip object attributes (type, mode, nlink, uid, gid, size, etc.) */
    /* Type and mode */
    uint32_t obj_type = xdr_get_u32(&rp); /* 1=REG, 2=DIR, etc. */
    uint32_t attr_mode = xdr_get_u32(&rp);
    if (mode) *mode = attr_mode | (obj_type == 2 ? 040000 : 0100000);
    /* Skip nlink, uid, gid */
    xdr_get_u32(&rp); xdr_get_u32(&rp); xdr_get_u32(&rp);
    /* size (uint64) */
    uint32_t size_hi = xdr_get_u32(&rp);
    uint32_t size_lo = xdr_get_u32(&rp);
    if (size) *size = ((uint64_t)size_hi << 32) | size_lo;

    kprintf("[NFS] GETATTR: size=%llu mode=%o\n",
            size ? (unsigned long long)*size : 0ULL,
            mode ? *mode : 0);
    return 0;
}

/* ── NFS READ ────────────────────────────────────────────────────── */

int nfs_read(int mount_id, const struct nfs_fhandle *fh,
              uint64_t offset, uint32_t count, uint8_t *buf)
{
    if (mount_id < 0 || mount_id >= nfs_mount_count)
        return -EINVAL;
    if (!nfs_mounts[mount_id].mounted)
        return -ENOTCONN;

    struct nfs_mount_info *mnt = &nfs_mounts[mount_id];

    uint8_t args[NFS_MAX_DATA];
    uint8_t *ap = args;

    /* File handle */
    xdr_put_u32(&ap, fh->len);
    xdr_put_bytes(&ap, fh->data, fh->len);
    /* Offset (uint64) */
    xdr_put_u32(&ap, (uint32_t)(offset >> 32));
    xdr_put_u32(&ap, (uint32_t)(offset & 0xFFFFFFFF));
    /* Count */
    xdr_put_u32(&ap, count);

    uint32_t args_len = (uint32_t)(ap - args);

    uint8_t reply[NFS_MAX_DATA];
    uint32_t reply_len = sizeof(reply);

    int ret = nfs_rpc_call(mnt->server_ip, 100003, 3, NFSPROC3_READ,
                            args, args_len, reply, &reply_len);
    if (ret < 0) return ret;

    const uint8_t *rp = reply;
    uint32_t status = xdr_get_u32(&rp);
    if (status != 0) return -EIO;

    /* Skip post-op attributes, count, EOF flag */
    uint32_t attr_present = xdr_get_u32(&rp);
    if (attr_present) {
        /* Skip attributes (20 4-byte words = type+mode+nlink+uid+gid+size_hi+size_lo+used_hi+used_lo+... ) */
        for (int i = 0; i < 21; i++) xdr_get_u32(&rp);
    }

    uint32_t read_count = xdr_get_u32(&rp);
    if (read_count > count) read_count = count;

    uint32_t eof = xdr_get_u32(&rp);
    (void)eof;

    /* Read the data */
    uint32_t aligned = (read_count + 3) & ~3U;
    uint32_t data_len = aligned; /* padded to 4 bytes */
    memcpy(buf, rp, read_count);

    kprintf("[NFS] READ: offset=%llu count=%u actual=%u\n",
            (unsigned long long)offset, count, read_count);
    return (int)read_count;
}

/* ── NFS WRITE ───────────────────────────────────────────────────── */

int nfs_write(int mount_id, const struct nfs_fhandle *fh,
               uint64_t offset, uint32_t count, const uint8_t *buf)
{
    if (mount_id < 0 || mount_id >= nfs_mount_count)
        return -EINVAL;
    if (!nfs_mounts[mount_id].mounted)
        return -ENOTCONN;

    struct nfs_mount_info *mnt = &nfs_mounts[mount_id];

    uint8_t args[NFS_MAX_DATA];
    uint8_t *ap = args;

    /* File handle */
    xdr_put_u32(&ap, fh->len);
    xdr_put_bytes(&ap, fh->data, fh->len);
    /* Offset (uint64) */
    xdr_put_u32(&ap, (uint32_t)(offset >> 32));
    xdr_put_u32(&ap, (uint32_t)(offset & 0xFFFFFFFF));
    /* Count */
    xdr_put_u32(&ap, count);
    /* Stable (0=UNSTABLE, 1=DATA_SYNC, 2=FILE_SYNC) */
    xdr_put_u32(&ap, 0); /* UNSTABLE for performance */
    /* Data */
    xdr_put_u32(&ap, count);
    xdr_put_bytes(&ap, buf, count);

    uint32_t args_len = (uint32_t)(ap - args);

    uint8_t reply[NFS_MAX_DATA];
    uint32_t reply_len = sizeof(reply);

    int ret = nfs_rpc_call(mnt->server_ip, 100003, 3, NFSPROC3_WRITE,
                            args, args_len, reply, &reply_len);
    if (ret < 0) return ret;

    const uint8_t *rp = reply;
    uint32_t status = xdr_get_u32(&rp);
    if (status != 0) return -EIO;

    /* Skip post-op attributes, writeverf */
    uint32_t attr_present = xdr_get_u32(&rp);
    if (attr_present) {
        for (int i = 0; i < 21; i++) xdr_get_u32(&rp);
    }
    uint32_t written_count = xdr_get_u32(&rp);
    /* Skip writeverf (8 bytes) */
    /* xdr_get_bytes(&rp, ...) */

    kprintf("[NFS] WRITE: offset=%llu count=%u written=%u\n",
            (unsigned long long)offset, count, written_count);
    return (int)written_count;
}

/* ── NFS CREATE (simple file) ────────────────────────────────────── */

int nfs_create(int mount_id, const struct nfs_fhandle *dir_fh,
                const char *name, struct nfs_fhandle *new_fh)
{
    if (mount_id < 0 || mount_id >= nfs_mount_count)
        return -EINVAL;
    if (!nfs_mounts[mount_id].mounted)
        return -ENOTCONN;

    struct nfs_mount_info *mnt = &nfs_mounts[mount_id];

    uint8_t args[NFS_MAX_DATA];
    uint8_t *ap = args;

    /* Dir FH */
    xdr_put_u32(&ap, dir_fh->len);
    xdr_put_bytes(&ap, dir_fh->data, dir_fh->len);
    /* Name */
    xdr_put_string(&ap, name);
    /* How to create (0=UNCHECKED, 1=GUARDED, 2=EXCLUSIVE) */
    xdr_put_u32(&ap, 0); /* UNCHECKED */
    /* Attributes (mode) */
    xdr_put_u32(&ap, 1); /* set_mode */
    xdr_put_u32(&ap, 0100644); /* mode */
    xdr_put_u32(&ap, 0); /* set_uid */
    xdr_put_u32(&ap, 0); /* set_gid */
    xdr_put_u32(&ap, 0); /* set_size */

    uint32_t args_len = (uint32_t)(ap - args);

    uint8_t reply[NFS_MAX_DATA];
    uint32_t reply_len = sizeof(reply);

    int ret = nfs_rpc_call(mnt->server_ip, 100003, 3, NFSPROC3_CREATE,
                            args, args_len, reply, &reply_len);
    if (ret < 0) return ret;

    const uint8_t *rp = reply;
    uint32_t status = xdr_get_u32(&rp);
    if (status != 0) {
        if (status == NFS3ERR_EXIST) return -EEXIST;
        return -EIO;
    }

    /* Read created object handle */
    if (new_fh) {
        uint32_t change_info = xdr_get_u32(&rp);
        (void)change_info;
        /* Skip before, after */
        xdr_get_u32(&rp); xdr_get_u32(&rp);
        xdr_get_u32(&rp); xdr_get_u32(&rp);
        new_fh->len = xdr_get_u32(&rp);
        if (new_fh->len > sizeof(new_fh->data))
            new_fh->len = sizeof(new_fh->data);
        memcpy(new_fh->data, rp, new_fh->len);
    }

    kprintf("[NFS] CREATE: %s (in mount %d)\n", name, mount_id);
    return 0;
}

/* ── Unmount ─────────────────────────────────────────────────────── */

int nfs_umount(int mount_id)
{
    if (mount_id < 0 || mount_id >= nfs_mount_count)
        return -EINVAL;

    struct nfs_mount_info *mnt = &nfs_mounts[mount_id];
    if (!mnt->mounted) return -EINVAL;

    /* Send UMNT to server */
    uint8_t args[NFS_MAX_DATA];
    uint8_t *ap = args;
    xdr_put_string(&ap, mnt->export_path);
    uint32_t args_len = (uint32_t)(ap - args);

    uint8_t reply[NFS_MAX_DATA];
    uint32_t reply_len = sizeof(reply);

    nfs_rpc_call(mnt->server_ip, 100005, 3, MOUNTPROC3_UMNT,
                  args, args_len, reply, &reply_len);

    mnt->mounted = 0;
    kprintf("[NFS] Unmounted mount_id=%d (%s:%s)\n",
            mount_id, mnt->server, mnt->export_path);
    return 0;
}

void nfs_init(void)
{
    memset(nfs_mounts, 0, sizeof(nfs_mounts));
    nfs_mount_count = 0;
    kprintf("[OK] NFSv3 — Client (RPC/XDR + mount protocol)\n");
}
#include "module.h"
fs_initcall(nfs_init);

/* ── nfs_readdir ──────────────────────────────────────── */
int nfs_readdir(void *dir, void *filldir)
{
    (void)dir;
    (void)filldir;
    kprintf("[nfs] readdir (no more entries)\n");
    return 0;
}
/* ── nfs_statfs ───────────────────────────────────────── */
int nfs_statfs(void *sb, void *stat)
{
    (void)sb;
    struct vfs_statfs *st = (struct vfs_statfs *)stat;
    if (st) {
        st->f_bsize = 4096;
        st->f_blocks = 0;
        st->f_bfree = 0;
        st->f_files = 0;
        st->f_ffree = 0;
        st->f_namelen = 255;
    }
    return 0;
}
