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
static int nfs_mount(const char *server, const char *export_path)
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

/* ── NFS LOOKUP (single-component helper) ───────────────────────── */

/* Perform an NFS LOOKUP RPC for a single component name relative
 * to the given directory FH. Returns 0 on success with out_fh filled. */
static int nfs_lookup_component(struct nfs_mount_info *mnt,
                                 const struct nfs_fhandle *dir_fh,
                                 const char *component,
                                 struct nfs_fhandle *out_fh)
{
	uint8_t args[NFS_MAX_DATA];
	uint8_t *ap = args;

	/* Directory file handle */
	xdr_put_u32(&ap, dir_fh->len);
	xdr_put_bytes(&ap, dir_fh->data, dir_fh->len);
	/* Component name */
	xdr_put_string(&ap, component);

	uint32_t args_len = (uint32_t)(ap - args);

	uint8_t reply[NFS_MAX_DATA];
	uint32_t reply_len = sizeof(reply);

	int ret = nfs_rpc_call(mnt->server_ip, 100003, 3, NFSPROC3_LOOKUP,
	                        args, args_len, reply, &reply_len);
	if (ret < 0)
		return ret;

	const uint8_t *rp = reply;
	uint32_t status = xdr_get_u32(&rp);
	if (status != 0) {
		if (status == NFS3ERR_NOENT)  return -ENOENT;
		if (status == NFS3ERR_ACCES)  return -EACCES;
		if (status == NFS3ERR_NOTDIR) return -ENOTDIR;
		return -EIO;
	}

	/* Read object handle */
	if (out_fh) {
		out_fh->len = xdr_get_u32(&rp);
		if (out_fh->len > sizeof(out_fh->data))
			out_fh->len = sizeof(out_fh->data);
		memcpy(out_fh->data, rp, out_fh->len);
	}

	kprintf("[NFS] LOOKUP: %s -> handle len=%u\n", component,
	        out_fh ? out_fh->len : 0U);
	return 0;
}

/* ── NFS path resolution (multi-component) ────────────────────── */

/* Resolve a multi-component path relative to a mount point by
 * iteratively looking up each path component (separated by '/').
 * Returns 0 on success with the final entry's file handle in fh. */
static int nfs_path_resolve(int mount_id, const char *path,
                      struct nfs_fhandle *fh)
{
	if (mount_id < 0 || mount_id >= nfs_mount_count)
		return -EINVAL;
	if (!nfs_mounts[mount_id].mounted)
		return -ENOTCONN;
	if (!path || !fh)
		return -EINVAL;

	struct nfs_mount_info *mnt = &nfs_mounts[mount_id];

	/* Start from root FH */
	struct nfs_fhandle current_fh;
	memcpy(&current_fh, &mnt->root_fh, sizeof(current_fh));

	/* Skip leading '/' */
	const char *p = path;
	while (*p == '/')
		p++;

	/* If path is empty or just "/", return root FH */
	if (*p == '\0') {
		memcpy(fh, &mnt->root_fh, sizeof(*fh));
		return 0;
	}

	/* Walk path components */
	char component[256];
	while (*p) {
		/* Extract next path component */
		int ci = 0;
		while (*p && *p != '/' && ci < (int)sizeof(component) - 1)
			component[ci++] = *p++;
		component[ci] = '\0';

		/* Skip '/' separator */
		while (*p == '/')
			p++;

		/* Look up this component relative to current FH */
		struct nfs_fhandle next_fh;
		int ret = nfs_lookup_component(mnt, &current_fh,
		                                component, &next_fh);
		if (ret < 0) {
			kprintf("[NFS] path_resolve: failed at '%s': %d\n",
			        component, ret);
			return ret;
		}

		/* Advance to next FH for the next iteration */
		memcpy(&current_fh, &next_fh, sizeof(current_fh));
	}

	/* Return the final entry's FH */
	memcpy(fh, &current_fh, sizeof(*fh));
	return 0;
}

/* ── NFS LOOKUP (legacy convenience wrapper) ──────────────────── */

