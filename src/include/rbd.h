#ifndef RBD_H
#define RBD_H

#include "types.h"

/* ── Constants ─────────────────────────────────────────────────── */

#define CEPH_MON_PORT           6789
#define CEPH_OSD_PORT           6800
#define RBD_MAX_DEVICES         4

/* Ceph message framing */
#define CEPH_MSGR_TAG_READY     0x02
#define CEPH_MSGR_TAG_MSG       0x03
#define CEPH_MSGR_TAG_ACK       0x04
#define CEPH_MSGR_TAG_CLOSE     0x05

/* Ceph protocol version */
#define CEPH_PROTOCOL_VERSION   1

/* OSD operation codes */
#define CEPH_OSD_OP_READ        0x01
#define CEPH_OSD_OP_WRITE       0x02
#define CEPH_OSD_OP_STAT        0x03

/* Ceph entity types */
#define CEPH_ENTITY_TYPE_CLIENT 1
#define CEPH_ENTITY_TYPE_OSD    2
#define CEPH_ENTITY_TYPE_MON    3

/* RBD object size (4 MB) */
#define RBD_OBJECT_SIZE         (4ULL * 1024 * 1024)

/* ── Structures ───────────────────────────────────────────────── */

/* Ceph message header (on-wire) */
struct ceph_msg_header {
    uint64_t seq;          /* message sequence number */
    uint64_t tid;          /* transaction ID */
    uint16_t type;         /* message type */
    uint16_t _priority;
    uint32_t version;
    uint32_t front_len;    /* front segment length */
    uint32_t middle_len;
    uint32_t data_len;     /* data segment length */
    uint16_t data_off;
    uint8_t  tag;          /* CEPH_MSGR_TAG_* */
    uint8_t  _more;
    uint64_t _seq_lib;
} __attribute__((packed));

/* Ceph message footer */
struct ceph_msg_footer {
    uint32_t front_crc;
    uint32_t middle_crc;
    uint32_t data_crc;
    uint8_t  _flags;
} __attribute__((packed));

/* Ceph OSD request (simplified) */
struct ceph_osd_request {
    uint32_t op;           /* CEPH_OSD_OP_* */
    uint64_t offset;
    uint64_t length;
    uint64_t object_id;    /* simplified: numeric object ID */
} __attribute__((packed));

/* Ceph OSD response (simplified) */
struct ceph_osd_response {
    int32_t  ret;          /* return code */
    uint32_t data_len;
} __attribute__((packed));

/* RADOS object mapping for RBD */
struct rbd_object_info {
    uint64_t object_no;    /* object number within image */
    char     name[64];     /* object name like "rbd_data.1.1234" */
};

/* RBD device state */
struct rbd_device {
    int      connected;
    int      dev_id;          /* block device ID */

    /* Ceph cluster connection state */
    int      mon_conn_id;     /* monitor TCP connection */
    int      osd_conn_id;     /* OSD TCP connection */

    /* Connection state */
    uint64_t global_seq;
    uint64_t client_inc;
    uint64_t osd_epoch;

    /* Image info */
    uint64_t image_size;      /* total image size in bytes */
    uint64_t sector_count;
    uint32_t sector_size;

    /* OSD map (simplified: single OSD) */
    uint32_t osd_ip;
    uint16_t osd_port;

    /* Monitor address */
    uint32_t mon_ip;
    uint16_t mon_port;

    /* RBD object mapping */
    uint64_t num_objects;
};

/* ── Public API ─────────────────────────────────────────────────── */

void rbd_init(void);
int  rbd_connect(uint32_t mon_ip, uint32_t osd_ip);
void rbd_disconnect(int dev_id);

#endif /* RBD_H */
