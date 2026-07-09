#ifndef NBD_H
#define NBD_H

#include "types.h"

/* NBD protocol constants */
#define NBD_MAGIC         0x4E42444D41474943ULL  /* "NBDMAGIC" */
#define NBD_MAGIC_IHAVEOPT 0x49484156454F5054ULL /* "IHAVEOPT" */

#define NBD_PORT          10809

/* NBD option types (client → server during handshake) */
#define NBD_OPT_EXPORT_NAME  1

/* NBD command types */
#define NBD_CMD_READ   0
#define NBD_CMD_WRITE  1
#define NBD_CMD_FLUSH  3
#define NBD_CMD_TRIM   4

/* NBD reply error codes */
#define NBD_SUCCESS    0
#define NBD_EPERM      1
#define NBD_EIO        5
#define NBD_ENOMEM     12
#define NBD_EINVAL     22
#define NBD_ENOSPC     28

/* NBD request (simple) */
struct nbd_request {
    uint32_t magic;    /* 0x25609513 */
    uint32_t type;     /* NBD_CMD_* */
    uint64_t handle;
    uint64_t offset;
    uint32_t len;
} __attribute__((packed));

#define NBD_REQUEST_MAGIC 0x25609513

/* NBD reply (simple) */
struct nbd_reply {
    uint32_t magic;    /* 0x67446698 */
    uint32_t error;
    uint64_t handle;
} __attribute__((packed));

#define NBD_REPLY_MAGIC 0x67446698

/* NBD export info (returned from old-style negotiation).
 * Layout at buf[8..127] in the 128-byte server handshake:
 *   export_size (8) + flags (4) + reserved (108) = 120 bytes,
 * prefixed by buf[0..7] = "NBDMAGIC". */
struct nbd_export_info {
    uint64_t export_size;
    uint32_t flags;
    uint8_t  reserved[108];
} __attribute__((packed));

/* Block device operations for the NBD device */
struct nbd_device {
    int     conn_id;        /* TCP connection ID */
    int     dev_id;         /* block device ID */
    uint64_t export_size;   /* size in bytes */
    uint32_t flags;         /* NBD flags */
    int     connected;
};

/* ── Public API ─────────────────────────────────────────────────────── */

/* Initialize the NBD subsystem */
void nbd_init(void);

/* Connect to an NBD server and expose as a block device.
 * Returns the block device ID on success, -1 on error. */
int  nbd_connect(uint32_t server_ip);

/* Disconnect from an NBD server and unregister the block device. */
void nbd_disconnect(int dev_id);

#endif /* NBD_H */
