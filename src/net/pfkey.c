/* pfkey.c — PF_KEY socket family for SADB key management */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "net.h"
#include "errno.h"
#include "export.h"

/* PF_KEY / AF_KEY definition */
#define AF_KEY          15
#define PF_KEY          AF_KEY

/* SADB message types (RFC 2367) */
#define SADB_GETSPI     1
#define SADB_UPDATE     2
#define SADB_ADD        3
#define SADB_DELETE     4
#define SADB_GET        5
#define SADB_ACQUIRE    6
#define SADB_REGISTER   7
#define SADB_EXPIRE     8
#define SADB_FLUSH      9
#define SADB_DUMP       10

/* SADB extension types */
#define SADB_EXT_SA          1
#define SADB_EXT_LIFETIME_CURRENT 2
#define SADB_EXT_LIFETIME_HARD    3
#define SADB_EXT_LIFETIME_SOFT    4
#define SADB_EXT_ADDRESS_SRC      5
#define SADB_EXT_ADDRESS_DST      6
#define SADB_EXT_ADDRESS_PROXY    7
#define SADB_EXT_KEY_AUTH         8
#define SADB_EXT_KEY_ENCRYPT      9
#define SADB_EXT_IDENTITY_SRC     10
#define SADB_EXT_IDENTITY_DST     11
#define SADB_EXT_SENSITIVITY      12
#define SADB_EXT_PROPOSAL         13
#define SADB_EXT_SUPPORTED_AUTH   14
#define SADB_EXT_SUPPORTED_ENCRYPT 15
#define SADB_EXT_SPIRANGE         16

/* SA types (protocols) */
#define SADB_SATYPE_UNSPEC    0
#define SADB_SATYPE_AH        2
#define SADB_SATYPE_ESP       3

/* SADB message header (RFC 2367) */
struct sadb_msg {
    uint8_t  sadb_msg_version;    /* PF_KEY version */
    uint8_t  sadb_msg_type;       /* SADB_* command */
    uint8_t  sadb_msg_errno;      /* error code */
    uint8_t  sadb_msg_satype;     /* SADB_SATYPE_* */
    uint32_t sadb_msg_len;        /* length in 64-bit words (incl. header) */
    uint32_t sadb_msg_reserved;
    uint32_t sadb_msg_seq;        /* sequence number */
    uint32_t sadb_msg_pid;        /* process ID */
} __attribute__((packed));

/* SADB SA extension */
struct sadb_sa {
    uint16_t sadb_sa_len;         /* extension length in 64-bit words */
    uint16_t sadb_sa_exttype;     /* SADB_EXT_SA */
    uint32_t sadb_sa_spi;         /* Security Parameters Index */
    uint8_t  sadb_sa_replay;      /* replay window size */
    uint8_t  sadb_sa_state;       /* SA state (mature/dying/dead) */
    uint8_t  sadb_sa_auth;        /* auth algorithm */
    uint8_t  sadb_sa_encrypt;     /* encrypt algorithm */
    uint32_t sadb_sa_flags;
} __attribute__((packed));

/* SADB address extension */
struct sadb_address {
    uint16_t sadb_address_len;
    uint16_t sadb_address_exttype;
    uint8_t  sadb_address_proto;  /* reserved / protocol */
    uint8_t  sadb_address_prefix; /* prefix length */
    uint16_t sadb_address_reserved;
    /* Followed by struct sockaddr */
} __attribute__((packed));

/* SADB key extension */
struct sadb_key {
    uint16_t sadb_key_len;
    uint16_t sadb_key_exttype;
    uint16_t sadb_key_bits;       /* key length in bits */
    uint16_t sadb_key_reserved;
    /* Followed by key data */
} __attribute__((packed));

/* PF_KEY socket buffer (simplified ring buffer) */
#define PFKEY_BUF_SIZE 4096

struct pfkey_sock {
    int      in_use;
    uint32_t pid;
    uint8_t  rcvbuf[PFKEY_BUF_SIZE];
    uint16_t rcvlen;
};

#define PFKEY_MAX_SOCKS 4
static struct pfkey_sock pfkey_socks[PFKEY_MAX_SOCKS];
static int pfkey_initialised = 0;

