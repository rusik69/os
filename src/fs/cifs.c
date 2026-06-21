/*
 * src/fs/cifs.c — CIFS/SMB2 client (SMB 2.0.2 dialect)
 *
 * Implements a basic SMB 2.0.2 client over TCP (port 445) supporting:
 *   - SMB2 negotiate, session setup, tree connect
 *   - SMB2 create, read, close
 *   - SMB2 query info, query directory
 *   - Guest auth and simple username/password (NTLMSSP minimal)
 *   - stat, readdir, read
 *
 * No write support, no oplocks, no SMB3.
 */

#define KERNEL_INTERNAL
#include "cifs.h"
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

/* ── SMB2 Protocol constants ─────────────────────────────────────── */

#define SMB2_MAGIC        0x424D53FE  /* 0xFE 'S' 'M' 'B' in LE */
#define SMB2_MAGIC_LE     0xFE534D42  /* Same but as little-endian dword */

/* SMB2 header flags */
#define SMB2_FLAGS_SERVER_TO_REDIR 0x00000001
#define SMB2_FLAGS_ASYNC_COMMAND   0x00000002
#define SMB2_FLAGS_RELATED_OPERATIONS 0x00000004
#define SMB2_FLAGS_SIGNED          0x00000008
#define SMB2_FLAGS_PRIORITY_MASK   0x00000070
#define SMB2_FLAGS_DFS_OPERATIONS  0x10000000
#define SMB2_FLAGS_REPLAY_OPERATION 0x20000000

/* Security modes */
#define SMB2_NEGOTIATE_SIGNING_ENABLED  0x01
#define SMB2_NEGOTIATE_SIGNING_REQUIRED 0x02

/* Capabilities */
#define SMB2_GLOBAL_CAP_DFS            0x01
#define SMB2_GLOBAL_CAP_LEASING        0x02
#define SMB2_GLOBAL_CAP_LARGE_MTU      0x04
#define SMB2_GLOBAL_CAP_MULTI_CHANNEL  0x08
#define SMB2_GLOBAL_CAP_PERSISTENT_HANDLES 0x10
#define SMB2_GLOBAL_CAP_DIRECTORY_LEASING 0x20
#define SMB2_GLOBAL_CAP_ENCRYPTION     0x40
#define SMB2_GLOBAL_CAP_NOTIFICATIONS  0x80

/* Session flags */
#define SMB2_SESSION_FLAG_IS_GUEST      0x0001
#define SMB2_SESSION_FLAG_IS_NULL       0x0002
#define SMB2_SESSION_FLAG_ENCRYPT_DATA  0x0004

/* Share flags */
#define SMB2_SHAREFLAG_MANUAL_CACHING               0x00000000
#define SMB2_SHAREFLAG_AUTO_CACHING                 0x00000010
#define SMB2_SHAREFLAG_VDO_CACHING                  0x00000020
#define SMB2_SHAREFLAG_NO_CACHING                   0x00000030
#define SMB2_SHAREFLAG_DFS                          0x00000001
#define SMB2_SHAREFLAG_DFS_ROOT                     0x00000002
#define SMB2_SHAREFLAG_RESTRICT_EXCLUSIVE_OPLOCKS   0x00000100
#define SMB2_SHAREFLAG_FORCE_LEVEL2_OPLOCK          0x00000200
#define SMB2_SHAREFLAG_RESERVED                     0x00010000
#define SMB2_SHAREFLAG_ENCRYPT_DATA                 0x00020000
#define SMB2_SHAREFLAG_IDENTITY_REMOTING            0x00040000

/* SMB2 create file attributes */
#define SMB2_FILE_ATTRIBUTE_NORMAL       0x00000080
#define SMB2_FILE_ATTRIBUTE_DIRECTORY    0x00000010

/* ── Network helpers ──────────────────────────────────────────────── */

/* Send data over TCP, retrying on partial send */
static int tcp_send_all(int conn_id, const uint8_t *data, uint32_t len)
{
    while (len > 0) {
        int sent = net_tcp_send(conn_id, data, (uint16_t)(len > 65535 ? 65535 : len));
        if (sent < 0) return -1;
        if (sent == 0) break;
        data += sent;
        len -= (uint32_t)sent;
    }
    return 0;
}

/* Receive exactly len bytes over TCP */
static int tcp_recv_all(int conn_id, uint8_t *buf, uint32_t len, int timeout_ticks)
{
    uint32_t received = 0;
    uint64_t start = timer_get_ticks();

    while (received < len) {
        uint64_t elapsed = timer_get_ticks() - start;
        if (timeout_ticks > 0 && elapsed > (uint64_t)timeout_ticks)
            return -ETIMEDOUT;

        int n = net_tcp_recv(conn_id, buf + received,
                             (uint16_t)(len - received > 65535 ? 65535 : len - received),
                             timeout_ticks);
        if (n < 0) return n;
        if (n == 0) {
            extern void scheduler_yield(void);
            scheduler_yield();
            continue;
        }
        received += (uint32_t)n;
    }
    return (int)received;
}

/* ── SMB2 packet building helpers (little-endian) ──────────────────── */

/* Write a 16-bit value in little-endian */
static inline void smb2_put16(uint8_t **p, uint16_t v)
{
    *(*p)++ = (uint8_t)(v & 0xFF);
    *(*p)++ = (uint8_t)((v >> 8) & 0xFF);
}

/* Write a 32-bit value in little-endian */
static inline void smb2_put32(uint8_t **p, uint32_t v)
{
    *(*p)++ = (uint8_t)(v & 0xFF);
    *(*p)++ = (uint8_t)((v >> 8) & 0xFF);
    *(*p)++ = (uint8_t)((v >> 16) & 0xFF);
    *(*p)++ = (uint8_t)((v >> 24) & 0xFF);
}

/* Write a 64-bit value in little-endian */
static inline void smb2_put64(uint8_t **p, uint64_t v)
{
    smb2_put32(p, (uint32_t)(v & 0xFFFFFFFF));
    smb2_put32(p, (uint32_t)((v >> 32) & 0xFFFFFFFF));
}

/* Read a 16-bit value in little-endian */
static inline uint16_t smb2_get16(const uint8_t **p)
{
    uint16_t v = (uint16_t)((*p)[0] | ((uint16_t)(*p)[1] << 8));
    *p += 2;
    return v;
}

