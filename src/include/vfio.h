#ifndef VFIO_H
#define VFIO_H

#include "types.h"

/* ── VFIO API version ──────────────────────────────────────────────── */
#define VFIO_API_VERSION 0

/* ── VFIO type definitions for ioctl encoding ──────────────────────── */
#define VFIO_TYPE   (';')
#define VFIO_BASE   100

/* ── Helper macros for ioctl encoding ──────────────────────────────── */
#ifndef _IO
#define _IO(type, nr)        (((type) << 8) | (nr))
#endif
#ifndef _IOR
#define _IOR(type, nr, size) (((type) << 8) | (nr) | 0x80000000UL)
#endif
#ifndef _IOW
#define _IOW(type, nr, size) (((type) << 8) | (nr) | 0x40000000UL)
#endif
#define VFIO_GET_API_VERSION       _IO(VFIO_TYPE, VFIO_BASE + 0)
#define VFIO_CHECK_EXTENSION       _IO(VFIO_TYPE, VFIO_BASE + 1)
#define VFIO_SET_IOMMU             _IOW(VFIO_TYPE, VFIO_BASE + 2, int32_t)
#define VFIO_GROUP_GET_STATUS      _IOR(VFIO_TYPE, VFIO_BASE + 3, struct vfio_group_status)
#define VFIO_GROUP_SET_CONTAINER   _IOW(VFIO_TYPE, VFIO_BASE + 4, int32_t)
#define VFIO_GROUP_UNSET_CONTAINER _IO(VFIO_TYPE, VFIO_BASE + 5)
#define VFIO_GROUP_GET_DEVICE_FD   _IOW(VFIO_TYPE, VFIO_BASE + 6, char[256])
#define VFIO_DEVICE_GET_INFO       _IOR(VFIO_TYPE, VFIO_BASE + 7, struct vfio_device_info)
#define VFIO_DEVICE_GET_REGION_INFO _IOR(VFIO_TYPE, VFIO_BASE + 8, struct vfio_region_info)
#define VFIO_DEVICE_GET_IRQ_INFO   _IOR(VFIO_TYPE, VFIO_BASE + 9, struct vfio_irq_info)
#define VFIO_DEVICE_SET_IRQS       _IOW(VFIO_TYPE, VFIO_BASE + 10, struct vfio_irq_set)
#define VFIO_IOMMU_MAP_DMA         _IOW(VFIO_TYPE, VFIO_BASE + 11, struct vfio_iommu_type1_dma_map)
#define VFIO_IOMMU_UNMAP_DMA       _IOW(VFIO_TYPE, VFIO_BASE + 12, struct vfio_iommu_type1_dma_unmap)

/* ── VFIO extension identifiers ────────────────────────────────────── */
#define VFIO_TYPE1_IOMMU           1
#define VFIO_SPAPR_TCE_IOMMU       2
#define VFIO_TYPE1v2_IOMMU         3
#define VFIO_DMA_CC_IOMMU          4
#define VFIO_NOIOMMU_IOMMU         8

/* ── VFIO group status flags ────────────────────────────────────────── */
#define VFIO_GROUP_FLAGS_VIABLE     (1u << 0)
#define VFIO_GROUP_FLAGS_CONTAINER_SET (1u << 1)

struct vfio_group_status {
    uint32_t argsz;
    uint32_t flags;
};

/* ── VFIO device info ───────────────────────────────────────────────── */
#define VFIO_DEVICE_FLAGS_RESET      (1u << 0)
#define VFIO_DEVICE_FLAGS_PCI        (1u << 1)
#define VFIO_DEVICE_FLAGS_PLATFORM   (1u << 2)
#define VFIO_DEVICE_FLAGS_AMBA       (1u << 3)
#define VFIO_DEVICE_FLAGS_CCW        (1u << 4)
#define VFIO_DEVICE_FLAGS_AP         (1u << 5)

struct vfio_device_info {
    uint32_t argsz;
    uint32_t flags;
    uint32_t num_regions;
    uint32_t num_irqs;
};

/* ── VFIO region info ───────────────────────────────────────────────── */
struct vfio_region_info {
    uint32_t argsz;
    uint32_t flags;
    uint32_t index;
    uint32_t cap_offset;
    uint64_t size;
    uint64_t offset;
};

/* ── VFIO IRQ info ──────────────────────────────────────────────────── */
struct vfio_irq_info {
    uint32_t argsz;
    uint32_t flags;
    uint32_t index;
    uint32_t count;
};

struct vfio_irq_set {
    uint32_t argsz;
    uint32_t flags;
    uint32_t index;
    uint32_t start;
    uint32_t count;
    uint32_t padding;
    /* Followed by data for count entries (each uint64_t eventfd) */
};

/* ── VFIO IOMMU type1 DMA map/unmap ──────────────────────────────────── */
struct vfio_iommu_type1_dma_map {
    uint32_t argsz;
    uint32_t flags;
    uint64_t vaddr;     /* virtual address in the process */
    uint64_t iova;      /* IO virtual address (guest physical) */
    uint64_t size;      /* size of the mapping */
};

#define VFIO_DMA_MAP_FLAG_READ  (1u << 0)
#define VFIO_DMA_MAP_FLAG_WRITE (1u << 1)

struct vfio_iommu_type1_dma_unmap {
    uint32_t argsz;
    uint32_t flags;
    uint64_t iova;
    uint64_t size;
};

/* ── Public API ─────────────────────────────────────────────────────── */
int vfio_init(void);
int vfio_container_open(int container_id);
int vfio_group_get_device_fd(int group_id, const char *device_name);
void vfio_cleanup(void);

#endif /* VFIO_H */