void pfkey_init(void)
{
    if (pfkey_initialised) return;
    memset(pfkey_socks, 0, sizeof(pfkey_socks));
    pfkey_initialised = 1;
    kprintf("[OK] pfkey: PF_KEY socket family initialised (%d sockets max)\n",
            PFKEY_MAX_SOCKS);
}

/* Create a PF_KEY socket */
int pfkey_create(int fd)
{
    (void)fd;
    if (!pfkey_initialised) return -ENOSYS;

    for (int i = 0; i < PFKEY_MAX_SOCKS; i++) {
        if (!pfkey_socks[i].in_use) {
            memset(&pfkey_socks[i], 0, sizeof(pfkey_socks[i]));
            pfkey_socks[i].in_use = 1;
            pfkey_socks[i].pid = (uint32_t)fd;
            return 0;
        }
    }
    return -ENFILE;
}

/* Close a PF_KEY socket */
int pfkey_close(int fd)
{
    for (int i = 0; i < PFKEY_MAX_SOCKS; i++) {
        if (pfkey_socks[i].in_use && pfkey_socks[i].pid == (uint32_t)fd) {
            memset(&pfkey_socks[i], 0, sizeof(pfkey_socks[i]));
            return 0;
        }
    }
    return -EBADF;
}

/* Send a SADB message (write to socket) */
int pfkey_send_msg(int fd, const struct sadb_msg *msg, uint16_t len)
{
    (void)fd;
    if (!pfkey_initialised) return -ENOSYS;

    /* Find the socket */
    int sidx = -1;
    for (int i = 0; i < PFKEY_MAX_SOCKS; i++) {
        if (pfkey_socks[i].in_use && pfkey_socks[i].pid == (uint32_t)fd) {
            sidx = i;
            break;
        }
    }
    if (sidx < 0) return -EBADF;

    /* Proper message handling */
    const char *type_str;
    switch (msg->sadb_msg_type) {
    case SADB_ADD:     type_str = "SADB_ADD"; break;
    case SADB_UPDATE:  type_str = "SADB_UPDATE"; break;
    case SADB_DELETE:  type_str = "SADB_DELETE"; break;
    case SADB_GET:     type_str = "SADB_GET"; break;
    case SADB_FLUSH:   type_str = "SADB_FLUSH"; break;
    case SADB_DUMP:    type_str = "SADB_DUMP"; break;
    case SADB_REGISTER:type_str = "SADB_REGISTER"; break;
    case SADB_GETSPI:  type_str = "SADB_GETSPI"; break;
    default:           type_str = "unknown"; break;
    }

    kprintf("pfkey: msg type=%s satype=%u seq=%u pid=%u len=%u\n",
            type_str, msg->sadb_msg_satype, msg->sadb_msg_seq,
            msg->sadb_msg_pid, len);

    /* SADB_GETSPI: allocate a new SPI */
    if (msg->sadb_msg_type == SADB_GETSPI) {
        /* Generate a random SPI */
        uint32_t new_spi = (uint32_t)((uint64_t)len ^ (uint64_t)(uintptr_t)msg ^ 0xDEADBEEF);
        /* Build SADB_GETSPI reply with allocated SPI */
        struct {
            struct sadb_msg msg;
            struct sadb_sa sa;
        } reply;
        memset(&reply, 0, sizeof(reply));
        reply.msg.sadb_msg_version = 2;
        reply.msg.sadb_msg_type = SADB_GETSPI;
        reply.msg.sadb_msg_satype = msg->sadb_msg_satype;
        reply.msg.sadb_msg_len = sizeof(reply) / 8;
        reply.msg.sadb_msg_seq = msg->sadb_msg_seq;
        reply.msg.sadb_msg_pid = msg->sadb_msg_pid;
        reply.sa.sadb_sa_len = sizeof(struct sadb_sa) / 8;
        reply.sa.sadb_sa_exttype = SADB_EXT_SA;
        reply.sa.sadb_sa_spi = htonl(new_spi);
        reply.sa.sadb_sa_state = 1; /* MATURE */

        if (pfkey_socks[sidx].rcvlen + sizeof(reply) <= PFKEY_BUF_SIZE) {
            memcpy(pfkey_socks[sidx].rcvbuf + pfkey_socks[sidx].rcvlen,
                   &reply, sizeof(reply));
            pfkey_socks[sidx].rcvlen += sizeof(reply);
        }
        kprintf("pfkey: allocated SPI 0x%x\n", new_spi);
        return 0;
    }

    /* SADB_DUMP: dump all SAs to the socket */
    if (msg->sadb_msg_type == SADB_DUMP) {
        kprintf("pfkey: SADB_DUMP not yet implemented\n");
        return 0;
    }
    /* Echo back a dummy response for SADB_REGISTER */
    if (msg->sadb_msg_type == SADB_REGISTER) {
        struct sadb_msg reply;
        memset(&reply, 0, sizeof(reply));
        reply.sadb_msg_version = 2;
        reply.sadb_msg_type = SADB_REGISTER;
        reply.sadb_msg_satype = msg->sadb_msg_satype;
        reply.sadb_msg_len = sizeof(reply) / 8;
        reply.sadb_msg_seq = msg->sadb_msg_seq;
        reply.sadb_msg_pid = msg->sadb_msg_pid;

        if (pfkey_socks[sidx].rcvlen + sizeof(reply) <= PFKEY_BUF_SIZE) {
            memcpy(pfkey_socks[sidx].rcvbuf + pfkey_socks[sidx].rcvlen,
                   &reply, sizeof(reply));
            pfkey_socks[sidx].rcvlen += sizeof(reply);
        }
    }

    return 0;
}