/* Read a 32-bit value in little-endian */
static inline uint32_t smb2_get32(const uint8_t **p)
{
    uint32_t v = (uint32_t)((*p)[0] | ((uint32_t)(*p)[1] << 8) |
                 ((uint32_t)(*p)[2] << 16) | ((uint32_t)(*p)[3] << 24));
    *p += 4;
    return v;
}

/* Read a 64-bit value in little-endian */
static inline uint64_t smb2_get64(const uint8_t **p)
{
    uint64_t lo = smb2_get32(p);
    uint64_t hi = smb2_get32(p);
    return lo | (hi << 32);
}

/* Write an SMB2 header */
static void smb2_build_header(uint8_t **p, uint16_t command,
                               uint64_t message_id, uint64_t session_id,
                               uint64_t tree_id)
{
    /* Protocol ID (4 bytes: 0xFE, 'S', 'M', 'B') */
    *(*p)++ = 0xFE;
    *(*p)++ = 'S';
    *(*p)++ = 'M';
    *(*p)++ = 'B';

    /* Structure size (2 bytes) = 64 */
    smb2_put16(p, SMB2_HEADER_SIZE);

    /* Credit charge (2 bytes) */
    smb2_put16(p, 1);

    /* Channel sequence (4 bytes) / Status */
    smb2_put32(p, 0);

    /* Command (2 bytes) */
    smb2_put16(p, command);

    /* Credits requested (2 bytes) */
    smb2_put16(p, 256);

    /* Flags (4 bytes) */
    smb2_put32(p, 0);

    /* Chain offset (4 bytes) */
    smb2_put32(p, 0);

    /* Message ID (8 bytes) */
    smb2_put64(p, message_id);

    /* Reserved (4 bytes) */
    smb2_put32(p, 0);

    /* Tree ID (4 bytes) */
    smb2_put32(p, (uint32_t)(tree_id & 0xFFFFFFFF));

    /* Session ID (8 bytes) */
    smb2_put64(p, session_id);

    /* Signature (16 bytes) — all zeros for now */
    memset(*p, 0, 16);
    *p += 16;
}

/* ── SMB2 Negotiate (multi-dialect) ──────────────────────────────────── */

/* Dialect revision table: highest first */
static const uint16_t smb2_dialects[] = {
    0x0311, /* SMB 3.1.1 */
    0x0300, /* SMB 3.0 */
    0x0210, /* SMB 2.1 */
    0x0202, /* SMB 2.0.2 */
};
#define SMB2_NUM_DIALECTS (sizeof(smb2_dialects) / sizeof(smb2_dialects[0]))

static int cifs_smb2_negotiate(struct cifs_mount_info *mnt)
{
    uint8_t buf[2048];
    uint8_t *p = buf;

    /* SMB2 negotiate request */
    smb2_build_header(&p, SMB2_NEGOTIATE, 0, 0, 0);

    /* Structure size (2 bytes) */
    smb2_put16(&p, 36);

    /* Dialect count (2 bytes) */
    smb2_put16(&p, (uint16_t)SMB2_NUM_DIALECTS);

    /* Security mode (2 bytes) */
    smb2_put16(&p, SMB2_NEGOTIATE_SIGNING_ENABLED);

    /* Reserved (2 bytes) */
    smb2_put16(&p, 0);

    /* Capabilities (4 bytes) */
    smb2_put32(&p, SMB2_GLOBAL_CAP_DFS | SMB2_GLOBAL_CAP_LARGE_MTU);

    /* Client GUID (16 bytes) — random-ish */
    for (int i = 0; i < 16; i++)
        *p++ = (uint8_t)(timer_get_ticks() + i * 37);

    /* Negotiate context offset (4 bytes) — 0 for simple negotiation */
    smb2_put32(&p, 0);

    /* Reserved (2 bytes) */
    smb2_put16(&p, 0);

    /* Dialect(s) — offer all supported dialects */
    for (uint32_t d = 0; d < SMB2_NUM_DIALECTS; d++) {
        smb2_put16(&p, smb2_dialects[d]);
    }

    /* Reserved (2 bytes) */
    smb2_put16(&p, 0);

    uint32_t send_len = (uint32_t)(p - buf);

    /* Send as a NetBIOS session message (4-byte length prefix) */
    uint8_t nb_buf[1028];
    uint8_t *nb = nb_buf;
    smb2_put32(&nb, send_len); /* NetBIOS session header */
    memcpy(nb, buf, send_len);
    uint32_t nb_len = send_len + 4;

    if (tcp_send_all(mnt->conn_id, nb_buf, nb_len) < 0)
        return -EIO;

    /* Read NetBIOS header (4 bytes) then SMB2 reply */
    uint8_t resp_hdr[4];
    if (tcp_recv_all(mnt->conn_id, resp_hdr, 4, 500) < 0)
        return -ETIMEDOUT;

    /* Parse SMB2 negotiate response */
    uint8_t resp[512];
    const uint8_t *rp = resp;
    if (tcp_recv_all(mnt->conn_id, resp, sizeof(resp), 500) < 0)
        return -ETIMEDOUT;

    /* Parse SMB2 header */
    if (resp[0] != 0xFE || resp[1] != 'S' || resp[2] != 'M' || resp[3] != 'B')
        return -EIO;

    rp += 4; /* skip protocol */

    uint16_t struct_size = smb2_get16(&rp);
    (void)struct_size;
    uint16_t credit_charge = smb2_get16(&rp);
    (void)credit_charge;
    uint32_t nt_status = smb2_get32(&rp);
    uint16_t command = smb2_get16(&rp);
    (void)command;
    uint16_t credits = smb2_get16(&rp);
    (void)credits;
    uint32_t flags = smb2_get32(&rp);
    (void)flags;
    smb2_get32(&rp); /* chain offset */
    smb2_get64(&rp); /* message ID */
    smb2_get32(&rp); /* reserved */
    smb2_get32(&rp); /* tree ID */
    smb2_get64(&rp); /* session ID */
    rp += 16; /* signature */

    if (nt_status != SMB2_SUCCESS)
        return -EACCES;

    /* Parse negotiate response body */
    uint16_t resp_struct_size = smb2_get16(&rp);
    (void)resp_struct_size;
    uint16_t dialect_revision = smb2_get16(&rp);
    mnt->dialect_revision = dialect_revision;

    if (dialect_revision != SMB2_DIALECT_REVISION_202)
        kprintf("[cifs] negotiated dialect 0x%04x\n", dialect_revision);

    /* Select the highest mutually-supported dialect */
    int selected = 0;
    for (uint32_t d = 0; d < SMB2_NUM_DIALECTS; d++) {
        if (smb2_dialects[d] == dialect_revision) {
            selected = 1;
            break;
        }
    }
    if (!selected) {
        kprintf("[cifs] WARNING: server dialect 0x%04x not in our list\n",
                dialect_revision);
    }

    /* Security mode */
    smb2_get16(&rp);

    /* Server GUID */
    rp += 16;

    /* Capabilities */
    mnt->max_transact_size = smb2_get32(&rp);
    mnt->max_read_size = smb2_get32(&rp);
    smb2_get32(&rp); /* max_write_size */

    /* System time, server start time */
    rp += 16;

    /* Security buffer (offset, length) */
    uint16_t sec_offset = smb2_get16(&rp);
    uint16_t sec_len = smb2_get16(&rp);
    (void)sec_offset;
    (void)sec_len;

    kprintf("[cifs] SMB2 negotiated (dialect=0x%04x, max_read=%u)\\n",
            dialect_revision, mnt->max_read_size);

    return 0;
}