/* Look up a path relative to a mount, filling the file handle.
 * Supports multi-component paths by delegating to nfs_path_resolve. */
static int nfs_lookup(int mount_id, const char *path, struct nfs_fhandle *fh)
{
	return nfs_path_resolve(mount_id, path, fh);
}

/* ── NFS GETATTR ─────────────────────────────────────────────────── */

static int nfs_getattr(int mount_id, const struct nfs_fhandle *fh,
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

/* ── NFS SETATTR ──────────────────────────────────────────────────── */

/* Set file attributes (sattr3 per RFC 1813) */
struct nfs_sattr {
    uint32_t set_mode;
    uint32_t mode;
    uint32_t set_uid;
    uint32_t uid;
    uint32_t set_gid;
    uint32_t gid;
    uint32_t set_size;
    uint64_t size;
    uint32_t set_atime;
    uint32_t atime_how;  /* 0=DONT_CHANGE, 1=SET_TO_SERVER_TIME, 2=SET_TO_CLIENT_TIME */
    uint64_t atime;
    uint32_t atime_nsec;
    uint32_t set_mtime;
    uint32_t mtime_how;
    uint64_t mtime;
    uint32_t mtime_nsec;
};

static int nfs_setattr(int mount_id, const struct nfs_fhandle *fh,
                 const struct nfs_sattr *sattr)
{
    if (mount_id < 0 || mount_id >= nfs_mount_count)
        return -EINVAL;
    if (!nfs_mounts[mount_id].mounted)
        return -ENOTCONN;
    if (!sattr)
        return -EINVAL;

    struct nfs_mount_info *mnt = &nfs_mounts[mount_id];

    uint8_t args[NFS_MAX_DATA];
    uint8_t *ap = args;

    /* File handle */
    xdr_put_u32(&ap, fh->len);
    xdr_put_bytes(&ap, fh->data, fh->len);
    /* sattr3 fields */
    xdr_put_u32(&ap, sattr->set_mode);
    xdr_put_u32(&ap, sattr->mode);
    xdr_put_u32(&ap, sattr->set_uid);
    xdr_put_u32(&ap, sattr->uid);
    xdr_put_u32(&ap, sattr->set_gid);
    xdr_put_u32(&ap, sattr->gid);
    xdr_put_u32(&ap, sattr->set_size);
    xdr_put_u32(&ap, (uint32_t)(sattr->size >> 32));
    xdr_put_u32(&ap, (uint32_t)(sattr->size & 0xFFFFFFFF));
    xdr_put_u32(&ap, sattr->set_atime);
    xdr_put_u32(&ap, sattr->atime_how);
    xdr_put_u32(&ap, (uint32_t)(sattr->atime >> 32));
    xdr_put_u32(&ap, (uint32_t)(sattr->atime & 0xFFFFFFFF));
    xdr_put_u32(&ap, sattr->atime_nsec);
    xdr_put_u32(&ap, sattr->set_mtime);
    xdr_put_u32(&ap, sattr->mtime_how);
    xdr_put_u32(&ap, (uint32_t)(sattr->mtime >> 32));
    xdr_put_u32(&ap, (uint32_t)(sattr->mtime & 0xFFFFFFFF));
    xdr_put_u32(&ap, sattr->mtime_nsec);

    uint32_t args_len = (uint32_t)(ap - args);

    uint8_t reply[NFS_MAX_DATA];
    uint32_t reply_len = sizeof(reply);

    int ret = nfs_rpc_call(mnt->server_ip, 100003, 3, NFSPROC3_SETATTR,
                            args, args_len, reply, &reply_len);
    if (ret < 0) return ret;

    const uint8_t *rp = reply;
    uint32_t status = xdr_get_u32(&rp);
    if (status != 0) {
        if (status == NFS3ERR_PERM) return -EPERM;
        if (status == NFS3ERR_ACCES) return -EACCES;
        if (status == NFS3ERR_ROFS)  return -EROFS;
        if (status == NFS3ERR_STALE) return -ESTALE;
        return -EIO;
    }

    /* Skip wcc_data (pre-op and post-op attributes — both optional) */
    uint32_t pre_present = xdr_get_u32(&rp);
    if (pre_present) {
        /* Skip 21 words of fattr3 */
        for (int i = 0; i < 21; i++) xdr_get_u32(&rp);
    }
    uint32_t post_present = xdr_get_u32(&rp);
    if (post_present) {
        for (int i = 0; i < 21; i++) xdr_get_u32(&rp);
    }

    kprintf("[NFS] SETATTR: success (fh len=%u)\n", fh->len);
    return 0;
}

/* ── NFS READ ────────────────────────────────────────────────────── */

static int nfs_read(int mount_id, const struct nfs_fhandle *fh,
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

static int nfs_write(int mount_id, const struct nfs_fhandle *fh,
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

static int nfs_create(int mount_id, const struct nfs_fhandle *dir_fh,
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

static int nfs_umount(int mount_id)
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

static void nfs_init(void)
{
    memset(nfs_mounts, 0, sizeof(nfs_mounts));
    nfs_mount_count = 0;
    kprintf("[OK] NFSv3 — Client (RPC/XDR + mount protocol)\n");
}
#ifdef MODULE
#include "module.h"
#else
#include "initcall.h"
#endif
#ifndef MODULE
fs_initcall(nfs_init);
#else
int __init init_module(void)
{
	nfs_init();
	return 0;
}

void __exit cleanup_module(void)
{
	memset(nfs_mounts, 0, sizeof(nfs_mounts));
	nfs_mount_count = 0;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("NFSv3 — Client (RPC/XDR + mount protocol)");
MODULE_VERSION("1.0");
#endif

/* ── nfs_readdir — NFSv3 READDIR with cookie verifier ── */
/*
 * Read directory entries via NFSPROC3_READDIR.
 *
 * Usage:
 *   uint64_t cookie = 0, cookieverf = 0;
 *   do {
 *       ret = nfs_readdir(mount_id, &fh, &cookie, &cookieverf,
 *                         my_callback, priv);
 *       if (ret == -EAGAIN) {
 *           cookie = 0;   // dir changed, restart
 *           continue;
 *       }
 *   } while (ret == 0 && cookie != 0);
 */
static int nfs_readdir(int mount_id, const struct nfs_fhandle *fh,
                uint64_t *cookie, uint64_t *cookieverf,
                int (*callback)(uint64_t fileid, const char *name,
                                uint64_t entry_cookie, void *priv),
                void *priv)
{
    if (mount_id < 0 || mount_id >= nfs_mount_count)
        return -EINVAL;
    if (!nfs_mounts[mount_id].mounted)
        return -ENOTCONN;

    struct nfs_mount_info *mnt = &nfs_mounts[mount_id];

    /* Build READDIR3args */
    uint8_t args[NFS_MAX_DATA];
    uint8_t *ap = args;

    /* Dir FH (nfs_fh3) */
    xdr_put_u32(&ap, fh->len);
    xdr_put_bytes(&ap, fh->data, fh->len);
    /* Cookie (uint64) — 0 means start from beginning */
    xdr_put_u32(&ap, (uint32_t)(*cookie >> 32));
    xdr_put_u32(&ap, (uint32_t)(*cookie & 0xFFFFFFFF));
    /* Cookie verifier (uint64) — 0 on first call */
    xdr_put_u32(&ap, (uint32_t)(*cookieverf >> 32));
    xdr_put_u32(&ap, (uint32_t)(*cookieverf & 0xFFFFFFFF));
    /* Dircount — max bytes of directory info we want */
    xdr_put_u32(&ap, 4096);
    /* Maxcount — max total reply bytes */
    xdr_put_u32(&ap, NFS_MAX_DATA - 256);

    uint32_t args_len = (uint32_t)(ap - args);

    uint8_t reply[NFS_MAX_DATA];
    uint32_t reply_len = sizeof(reply);

    int ret = nfs_rpc_call(mnt->server_ip, 100003, 3, NFSPROC3_READDIR,
                            args, args_len, reply, &reply_len);
    if (ret < 0)
        return ret;

    const uint8_t *rp = reply;

    /* Parse NFS3 status */
    uint32_t nfs_status = xdr_get_u32(&rp);
    if (nfs_status != 0) {
        if (nfs_status == NFS3ERR_NOENT)  return -ENOENT;
        if (nfs_status == NFS3ERR_ACCES)  return -EACCES;
        if (nfs_status == NFS3ERR_NOTDIR) return -ENOTDIR;
        if (nfs_status == NFS3ERR_STALE)  return -ESTALE;
        return -EIO;
    }

    /* Skip post-op dir attributes (optional) */
    uint32_t attr_present = xdr_get_u32(&rp);
    if (attr_present) {
        /* Skip 21 uint32 fattr3 words (type, mode, nlink, uid, gid,
         * size_hi, size_lo, used_hi, used_lo, rdev1, rdev2,
         * fsid_hi, fsid_lo, fileid_hi, fileid_lo,
         * atime_hi, atime_lo, atime_nsec,
         * mtime_hi, mtime_lo, mtime_nsec,
         * ctime_hi, ctime_lo, ctime_nsec) */
        for (int i = 0; i < 21; i++) xdr_get_u32(&rp);
    }

    /* Read cookie verifier returned by server */
    uint64_t new_verf = ((uint64_t)xdr_get_u32(&rp) << 32)
                      | xdr_get_u32(&rp);

    /* Detect directory change via verifier mismatch.
     * Skip check on first call (*cookie == 0 && *cookieverf == 0). */
    if (*cookie != 0 && *cookieverf != 0 && new_verf != *cookieverf) {
        kprintf("[NFS] dir verifier changed, restart needed\n");
        *cookieverf = new_verf;
        *cookie = 0;
        return -EAGAIN;
    }
    *cookieverf = new_verf;

    /* Parse entry list (linked-list follow-pointer encoding) */
    int entry_count = 0;
    while (1) {
        /* Boolean: 1 = entry follows, 0 = no more entries */
        uint32_t has_entry = xdr_get_u32(&rp);
        if (!has_entry)
            break;

        /* fileid (uint64) */
        uint64_t fileid = ((uint64_t)xdr_get_u32(&rp) << 32)
                        | xdr_get_u32(&rp);

        /* name (filename3 = string) */
        uint32_t name_len = xdr_get_u32(&rp);
        uint32_t wire_name_len = name_len; /* save original for wire pointer advancement */
        char name[256];
        if (name_len > 255)
            name_len = 255;
        memcpy(name, rp, name_len);
        rp += wire_name_len; /* advance past actual wire data, not capped */
        name[name_len] = '\0';
        /* Pad to 4 bytes based on original wire length */
        while (wire_name_len & 3) { rp++; wire_name_len++; }

        /* cookie (uint64) — pass back for next page */
        uint64_t entry_cookie = ((uint64_t)xdr_get_u32(&rp) << 32)
                              | xdr_get_u32(&rp);

        /* Use this entry's cookie as resume point */
        *cookie = entry_cookie;

        /* Skip . and .. in callback (VFS handles these) */
        if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
            if (callback && callback(fileid, name, entry_cookie, priv))
                break;  /* callback signaled stop */
        }
        entry_count++;
    }

    /* EOF (uint32 — 1 = no more entries) */
    uint32_t eof = xdr_get_u32(&rp);
    if (eof)
        *cookie = 0;  /* Signal end of directory */

    kprintf("[NFS] READDIR: %d entries%s\n",
            entry_count, eof ? " (EOF)" : "");
    return 0;
}
/* ── NFS FSSTAT (NFSPROC3_FSSTAT) ─────────────────────── */

/*
 * Perform an NFSv3 FSSTAT RPC call on the given file handle.
 * Returns 0 on success with statfs info filled, or negative errno.
 *
 * The FSSTAT procedure (RFC 1813 Section 3.9) queries the server for
 * filesystem-wide usage statistics: total/used/available space and
 * total/used/available inode slots.
 */
static int nfs_fsstat(int mount_id, const struct nfs_fhandle *fh,
               struct vfs_statfs *st)
{
    if (mount_id < 0 || mount_id >= nfs_mount_count)
        return -EINVAL;
    if (!nfs_mounts[mount_id].mounted)
        return -ENOTCONN;
    if (!fh || !st)
        return -EINVAL;

    struct nfs_mount_info *mnt = &nfs_mounts[mount_id];

    /* Build FSSTAT3args: just a file handle */
    uint8_t args[NFS_MAX_DATA];
    uint8_t *ap = args;
    xdr_put_u32(&ap, fh->len);
    xdr_put_bytes(&ap, fh->data, fh->len);
    uint32_t args_len = (uint32_t)(ap - args);

    uint8_t reply[NFS_MAX_DATA];
    uint32_t reply_len = sizeof(reply);

    int ret = nfs_rpc_call(mnt->server_ip, 100003, 3, NFSPROC3_FSSTAT,
                            args, args_len, reply, &reply_len);
    if (ret < 0)
        return ret;

    const uint8_t *rp = reply;
    uint32_t status = xdr_get_u32(&rp);
    if (status != 0) {
        if (status == NFS3ERR_STALE)    return -ESTALE;
        if (status == NFS3ERR_ACCES)    return -EACCES;
        if (status == NFS3ERR_NOENT)    return -ENOENT;
        if (status == NFS3ERR_SERVERFAULT) return -EREMOTEIO;
        return -EIO;
    }

    /* Skip post-op object attributes (optional) */
    uint32_t attr_present = xdr_get_u32(&rp);
    if (attr_present) {
        /* Skip 21 uint32 words of fattr3 */
        for (int i = 0; i < 21; i++)
            (void)xdr_get_u32(&rp);
    }

    /* Parse FSSTAT3resok fields */
    uint64_t tbytes = ((uint64_t)xdr_get_u32(&rp) << 32) | xdr_get_u32(&rp);
    uint64_t fbytes = ((uint64_t)xdr_get_u32(&rp) << 32) | xdr_get_u32(&rp);
    uint64_t abytes = ((uint64_t)xdr_get_u32(&rp) << 32) | xdr_get_u32(&rp);
    uint64_t tfiles = ((uint64_t)xdr_get_u32(&rp) << 32) | xdr_get_u32(&rp);
    uint64_t ffiles = ((uint64_t)xdr_get_u32(&rp) << 32) | xdr_get_u32(&rp);
    uint64_t afiles = ((uint64_t)xdr_get_u32(&rp) << 32) | xdr_get_u32(&rp);
    uint32_t invarsec = xdr_get_u32(&rp);
    (void)invarsec;

    /* Convert to vfs_statfs format (block-based) */
    memset(st, 0, sizeof(*st));
    st->f_type    = 0x0000696e;  /* NFS super magic */
    st->f_bsize   = 4096;
    st->f_blocks  = tbytes / 4096;
    st->f_bfree   = fbytes / 4096;
    st->f_bavail  = abytes / 4096;
    st->f_files   = tfiles;
    st->f_ffree   = ffiles;
    st->f_namelen = 255;

    kprintf("[NFS] FSSTAT: total=%llu free=%llu files=%llu\n",
            (unsigned long long)tbytes,
            (unsigned long long)fbytes,
            (unsigned long long)tfiles);
    return 0;
}

/* ── nfs_statfs (VFS-compatible wrapper) ───────────────── */
static int nfs_statfs(void *sb, void *stat)
{
    (void)sb;
    struct vfs_statfs *st = (struct vfs_statfs *)stat;
    if (!st)
        return -EINVAL;

    /* Try to use the first mounted NFS filesystem for real stats */
    for (int i = 0; i < nfs_mount_count; i++) {
        if (nfs_mounts[i].mounted) {
            int ret = nfs_fsstat(i, &nfs_mounts[i].root_fh, st);
            if (ret == 0)
                return 0;
            break;
        }
    }

    /* Fallback: return sensible defaults */
    memset(st, 0, sizeof(*st));
    st->f_type    = 0x0000696e;
    st->f_bsize   = 4096;
    st->f_blocks  = 0;
    st->f_bfree   = 0;
    st->f_bavail  = 0;
    st->f_files   = 0;
    st->f_ffree   = 0;
    st->f_namelen = 255;
    return 0;
}

/* ── NFS REMOVE (unlink) ──────────────────────────────────────── */

static int nfs_remove(int mount_id, const struct nfs_fhandle *dir_fh,
               const char *name)
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

    uint32_t args_len = (uint32_t)(ap - args);

    uint8_t reply[NFS_MAX_DATA];
    uint32_t reply_len = sizeof(reply);

    int ret = nfs_rpc_call(mnt->server_ip, 100003, 3, NFSPROC3_REMOVE,
                            args, args_len, reply, &reply_len);
    if (ret < 0) return ret;

    const uint8_t *rp = reply;
    uint32_t status = xdr_get_u32(&rp);
    if (status != 0) {
        if (status == NFS3ERR_NOENT) return -ENOENT;
        if (status == NFS3ERR_ACCES) return -EACCES;
        if (status == NFS3ERR_ROFS)  return -EROFS;
        return -EIO;
    }

    kprintf("[NFS] REMOVE: %s (in mount %d)\n", name, mount_id);
    return 0;
}

/* ── NFS MKDIR ─────────────────────────────────────────────────── */

static int nfs_mkdir(int mount_id, const struct nfs_fhandle *dir_fh,
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
    /* Attributes (sattr3) — set mode 0755 */
    xdr_put_u32(&ap, 1); /* set_mode = true */
    xdr_put_u32(&ap, 040755); /* mode */
    xdr_put_u32(&ap, 0); /* set_uid = false */
    xdr_put_u32(&ap, 0); /* uid (ignored) */
    xdr_put_u32(&ap, 0); /* set_gid = false */
    xdr_put_u32(&ap, 0); /* gid (ignored) */
    xdr_put_u32(&ap, 0); /* set_size = false */
    xdr_put_u32(&ap, 0); /* size_hi */
    xdr_put_u32(&ap, 0); /* size_lo */
    xdr_put_u32(&ap, 0); /* set_atime = false */
    xdr_put_u32(&ap, 0); /* atime_how (ignored) */
    xdr_put_u32(&ap, 0); /* atime_hi */
    xdr_put_u32(&ap, 0); /* atime_lo */
    xdr_put_u32(&ap, 0); /* atime_nsec */
    xdr_put_u32(&ap, 0); /* set_mtime = false */
    xdr_put_u32(&ap, 0); /* mtime_how (ignored) */
    xdr_put_u32(&ap, 0); /* mtime_hi */
    xdr_put_u32(&ap, 0); /* mtime_lo */
    xdr_put_u32(&ap, 0); /* mtime_nsec */

    uint32_t args_len = (uint32_t)(ap - args);

    uint8_t reply[NFS_MAX_DATA];
    uint32_t reply_len = sizeof(reply);

    int ret = nfs_rpc_call(mnt->server_ip, 100003, 3, NFSPROC3_MKDIR,
                            args, args_len, reply, &reply_len);
    if (ret < 0) return ret;

    const uint8_t *rp = reply;
    uint32_t status = xdr_get_u32(&rp);
    if (status != 0) {
        if (status == NFS3ERR_EXIST) return -EEXIST;
        if (status == NFS3ERR_ACCES) return -EACCES;
        return -EIO;
    }

    /* Skip dir_wcc */
    uint32_t pre_present = xdr_get_u32(&rp);
    if (pre_present) {
        for (int i = 0; i < 21; i++) xdr_get_u32(&rp);
    }
    uint32_t post_present = xdr_get_u32(&rp);
    if (post_present) {
        for (int i = 0; i < 21; i++) xdr_get_u32(&rp);
    }

    /* Read created directory handle (postop_fh3 — optional) */
    uint32_t fh_present = xdr_get_u32(&rp);
    if (new_fh && fh_present) {
        new_fh->len = xdr_get_u32(&rp);
        if (new_fh->len > sizeof(new_fh->data))
            new_fh->len = sizeof(new_fh->data);
        memcpy(new_fh->data, rp, new_fh->len);
    }

    kprintf("[NFS] MKDIR: %s (in mount %d)\n", name, mount_id);
    return 0;
}

/* ── NFS RMDIR ─────────────────────────────────────────────────── */

static int nfs_rmdir(int mount_id, const struct nfs_fhandle *dir_fh,
              const char *name)
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

    uint32_t args_len = (uint32_t)(ap - args);

    uint8_t reply[NFS_MAX_DATA];
    uint32_t reply_len = sizeof(reply);

    int ret = nfs_rpc_call(mnt->server_ip, 100003, 3, NFSPROC3_RMDIR,
                            args, args_len, reply, &reply_len);
    if (ret < 0) return ret;

    const uint8_t *rp = reply;
    uint32_t status = xdr_get_u32(&rp);
    if (status != 0) {
        if (status == NFS3ERR_NOENT)    return -ENOENT;
        if (status == NFS3ERR_NOTEMPTY) return -ENOTEMPTY;
        if (status == NFS3ERR_ACCES)    return -EACCES;
        if (status == NFS3ERR_NOTDIR)   return -ENOTDIR;
        if (status == NFS3ERR_ROFS)     return -EROFS;
        return -EIO;
    }

    kprintf("[NFS] RMDIR: %s (in mount %d)\n", name, mount_id);
    return 0;
}

/* ── NFS RENAME ───────────────────────────────────────────────── */

static int nfs_rename(int mount_id, const struct nfs_fhandle *from_dir_fh,
               const char *from_name,
               const struct nfs_fhandle *to_dir_fh,
               const char *to_name)
{
    if (mount_id < 0 || mount_id >= nfs_mount_count)
        return -EINVAL;
    if (!nfs_mounts[mount_id].mounted)
        return -ENOTCONN;

    struct nfs_mount_info *mnt = &nfs_mounts[mount_id];

    uint8_t args[NFS_MAX_DATA];
    uint8_t *ap = args;

    /* From: dir FH + name */
    xdr_put_u32(&ap, from_dir_fh->len);
    xdr_put_bytes(&ap, from_dir_fh->data, from_dir_fh->len);
    xdr_put_string(&ap, from_name);
    /* To: dir FH + name */
    xdr_put_u32(&ap, to_dir_fh->len);
    xdr_put_bytes(&ap, to_dir_fh->data, to_dir_fh->len);
    xdr_put_string(&ap, to_name);

    uint32_t args_len = (uint32_t)(ap - args);

    uint8_t reply[NFS_MAX_DATA];
    uint32_t reply_len = sizeof(reply);

    int ret = nfs_rpc_call(mnt->server_ip, 100003, 3, NFSPROC3_RENAME,
                            args, args_len, reply, &reply_len);
    if (ret < 0) return ret;

    const uint8_t *rp = reply;
    uint32_t status = xdr_get_u32(&rp);
    if (status != 0) {
        if (status == NFS3ERR_NOENT)    return -ENOENT;
        if (status == NFS3ERR_ACCES)    return -EACCES;
        if (status == NFS3ERR_EXIST)    return -EEXIST;
        if (status == NFS3ERR_NOTDIR)   return -ENOTDIR;
        if (status == NFS3ERR_ISDIR)    return -EISDIR;
        if (status == NFS3ERR_ROFS)     return -EROFS;
        if (status == NFS3ERR_NOTEMPTY) return -ENOTEMPTY;
        return -EIO;
    }

    kprintf("[NFS] RENAME: %s -> %s (in mount %d)\n",
            from_name, to_name, mount_id);
    return 0;
}