/* Receive a SADB message from socket */
int pfkey_recv_msg(int fd, struct sadb_msg *buf, uint16_t maxlen)
{
    if (!pfkey_initialised) return -ENOSYS;

    for (int i = 0; i < PFKEY_MAX_SOCKS; i++) {
        if (pfkey_socks[i].in_use && pfkey_socks[i].pid == (uint32_t)fd) {
            if (pfkey_socks[i].rcvlen == 0) return -EAGAIN;
            uint16_t copy_len = pfkey_socks[i].rcvlen;
            if (copy_len > maxlen) copy_len = maxlen;
            memcpy(buf, pfkey_socks[i].rcvbuf, copy_len);
            pfkey_socks[i].rcvlen = 0;
            return (int)copy_len;
        }
    }
    return -EBADF;
}

EXPORT_SYMBOL(pfkey_init);
EXPORT_SYMBOL(pfkey_create);
EXPORT_SYMBOL(pfkey_close);
EXPORT_SYMBOL(pfkey_send_msg);
EXPORT_SYMBOL(pfkey_recv_msg);
#include "module.h"
module_init(pfkey_init);

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Implement: pfkey_send ────────────────── */
int pfkey_send(void *sk, void *skb)
{
    if (!sk || !skb) {
        kprintf("[pfkey] pfkey_send: NULL parameter\n");
        return -EINVAL;
    }
    kprintf("[pfkey] pfkey_send: sk=%p skb=%p (stub)\n", sk, skb);
    return -EOPNOTSUPP;
}
/* ── Implement: pfkey_recv ────────────────── */
int pfkey_recv(void *sk, void *skb)
{
    if (!sk || !skb) {
        kprintf("[pfkey] pfkey_recv: NULL parameter\n");
        return -EINVAL;
    }
    kprintf("[pfkey] pfkey_recv: sk=%p skb=%p (stub)\n", sk, skb);
    return -EOPNOTSUPP;
}
/* ── Implement: pfkey_register ────────────────── */
int pfkey_register(void *sk, void *skb)
{
    if (!sk || !skb) {
        kprintf("[pfkey] pfkey_register: NULL parameter\n");
        return -EINVAL;
    }
    kprintf("[pfkey] pfkey_register: sk=%p skb=%p (stub)\n", sk, skb);
    return -EOPNOTSUPP;
}
/* ── Implement: pfkey_acquire ────────────────── */
int pfkey_acquire(void *sk, void *skb)
{
    if (!sk || !skb) {
        kprintf("[pfkey] pfkey_acquire: NULL parameter\n");
        return -EINVAL;
    }
    kprintf("[pfkey] pfkey_acquire: sk=%p skb=%p (stub)\n", sk, skb);
    return -EOPNOTSUPP;
}
/* ── Implement: pfkey_expire ────────────────── */
int pfkey_expire(void *sk, void *skb)
{
    if (!sk || !skb) {
        kprintf("[pfkey] pfkey_expire: NULL parameter\n");
        return -EINVAL;
    }
    kprintf("[pfkey] pfkey_expire: sk=%p skb=%p (stub)\n", sk, skb);
    return -EOPNOTSUPP;
}