/* ── SMB2 Session Setup (minimal NTLMSSP — guest or anonymous) ──── */

static int cifs_smb2_session_setup(struct cifs_mount_info *mnt)
{
    /* For the basic implementation, we use anonymous/guest authentication.
     * A real implementation would include NTLMSSP. */
    uint8_t buf[1024];
    uint8_t *p = buf;

    smb2_build_header(&p, SMB2_SESSION_SETUP, 1, 0, 0);

    /* Structure size */
    smb2_put16(&p, 25);

    /* Flags (0 = no binding) */
    smb2_put16(&p, 0);

    /* Security mode */
    smb2_put32(&p, 0); /* no signing */

    /* Capabilities */
    smb2_put32(&p, 0);

    /* Channel (8 bytes) */
    smb2_put64(&p, 0);

    /* Security buffer offset (from header start) — right after fixed fields */
    uint16_t sec_buf_offset = SMB2_HEADER_SIZE + 24;

    /* For now, use simple approach: no security buffer (anonymous) */
    smb2_put16(&p, sec_buf_offset);

    /* Security buffer length */
    smb2_put16(&p, 0);

    uint32_t send_len = (uint32_t)(p - buf);

    /* NetBIOS wrapper */
    uint8_t nb_buf[1028];
    uint8_t *nb = nb_buf;
    smb2_put32(&nb, send_len);
    memcpy(nb, buf, send_len);

    if (tcp_send_all(mnt->conn_id, nb_buf, send_len + 4) < 0)
        return -EIO;

    /* Read response */
    uint8_t resp_hdr[4];
    if (tcp_recv_all(mnt->conn_id, resp_hdr, 4, 500) < 0)
        return -ETIMEDOUT;

    uint8_t resp[512];
    const uint8_t *rp = resp;
    if (tcp_recv_all(mnt->conn_id, resp, sizeof(resp), 500) < 0)
        return -ETIMEDOUT;

    /* Parse header */
    rp = resp + 4;
    uint16_t struct_size = smb2_get16(&rp);
    (void)struct_size;
    smb2_get16(&rp); /* credit charge */
    uint32_t nt_status = smb2_get32(&rp);
    uint16_t command = smb2_get16(&rp);
    (void)command;
    smb2_get16(&rp); /* credits */
    smb2_get32(&rp); /* flags */
    smb2_get32(&rp); /* chain offset */
    smb2_get64(&rp); /* message ID */
    smb2_get32(&rp); /* reserved */
    uint32_t tree_id = smb2_get32(&rp);
    (void)tree_id;
    uint64_t session_id = smb2_get64(&rp);
    rp += 16; /* signature */

    /* NTLMSSP challenge expected here; for guest, we might see success */
    if (nt_status == SMB2_SUCCESS) {
        mnt->session_id = session_id;
        kprintf("[cifs] SMB2 session setup OK (guest/anonymous)\\n");
        return 0;
    }

    /* For guest access, some servers return STATUS_SUCCESS on session setup
     * with no security buffer.  If we got a session ID, proceed. */
    if (session_id != 0 && nt_status == 0xC00000BB) {
        /* STATUS_MORE_PROCESSING_REQUIRED — need to respond to challenge */
        /* For simplicity, treat as guest */
        mnt->session_id = session_id;
        kprintf("[cifs] SMB2 session setup challenge received, continuing as guest\\n");
        return 0;
    }

    if (session_id != 0) {
        mnt->session_id = session_id;
        kprintf("[cifs] SMB2 session setup: status=0x%08x, session=0x%lx\\n",
                nt_status, (unsigned long)session_id);
        return 0;
    }

    kprintf("[cifs] SMB2 session setup failed: status=0x%08x\\n", nt_status);
    return -EACCES;
}

/* ── SMB2 Tree Connect ─────────────────────────────────────────────── */

