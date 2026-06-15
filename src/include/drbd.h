#ifndef DRBD_H
#define DRBD_H

#include "types.h"

/* ── Constants ─────────────────────────────────────────────────── */

#define DRBD_PORT               7788
#define DRBD_MAX_RESOURCES      1
#define DRBD_SECTOR_SIZE        512

/* DRBD packet types */
#define P_DATA                  0x01
#define P_DATA_REPLY            0x02
#define P_ACK                   0x03
#define P_BARRIER               0x04
#define P_CONN_STATE            0x05
#define P_SYNC_REQ              0x06
#define P_SYNC_DATA             0x07

/* DRBD connection states */
#define DRBD_STATE_STANDALONE   0
#define DRBD_STATE_CONNECTING   1
#define DRBD_STATE_CONNECTED    2
#define DRBD_STATE_SYNC_SOURCE  3
#define DRBD_STATE_SYNC_TARGET  4

/* DRBD disk states */
#define DRBD_DISK_DISKLESS     0
#define DRBD_DISK_INCONSISTENT 1
#define DRBD_DISK_OUTDATED     2
#define DRBD_DISK_CONSISTENT   3
#define DRBD_DISK_UP_TO_DATE   4

/* ── Structures ───────────────────────────────────────────────── */

/* DRBD packet header */
struct drbd_packet_hdr {
    uint16_t magic;
    uint16_t type;       /* P_DATA, P_ACK, etc. */
    uint32_t len;        /* total packet length including header */
    uint64_t sector;     /* sector number */
    uint32_t count;      /* sector count */
    uint32_t seq;        /* sequence number */
    uint8_t  _rsv[4];
} __attribute__((packed));

#define DRBD_MAGIC  0x8372  /* BD -> DRBD magic */

/* DRBD resource */
struct drbd_resource {
    char     name[32];
    int      active;
    int      conn_id;         /* TCP connection ID to peer */
    uint32_t peer_ip;
    uint16_t peer_port;

    int      local_dev_id;    /* block device ID for local backing store */
    int      drbd_dev_id;     /* block device ID for DRBD device */

    /* State machine */
    int      conn_state;      /* DRBD_STATE_* */
    int      disk_state;

    /* Replication tracking */
    uint64_t sector_count;
    uint32_t pending_writes;
    uint32_t last_seq;        /* last sequence number sent */

    /* Stats */
    uint64_t writes_replicated;
    uint64_t writes_acked;
    uint64_t bytes_sent;
};

/* ── Public API ─────────────────────────────────────────────────── */

void drbd_init(void);
int  drbd_create_resource(const char *name, int local_dev_id);
int  drbd_connect_peer(int res_id, uint32_t peer_ip, uint16_t port);
void drbd_disconnect(int res_id);
void drbd_poll(void);
int  drbd_get_state(int res_id, int *conn_state, int *disk_state);

#endif /* DRBD_H */
