#ifndef VHOST_BLK_H
#define VHOST_BLK_H

#include "types.h"

/* ── vhost-blk constants ───────────────────────────────────────────── */
#define VHOST_BLK_SECTOR_SIZE      512
#define VHOST_BLK_MAX_SEGMENTS     256
#define VHOST_BLK_QUEUE_DEPTH      128

/* vhost-blk request types (matching virtio-blk) */
#define VHOST_BLK_T_IN            0
#define VHOST_BLK_T_OUT           1
#define VHOST_BLK_T_FLUSH         4
#define VHOST_BLK_T_DISCARD       11
#define VHOST_BLK_T_WRITE_ZEROES  13

/* vhost-blk response status */
#define VHOST_BLK_S_OK            0
#define VHOST_BLK_S_IOERR         1
#define VHOST_BLK_S_UNSUPP        2

/* ── vhost-blk request header (matching virtio-blk) ─────────────────── */
#pragma pack(push, 1)
struct vhost_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

/* vhost-blk discard/write_zeroes descriptor */
struct vhost_blk_discard_desc {
    uint64_t sector;
    uint32_t num_sectors;
    uint32_t flags;
};

#define VHOST_BLK_DISCARD_F_UNMAP   (1u << 0)
#pragma pack(pop)

/* ── Backing store descriptor ───────────────────────────────────────── */
struct vhost_blk_backing {
    uint8_t       *data;         /* pointer to block data (512-byte sectors) */
    uint64_t       num_sectors;  /* total sector count */
    int            readonly;
    char           serial[20];   /* device serial (GET_ID returns this) */
};

/* ── virtqueue elements for vhost-blk processing ────────────────────── */
struct vhost_blk_vqe {
    uint64_t desc_addr;          /* address of the descriptor in guest's space */
    uint32_t desc_len;
    uint8_t  desc_flags;         /* VRING_DESC_F_* */
};

/* ── Public API ─────────────────────────────────────────────────────── */
int  vhost_blk_init(void);
int  vhost_blk_set_backing(struct vhost_blk_backing *bak);
int  vhost_blk_handle_kick(int vq_idx);
void vhost_blk_cleanup(void);

#endif /* VHOST_BLK_H */