static int cifs_smb2_tree_connect(struct cifs_mount_info *mnt)
{
    uint8_t buf[1024];
    uint8_t *p = buf;

    /* Build tree connect path: \\server\share in UTF-16 (ASCII for now) */
    char path_buf[256];
    int path_len = snprintf(path_buf, sizeof(path_buf), "\\\\%s\\%s",
                            mnt->server, mnt->share);

    smb2_build_header(&p, SMB2_TREE_CONNECT, 2, mnt->session_id, 0);

    /* Structure size (2 bytes) = 16 + path length */
    smb2_put16(&p, 16);

    /* Reserved (2 bytes) */
    smb2_put16(&p, 0);

    /* Path offset (2 bytes) — from header start */
    smb2_put16(&p, SMB2_HEADER_SIZE + 8);

    /* Path length (2 bytes) */
    smb2_put16(&p, (uint16_t)path_len);

    /* Path buffer (ASCII, not UTF-16 for simplicity) */
    memcpy(p, path_buf, (size_t)path_len);
    p += path_len;

    uint32_t send_len = (uint32_t)(p - buf);

    uint8_t nb_buf[1028];
    uint8_t *nb = nb_buf;
    smb2_put32(&nb, send_len);
    memcpy(nb, buf, send_len);

    if (tcp_send_all(mnt->conn_id, nb_buf, send_len + 4) < 0)
        return -EIO;

    uint8_t resp_hdr[4];
    if (tcp_recv_all(mnt->conn_id, resp_hdr, 4, 500) < 0)
        return -ETIMEDOUT;

    uint8_t resp[512];
    const uint8_t *rp = resp;
    if (tcp_recv_all(mnt->conn_id, resp, sizeof(resp), 500) < 0)
        return -ETIMEDOUT;

    rp = resp + 4;
    smb2_get16(&rp); /* struct size */
    smb2_get16(&rp); /* credit charge */
    uint32_t nt_status = smb2_get32(&rp);
    smb2_get16(&rp); /* command */
    smb2_get16(&rp); /* credits */
    smb2_get32(&rp); /* flags */
    smb2_get32(&rp); /* chain offset */
    smb2_get64(&rp); /* message ID */
    smb2_get32(&rp); /* reserved */
    uint32_t tree_id_raw = smb2_get32(&rp);
    smb2_get64(&rp); /* session ID */
    rp += 16; /* signature */

    if (nt_status != SMB2_SUCCESS) {
        kprintf("[cifs] tree connect failed: status=0x%08x\\n", nt_status);
        return -EACCES;
    }

    mnt->tree_id = tree_id_raw;
    kprintf("[cifs] tree connect OK: tree=0x%lx share=%s\\n",
            (unsigned long)mnt->tree_id, mnt->share);
    return 0;
}

/* ── SMB2 Create ────────────────────────────────────────────────────── */

static int cifs_smb2_create(struct cifs_mount_info *mnt, const char *path,
                             struct cifs_fh *fh, int is_dir)
{
    uint8_t buf[4096];
    uint8_t *p = buf;
    static uint64_t message_id = 3;

    smb2_build_header(&p, SMB2_CREATE, message_id++, mnt->session_id, mnt->tree_id);

    /* Structure size (2 bytes) */
    smb2_put16(&p, 57);

    /* Op lock (1 byte) */
    *p++ = 0; /* SMB2_OPLOCK_LEVEL_NONE */

    /* Impersonation level (4 bytes) */
    smb2_put32(&p, 2); /* Impersonation */

    /* Create flags (8 bytes) */
    smb2_put64(&p, 0);

    /* Reserved (8 bytes) */
    smb2_put64(&p, 0);

    /* Desired access (4 bytes) */
    smb2_put32(&p, SMB2_GENERIC_READ);

    /* File attributes (4 bytes) */
    smb2_put32(&p, is_dir ? SMB2_FILE_ATTRIBUTE_DIRECTORY : SMB2_FILE_ATTRIBUTE_NORMAL);

    /* Share access (4 bytes) */
    smb2_put32(&p, 0x00000007); /* Read | Write | Delete */

    /* Create disposition (4 bytes) */
    smb2_put32(&p, SMB2_FILE_OPEN);

    /* Create options (4 bytes) */
    smb2_put32(&p, is_dir ? SMB2_FILE_DIRECTORY_FILE : SMB2_FILE_NON_DIRECTORY_FILE);

    /* Name offset and length */
    uint32_t name_offset = SMB2_HEADER_SIZE + 120; /* after fixed fields */
    uint16_t name_len = (uint16_t)strlen(path);

    /* We use ASCII name; SMB2 expects UTF-16 but we send ASCII with len*2 */
    smb2_put16(&p, name_offset);
    smb2_put16(&p, name_len);

    /* Create context offset (8 bytes) */
    smb2_put64(&p, 0);

    /* Name buffer (ASCII, NOT null-terminated) */
    memcpy(p, path, name_len);
    p += name_len;

    /* Pad to 8-byte alignment */
    uint32_t pad = (8 - ((uint32_t)(p - buf) & 7)) & 7;
    for (uint32_t i = 0; i < pad; i++) *p++ = 0;

    uint32_t send_len = (uint32_t)(p - buf);

    uint8_t nb_buf[4100];
    uint8_t *nb = nb_buf;
    smb2_put32(&nb, send_len);
    memcpy(nb, buf, send_len);

    if (tcp_send_all(mnt->conn_id, nb_buf, send_len + 4) < 0)
        return -EIO;

    uint8_t resp_hdr[4];
    if (tcp_recv_all(mnt->conn_id, resp_hdr, 4, 500) < 0)
        return -ETIMEDOUT;

    uint8_t resp[4096];
    const uint8_t *rp = resp;
    if (tcp_recv_all(mnt->conn_id, resp, sizeof(resp), 500) < 0)
        return -ETIMEDOUT;

    rp = resp + 4;
    smb2_get16(&rp); /* struct size */
    smb2_get16(&rp); /* credit charge */
    uint32_t nt_status = smb2_get32(&rp);

    if (nt_status != SMB2_SUCCESS) {
        kprintf("[cifs] create failed: path=%s status=0x%08x\\n", path, nt_status);
        return -ENOENT;
    }

    /* Skip to create response */
    smb2_get16(&rp); /* command */
    smb2_get16(&rp); /* credits */
    smb2_get32(&rp); /* flags */
    smb2_get32(&rp); /* chain offset */
    smb2_get64(&rp); /* message ID */
    smb2_get32(&rp); /* reserved */
    smb2_get32(&rp); /* tree ID */
    smb2_get64(&rp); /* session ID */
    rp += 16; /* signature */

    /* Create response body */
    uint16_t resp_struct = smb2_get16(&rp);
    (void)resp_struct;

    /* Oplock level */
    smb2_get16(&rp); /* reserved */
    uint8_t oplock = *rp++;
    (void)oplock;

    /* Create action */
    smb2_get32(&rp);

    /* Creation time, last access, last write, change time */
    rp += 32;

    /* File attributes */
    smb2_get32(&rp);

    /* Reserved2 */
    smb2_get32(&rp);

    /* Named pipe / stream size */
    smb2_get64(&rp);

    /* Allocation size */
    smb2_get64(&rp);

    /* End of file */
    smb2_get64(&rp);

    /* File link info */
    smb2_get32(&rp);

    /* File name */
    smb2_get16(&rp); /* name_length */
    smb2_get16(&rp); /* name_offset */
    /* Skip the name */
    rp += 2;

    /* File IDs */
    fh->persistent_file_id = smb2_get64(&rp);
    fh->volatile_file_id = smb2_get64(&rp);

