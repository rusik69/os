#ifndef VIRTIO_IOMMU_H
#define VIRTIO_IOMMU_H

#include "types.h"

/* ── Virtio IOMMU device ID ─────────────────────────────────────────── */
#define VIRTIO_ID_IOMMU       23

/* ── Virtio IOMMU feature bits ──────────────────────────────────────── */
#define VIRTIO_IOMMU_F_INPUT_RANGE     (1u << 0)
#define VIRTIO_IOMMU_F_DOMAIN_RANGE    (1u << 1)
#define VIRTIO_IOMMU_F_MAP_UNMAP       (1u << 2)
#define VIRTIO_IOMMU_F_BYPASS          (1u << 3)
#define VIRTIO_IOMMU_F_PROBE           (1u << 4)
#define VIRTIO_IOMMU_F_MMIO            (1u << 5)

/* ── Virtio IOMMU request types ─────────────────────────────────────── */
#define VIRTIO_IOMMU_T_MAP            0
#define VIRTIO_IOMMU_T_UNMAP          1
#define VIRTIO_IOMMU_T_ATTACH         2
#define VIRTIO_IOMMU_T_DETACH         3
#define VIRTIO_IOMMU_T_PROBE          4

/* ── Virtio IOMMU status codes ──────────────────────────────────────── */
#define VIRTIO_IOMMU_S_OK             0
#define VIRTIO_IOMMU_S_IOERR          1
#define VIRTIO_IOMMU_S_UNSUPP         2
#define VIRTIO_IOMMU_S_DEVERR         3
#define VIRTIO_IOMMU_S_INVAL          4
#define VIRTIO_IOMMU_S_RANGE          5
#define VIRTIO_IOMMU_S_NOENT          6
#define VIRTIO_IOMMU_S_FAULT          7
#define VIRTIO_IOMMU_S_NOMEM          8

/* ── Virtio IOMMU map flags ─────────────────────────────────────────── */
#define VIRTIO_IOMMU_MAP_F_READ        (1u << 0)
#define VIRTIO_IOMMU_MAP_F_WRITE       (1u << 1)
#define VIRTIO_IOMMU_MAP_F_MMIO        (1u << 2)
#define VIRTIO_IOMMU_MAP_F_EXEC        (1u << 3)

/* ── Virtio IOMMU request/response structures ───────────────────────── */
#pragma pack(push, 1)
struct virtio_iommu_req_head {
    uint8_t  type;
    uint8_t  reserved[3];
    uint32_t domain;            /* domain ID */
};

struct virtio_iommu_req_tail {
    uint8_t  status;
    uint8_t  reserved[3];
};

/* MAP request (type = VIRTIO_IOMMU_T_MAP) */
struct virtio_iommu_req_map {
    struct virtio_iommu_req_head head;
    uint64_t virt_start;        /* virtual (I/O) start address */
    uint64_t virt_end;          /* virtual (I/O) end address (inclusive) */
    uint64_t phys_start;        /* physical start address */
    uint32_t flags;             /* VIRTIO_IOMMU_MAP_F_* */
    struct virtio_iommu_req_tail tail;
};

/* UNMAP request (type = VIRTIO_IOMMU_T_UNMAP) */
struct virtio_iommu_req_unmap {
    struct virtio_iommu_req_head head;
    uint64_t virt_start;        /* virtual (I/O) start address */
    uint64_t virt_end;          /* virtual (I/O) end address (inclusive) */
    struct virtio_iommu_req_tail tail;
};

/* ATTACH request (type = VIRTIO_IOMMU_T_ATTACH) */
struct virtio_iommu_req_attach {
    struct virtio_iommu_req_head head;
    uint32_t endpoint;          /* endpoint/device ID */
    uint8_t  reserved[4];
    struct virtio_iommu_req_tail tail;
};

/* DETACH request (type = VIRTIO_IOMMU_T_DETACH) */
struct virtio_iommu_req_detach {
    struct virtio_iommu_req_head head;
    uint32_t endpoint;
    uint8_t  reserved[4];
    struct virtio_iommu_req_tail tail;
};
#pragma pack(pop)

/* ── IOMMU mapping entry (software page table) ───────────────────────── */
struct virtio_iommu_map_entry {
    uint64_t virt_start;
    uint64_t virt_end;
    uint64_t phys_start;
    uint32_t flags;
    uint32_t domain;
    int      used;
};

/* ── IOMMU domain descriptor ────────────────────────────────────────── */
struct virtio_iommu_domain {
    uint32_t id;
    int      used;
    int      num_endpoints;
    uint32_t endpoints[16];     /* device/endpoint IDs attached */
};

/* ── Public API ─────────────────────────────────────────────────────── */
int  virtio_iommu_init(void);
int  virtio_iommu_map(uint64_t virt_start, uint64_t virt_end,
                      uint64_t phys_start, uint32_t flags);
int  virtio_iommu_unmap(uint64_t virt_start);
int  virtio_iommu_attach(uint32_t domain_id, uint32_t endpoint);
int  virtio_iommu_detach(uint32_t domain_id, uint32_t endpoint);
int  virtio_iommu_handle_request(int vq_idx);
void virtio_iommu_cleanup(void);

#endif /* VIRTIO_IOMMU_H */