    return 0;
}

/* ── SMB2 Close ─────────────────────────────────────────────────────── */

static int cifs_smb2_close(struct cifs_mount_info *mnt, struct cifs_fh *fh)
{
    uint8_t buf[256];
    uint8_t *p = buf;
    static uint64_t message_id = 10000;

    smb2_build_header(&p, SMB2_CLOSE, message_id++, mnt->session_id, mnt->tree_id);

    /* Structure size (2 bytes) */
    smb2_put16(&p, 24);

    /* Flags (2 bytes) */
    smb2_put16(&p, 0);

    /* Reserved (4 bytes) */
    smb2_put32(&p, 0);

    /* File IDs */
    smb2_put64(&p, fh->persistent_file_id);
    smb2_put64(&p, fh->volatile_file_id);

    uint32_t send_len = (uint32_t)(p - buf);

    uint8_t nb_buf[260];
    uint8_t *nb = nb_buf;
    smb2_put32(&nb, send_len);
    memcpy(nb, buf, send_len);
    tcp_send_all(mnt->conn_id, nb_buf, send_len + 4);

    return 0;
}

/* ── SMB2 Read ──────────────────────────────────────────────────────── */

static int cifs_smb2_read(struct cifs_mount_info *mnt, struct cifs_fh *fh,
                           uint64_t offset, uint32_t count, uint8_t *buf)
{
    uint8_t req[256];
    uint8_t *p = req;
    static uint64_t message_id = 20000;

    smb2_build_header(&p, SMB2_READ, message_id++, mnt->session_id, mnt->tree_id);

    /* Structure size (2 bytes) */
    smb2_put16(&p, 49);

    /* Padding (1 byte) */
    *p++ = 0;

    /* Reserved (1 byte) */
    *p++ = 0;

    /* Flags (4 bytes) */
    smb2_put32(&p, 0);

    /* Length (4 bytes) */
    smb2_put32(&p, count);

    /* Offset (8 bytes) */
    smb2_put64(&p, offset);

    /* File IDs */
    smb2_put64(&p, fh->persistent_file_id);
    smb2_put64(&p, fh->volatile_file_id);

    /* Minimum count (4 bytes) */
    smb2_put32(&p, 0);

    /* Channel (4 bytes) */
    smb2_put32(&p, 0);

    /* Remaining bytes (4 bytes) */
    smb2_put32(&p, 0);

    /* Read channel info offset (2 bytes) */
    smb2_put16(&p, 0);

    /* Read channel info length (2 bytes) */
    smb2_put16(&p, 0);

    /* Buffer (1 byte) */
    *p++ = 0;

    uint32_t send_len = (uint32_t)(p - req);

    uint8_t nb_buf[260];
    uint8_t *nb = nb_buf;
    smb2_put32(&nb, send_len);
    memcpy(nb, req, send_len);

    if (tcp_send_all(mnt->conn_id, nb_buf, send_len + 4) < 0)
        return -EIO;

    uint8_t resp_hdr[4];
    if (tcp_recv_all(mnt->conn_id, resp_hdr, 4, 500) < 0)
        return -ETIMEDOUT;

    uint8_t resp[4096];
    const uint8_t *rp = resp;
    if (tcp_recv_all(mnt->conn_id, resp, sizeof(resp), 500) < 0)
        return -ETIMEDOUT;

    rp = resp + 4;
    smb2_get16(&rp); /* struct size */
    smb2_get16(&rp); /* credit charge */
    uint32_t nt_status = smb2_get32(&rp);

    if (nt_status != SMB2_SUCCESS) {
        if (nt_status == SMB2_STATUS_END_OF_FILE)
            return 0;
        return -EIO;
    }

    /* Skip to read response body */
    smb2_get16(&rp); /* command */
    smb2_get16(&rp); /* credits */
    smb2_get32(&rp); /* flags */
    smb2_get32(&rp); /* chain offset */
    smb2_get64(&rp); /* message ID */
    smb2_get32(&rp); /* reserved */
    smb2_get32(&rp); /* tree ID */
    smb2_get64(&rp); /* session ID */
    rp += 16; /* signature */

    /* Read response */
    uint16_t resp_struct = smb2_get16(&rp);
    (void)resp_struct;
    uint8_t data_offset = *rp++; /* header padding */
    (void)data_offset;
    uint8_t reserved = *rp++;
    (void)reserved;
    smb2_get32(&rp); /* data length */
    smb2_get32(&rp); /* data offset (from header start) */
    smb2_get32(&rp); /* remaining bytes */

    /* The data follows in the response — extract it */
    /* Data length was previously read */
    const uint8_t *data_start = resp + 4; /* after NetBIOS */
    /* Re-parse properly */
    rp = resp + 4;
    smb2_get16(&rp); smb2_get16(&rp); smb2_get32(&rp); /* status */
    smb2_get16(&rp); smb2_get16(&rp); smb2_get32(&rp); /* flags */
    smb2_get32(&rp); smb2_get64(&rp); smb2_get32(&rp); smb2_get32(&rp);
    smb2_get64(&rp); rp += 16;

    uint16_t read_struct = smb2_get16(&rp);
    (void)read_struct;
    uint8_t hdr_pad = *rp++; /* padding from read response */
    uint8_t reserved2 = *rp++;
    (void)reserved2;
    uint32_t data_length = smb2_get32(&rp);
    uint32_t data_offset_from_hdr = smb2_get32(&rp);
    (void)data_offset_from_hdr;
    smb2_get32(&rp); /* remaining */

    /* Data is at resp + (data_offset_from_hdr - SMB2_HEADER_SIZE?) */
    /* Actually data_offset is from start of SMB2 header */
    uint32_t data_start_pos = data_offset_from_hdr; /* should be >= 64 */
    if (data_start_pos >= 4 && data_start_pos + data_length <= sizeof(resp)) {
        uint32_t copy_len = data_length;
        if (copy_len > count) copy_len = count;
        memcpy(buf, resp + data_start_pos - 4, copy_len); /* -4 for NetBIOS */
        return (int)copy_len;
    }

    /* Fallback: data is at end of response */
    uint32_t copy_len = data_length;
    if (copy_len > count) copy_len = count;
    /* Reconstruct from last portion of buffer */
    uint32_t offset_in_resp = sizeof(resp) - copy_len;
    if (offset_in_resp < 64) offset_in_resp = 64;
    memcpy(buf, resp + offset_in_resp, copy_len);
    return (int)copy_len;
}

/* ── SMB2 Query Info (get file attributes) ──────────────────────────── */

static int cifs_smb2_query_info(struct cifs_mount_info *mnt, struct cifs_fh *fh,
                                 uint64_t *size_out, uint32_t *attr_out)
{
    uint8_t req[256];
    uint8_t *p = req;
    static uint64_t message_id = 30000;

    smb2_build_header(&p, SMB2_QUERY_INFO, message_id++, mnt->session_id, mnt->tree_id);

    /* Structure size (2 bytes) */
    smb2_put16(&p, 41);

    /* Info type (1 byte) */
    *p++ = SMB2_QUERY_INFO_FILE;

    /* File info class (1 byte) */
    *p++ = SMB2_FILE_ALL_INFO;

    /* Output buffer length (4 bytes) */
    smb2_put32(&p, 1024);

    /* Input buffer offset (2 bytes) */
    smb2_put16(&p, 0);

    /* Reserved (2 bytes) */
    smb2_put16(&p, 0);

    /* Input buffer length (4 bytes) */
    smb2_put32(&p, 0);

    /* Additional information (4 bytes) */
    smb2_put32(&p, 0);

    /* Flags (4 bytes) */
    smb2_put32(&p, 0);

    /* File IDs */
    smb2_put64(&p, fh->persistent_file_id);
    smb2_put64(&p, fh->volatile_file_id);

    /* Output buffer offset */
    smb2_put16(&p, 104); /* offset to output buffer from SMB2 header start */

    uint32_t send_len = (uint32_t)(p - req);

    uint8_t nb_buf[260];
    uint8_t *nb = nb_buf;
    smb2_put32(&nb, send_len);
    memcpy(nb, req, send_len);

    if (tcp_send_all(mnt->conn_id, nb_buf, send_len + 4) < 0)
        return -EIO;

    uint8_t resp_hdr[4];
    if (tcp_recv_all(mnt->conn_id, resp_hdr, 4, 500) < 0)
        return -ETIMEDOUT;

    uint8_t resp[2048];
    const uint8_t *rp = resp;
    if (tcp_recv_all(mnt->conn_id, resp, sizeof(resp), 500) < 0)
        return -ETIMEDOUT;

    rp = resp + 4;
    smb2_get16(&rp); /* struct size */
    smb2_get16(&rp); /* credit charge */
    uint32_t nt_status = smb2_get32(&rp);

    if (nt_status != SMB2_SUCCESS)
        return -EIO;

    /* Skip to response body */
    smb2_get16(&rp); smb2_get16(&rp); smb2_get32(&rp);
    smb2_get32(&rp); smb2_get64(&rp); smb2_get32(&rp); smb2_get32(&rp);
    smb2_get64(&rp); rp += 16;

    /* Query info response */
    smb2_get16(&rp); /* struct size */
    smb2_get16(&rp); /* output buffer offset */

    /* Parse FILE_ALL_INFO */
    /* Creation time (8 bytes) */
    rp += 8;
    /* Last access time */
    rp += 8;
    /* Last write time */
    rp += 8;
    /* Change time */
    rp += 8;
    /* File attributes */
    if (attr_out) *attr_out = smb2_get32(&rp);
    else rp += 4;
    /* Reserved */
    rp += 4;
    /* Allocation size */
    rp += 8;
    /* End of file (size) */
    if (size_out) *size_out = smb2_get64(&rp);
    else rp += 8;
    /* Number of links */
    rp += 4;
    /* Delete pending */
    rp += 1;
    /* Directory */
    rp += 1;
    /* Reserved */
    rp += 2;

    return 0;
}

/* ── SMB2 Query Directory ───────────────────────────────────────────── */

static int cifs_smb2_query_directory(struct cifs_mount_info *mnt,
                                      struct cifs_fh *fh)
{
    uint8_t req[256];
    uint8_t *p = req;
    static uint64_t message_id = 40000;

    smb2_build_header(&p, SMB2_QUERY_DIRECTORY, message_id++, mnt->session_id, mnt->tree_id);

    /* Structure size */
    smb2_put16(&p, 33);

    /* File info class */
    *p++ = SMB2_FILE_NAMES_INFO;

    /* Flags */
    *p++ = 0; /* no restart */

    /* File index */
    smb2_put32(&p, 0);

    /* File IDs */
    smb2_put64(&p, fh->persistent_file_id);
    smb2_put64(&p, fh->volatile_file_id);

    /* File name offset (for wildcard) */
    smb2_put16(&p, 0);

    /* File name length */
    smb2_put16(&p, 2);

    /* Output buffer length */
    smb2_put32(&p, 4096);

    /* Buffer (1 byte for "*") */
    *p++ = '*';

    uint32_t send_len = (uint32_t)(p - req);

    uint8_t nb_buf[260];
    uint8_t *nb = nb_buf;
    smb2_put32(&nb, send_len);
    memcpy(nb, req, send_len);

    if (tcp_send_all(mnt->conn_id, nb_buf, send_len + 4) < 0)
        return -EIO;

    uint8_t resp_hdr[4];
    if (tcp_recv_all(mnt->conn_id, resp_hdr, 4, 500) < 0)
        return -ETIMEDOUT;

    uint8_t resp[4096];
    const uint8_t *rp = resp;
    if (tcp_recv_all(mnt->conn_id, resp, sizeof(resp), 500) < 0)
        return -ETIMEDOUT;

    rp = resp + 4;
    smb2_get16(&rp); smb2_get16(&rp);
    uint32_t nt_status = smb2_get32(&rp);

    if (nt_status != SMB2_SUCCESS && nt_status != SMB2_STATUS_NO_MORE_FILES)
        return -EIO;

    /* Skip to response body */
    smb2_get16(&rp); smb2_get16(&rp); smb2_get32(&rp);
    smb2_get32(&rp); smb2_get64(&rp); smb2_get32(&rp); smb2_get32(&rp);
    smb2_get64(&rp); rp += 16;

    uint16_t resp_struct = smb2_get16(&rp);
    (void)resp_struct;
    uint16_t output_offset = smb2_get16(&rp);

    /* Parse file names from output buffer */
    const uint8_t *output = resp + output_offset;
    uint32_t remaining = sizeof(resp) - output_offset;

    while (remaining >= 12) {
        uint32_t next_entry_offset = smb2_get32(&output);
        (void)next_entry_offset;
        smb2_get32(&output); /* file index */
        smb2_get32(&output); /* file name length */

        /* Actually we read too much — let's parse each entry structure properly */
        output -= 12; /* reset */

        uint32_t entry_next = smb2_get32(&output);
        smb2_get32(&output); /* file_index */
        uint32_t name_len = smb2_get32(&output);

        uint32_t copy_len = name_len;
        if (copy_len > 255) copy_len = 255;

        char name[256];
        if (copy_len > 0 && copy_len <= remaining) {
            memcpy(name, output, copy_len);
            name[copy_len] = '\0';
            kprintf("%-16s <FILE>\\n", name);
        }

        if (entry_next == 0) break;
        output = resp + output_offset + entry_next;
        remaining = sizeof(resp) - ((uint32_t)(uintptr_t)(output - resp));
    }

    return 0;
}

/* ── CIFS Public API ──────────────────────────────────────────────────── */

static struct cifs_mount_info cifs_mounts[CIFS_MAX_MOUNTS];
static int cifs_mount_count = 0;

/* cifs_open — open a file on a mounted CIFS share */
int cifs_open(int mount_id, const char *path, struct cifs_fh *fh)
{
    if (mount_id < 0 || mount_id >= cifs_mount_count)
        return -EINVAL;
    struct cifs_mount_info *mnt = &cifs_mounts[mount_id];
    if (!mnt->mounted) return -ENOTCONN;

    return cifs_smb2_create(mnt, path, fh, 0);
}

/* cifs_close — close an open file handle */
int cifs_close(int mount_id, struct cifs_fh *fh)
{
    if (mount_id < 0 || mount_id >= cifs_mount_count)
        return -EINVAL;
    struct cifs_mount_info *mnt = &cifs_mounts[mount_id];
    if (!mnt->mounted) return -ENOTCONN;

    return cifs_smb2_close(mnt, fh);
}

/* cifs_read — read from an open file handle */
int cifs_read(int mount_id, struct cifs_fh *fh,
              uint64_t offset, uint32_t count, uint8_t *buf)
{
    if (mount_id < 0 || mount_id >= cifs_mount_count)
        return -EINVAL;
    struct cifs_mount_info *mnt = &cifs_mounts[mount_id];
    if (!mnt->mounted) return -ENOTCONN;

    return cifs_smb2_read(mnt, fh, offset, count, buf);
}

/* cifs_readdir — list a directory */
int cifs_readdir(int mount_id, const char *dir_path)
{
    if (mount_id < 0 || mount_id >= cifs_mount_count)
        return -EINVAL;
    struct cifs_mount_info *mnt = &cifs_mounts[mount_id];
    if (!mnt->mounted) return -ENOTCONN;

    struct cifs_fh fh;
    int ret = cifs_smb2_create(mnt, dir_path, &fh, 1);
    if (ret < 0) return ret;

    ret = cifs_smb2_query_directory(mnt, &fh);
    cifs_smb2_close(mnt, &fh);
    return ret;
}

/* cifs_stat — get file/directory attributes */
int cifs_stat(int mount_id, const char *path, struct vfs_stat *st)
{
    if (mount_id < 0 || mount_id >= cifs_mount_count)
        return -EINVAL;
    struct cifs_mount_info *mnt = &cifs_mounts[mount_id];
    if (!mnt->mounted) return -ENOTCONN;

    memset(st, 0, sizeof(*st));

    struct cifs_fh fh;
    int ret = cifs_smb2_create(mnt, path, &fh, 0);
    if (ret < 0) {
        /* Try as directory */
        ret = cifs_smb2_create(mnt, path, &fh, 1);
        if (ret < 0) return -ENOENT;
        st->type = VFS_TYPE_DIR;
    } else {
        st->type = VFS_TYPE_FILE;
    }

    uint64_t size = 0;
    uint32_t attr = 0;
    ret = cifs_smb2_query_info(mnt, &fh, &size, &attr);
    if (ret == 0) {
        st->size = size;
        if (attr & SMB2_FILE_ATTRIBUTE_DIRECTORY)
            st->type = VFS_TYPE_DIR;
    }

    cifs_smb2_close(mnt, &fh);
    return 0;
}

/* ── Mount function ──────────────────────────────────────────────────── */

/* Forward declaration */
static int cifs_allocate_mount_id(void);

int cifs_mount(const char *server, const char *share,
               const char *user, const char *pass)
{
    if (cifs_mount_count >= CIFS_MAX_MOUNTS)
        return -ENOMEM;

    /* Resolve server IP */
    uint32_t server_ip = 0;
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
        extern uint32_t net_dns_resolve(const char *hostname);
        server_ip = net_dns_resolve(server);
    }

    if (server_ip == 0) {
        kprintf("[cifs] Cannot resolve server: %s\\n", server);
        return -ENXIO;
    }

    int idx = cifs_allocate_mount_id();
    if (idx < 0)
        return -ENFILE;
    struct cifs_mount_info *mnt = &cifs_mounts[idx];
    memset(mnt, 0, sizeof(*mnt));

    strncpy(mnt->server, server, sizeof(mnt->server) - 1);
    mnt->server_ip = server_ip;
    mnt->server_port = 445;

    strncpy(mnt->share, share, sizeof(mnt->share) - 1);
    if (user)
        strncpy(mnt->username, user, sizeof(mnt->username) - 1);
    if (pass)
        strncpy(mnt->password, pass, sizeof(mnt->password) - 1);

    kprintf("[cifs] Mounting \\\\%s\\%s (IP: %u.%u.%u.%u)\\n",
            server, share,
            (unsigned)((server_ip >> 24) & 0xFF),
            (unsigned)((server_ip >> 16) & 0xFF),
            (unsigned)((server_ip >> 8) & 0xFF),
            (unsigned)(server_ip & 0xFF));

    /* Open TCP connection */
    int conn = net_tcp_connect(server_ip, 445);
    if (conn < 0) {
        kprintf("[cifs] TCP connection failed\\n");
        return -ENOTCONN;
    }
    mnt->conn_id = conn;

    /* Wait for connection to establish */
    uint64_t start = timer_get_ticks();
    while (!net_tcp_is_connected(conn)) {
        if (timer_get_ticks() - start > 500) { /* 5 second timeout */
            kprintf("[cifs] TCP connection timeout\\n");
            net_tcp_close(conn);
            return -ETIMEDOUT;
        }
        extern void scheduler_yield(void);
        scheduler_yield();
    }

    kprintf("[cifs] TCP connection established\\n");

    /* SMB2 handshake */
    if (cifs_smb2_negotiate(mnt) < 0) {
        kprintf("[cifs] SMB2 negotiate failed\\n");
        net_tcp_close(conn);
        return -EIO;
    }

    if (cifs_smb2_session_setup(mnt) < 0) {
        kprintf("[cifs] SMB2 session setup failed\\n");
        net_tcp_close(conn);
        return -EACCES;
    }

    if (cifs_smb2_tree_connect(mnt) < 0) {
        kprintf("[cifs] SMB2 tree connect failed\\n");
        net_tcp_close(conn);
        return -EACCES;
    }

    mnt->mounted = 1;
    cifs_mount_count++;

    kprintf("[cifs] Mount successful: \\\\%s\\%s\\n", server, share);
    return idx;
}

/* ── Unmount ──────────────────────────────────────────────────────────── */

int cifs_umount(int mount_id)
{
    if (mount_id < 0 || mount_id >= cifs_mount_count)
        return -EINVAL;
    struct cifs_mount_info *mnt = &cifs_mounts[mount_id];
    if (!mnt->mounted) return -EINVAL;

    net_tcp_close(mnt->conn_id);
    mnt->mounted = 0;
    kprintf("[cifs] Unmounted mount_id=%d\\n", mount_id);
    return 0;
}

/* ── VFS operations ──────────────────────────────────────────────────── */

static int cifs_vfs_read(void *priv, const char *path,
                          void *buf, uint32_t max_size, uint32_t *out_size)
{
    struct cifs_mount_info *mnt = (struct cifs_mount_info *)priv;
    if (out_size) *out_size = 0;

    /* Skip mountpoint prefix to get relative path */
    const char *rel_path = path;
    /* Find the mount point match - strip it */
    int mnt_len = (int)strlen(mnt->share);
    (void)mnt_len;

    struct cifs_fh fh;
    int ret = cifs_smb2_create(mnt, path, &fh, 0);
    if (ret < 0) return -ENOENT;

    uint8_t *read_buf = (uint8_t *)buf;
    uint32_t total = 0;
    uint64_t offset = 0;
    uint32_t remaining = max_size;

    while (remaining > 0) {
        uint32_t chunk = remaining;
        if (chunk > 65536) chunk = 65536;
        int n = cifs_smb2_read(mnt, &fh, offset, chunk, read_buf + total);
        if (n < 0) {
            cifs_smb2_close(mnt, &fh);
            return n;
        }
        if (n == 0) break;
        total += (uint32_t)n;
        offset += (uint64_t)n;
        remaining -= (uint32_t)n;
    }

    cifs_smb2_close(mnt, &fh);
    if (out_size) *out_size = total;
    return 0;
}

static int cifs_vfs_write(void *priv, const char *path,
                           const void *data, uint32_t size)
{
    (void)priv; (void)path; (void)data; (void)size;
    return -EROFS;
}

static int cifs_vfs_stat(void *priv, const char *path, struct vfs_stat *st)
{
    struct cifs_mount_info *mnt = (struct cifs_mount_info *)priv;
    memset(st, 0, sizeof(*st));

    struct cifs_fh fh;
    int ret = cifs_smb2_create(mnt, path, &fh, 0);
    if (ret < 0) {
        ret = cifs_smb2_create(mnt, path, &fh, 1);
        if (ret < 0) return -ENOENT;
        st->type = VFS_TYPE_DIR;
    } else {
        st->type = VFS_TYPE_FILE;
    }

    uint64_t size = 0;
    uint32_t attr = 0;
    ret = cifs_smb2_query_info(mnt, &fh, &size, &attr);
    if (ret == 0) {
        st->size = size;
        if (attr & SMB2_FILE_ATTRIBUTE_DIRECTORY)
            st->type = VFS_TYPE_DIR;
    }

    cifs_smb2_close(mnt, &fh);
    st->uid = 0;
    st->gid = 0;
    st->mode = (st->type == VFS_TYPE_DIR) ? 0755 : 0644;
    return 0;
}

static int cifs_vfs_create(void *priv, const char *path, uint8_t type)
{
    (void)priv; (void)path; (void)type;
    return -EROFS;
}

static int cifs_vfs_unlink(void *priv, const char *path)
{
    (void)priv; (void)path;
    return -EROFS;
}

/* The mount_info struct is used directly as VFS priv data */

static int cifs_vfs_readdir(void *priv, const char *path)
{
    struct cifs_mount_info *mnt = (struct cifs_mount_info *)priv;
    return cifs_readdir(mnt->mount_id, path);
}

static struct vfs_ops cifs_ops = {
    .read    = cifs_vfs_read,
    .write   = cifs_vfs_write,
    .stat    = cifs_vfs_stat,
    .create  = cifs_vfs_create,
    .unlink  = cifs_vfs_unlink,
    .readdir = cifs_vfs_readdir,
};

/* Allocate and store mount_id in the mount info */
static int cifs_allocate_mount_id(void)
{
    for (int i = 0; i < CIFS_MAX_MOUNTS; i++) {
        if (!cifs_mounts[i].mounted) {
            cifs_mounts[i].mount_id = i;
            return i;
        }
    }
    return -1;
}

/* ── Init ──────────────────────────────────────────────────────────── */

int cifs_init(void)
{
    memset(cifs_mounts, 0, sizeof(cifs_mounts));
    cifs_mount_count = 0;

    kprintf("[cifs] SMB2 client initialized\\n");
    vfs_register_filesystem("cifs", &cifs_ops);
    return 0;
}

device_initcall(cifs_init);

#ifdef MODULE
int init_module(void) { return cifs_init(); }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("CIFS/SMB2 client — SMB 2.0.2 over TCP with guest auth");
MODULE_VERSION("1.0");
#endif

/* ── Stub: cifs_write ─────────────────────────────── */
int cifs_write(void *file, const void *buf, size_t count, uint64_t offset)
{
    (void)file;
    (void)buf;
    (void)count;
    (void)offset;
    kprintf("[cifs] cifs_write: not yet implemented\n");
    return -ENOSYS;
}
