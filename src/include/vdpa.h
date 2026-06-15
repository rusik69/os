#ifndef VDPA_H
#define VDPA_H

#include "types.h"

/* ── vDPA device operations ─────────────────────────────────────────── */
struct vdpa_device;

/* Operations a vDPA device must implement */
struct vdpa_ops {
    /* Get the number of virtqueues supported */
    int (*get_vq_num)(struct vdpa_device *dev);

    /* Kick (notify) a specific virtqueue */
    int (*kick_vq)(struct vdpa_device *dev, int vq_idx);

    /* Set DMA mapping for IOVA range */
    int (*set_map)(struct vdpa_device *dev, uint64_t iova,
                   uint64_t size, uint64_t pa, uint32_t flags);

    /* Optional: get/set virtqueue state */
    int (*get_vq_state)(struct vdpa_device *dev, int vq_idx, uint16_t *avail_idx);
    int (*set_vq_state)(struct vdpa_device *dev, int vq_idx, uint16_t avail_idx);

    /* Optional: device-specific configuration space read/write */
    int (*get_config)(struct vdpa_device *dev, uint32_t offset, void *buf, uint32_t len);
    int (*set_config)(struct vdpa_device *dev, uint32_t offset, const void *buf, uint32_t len);

    /* Optional: feature negotiation */
    uint64_t (*get_features)(struct vdpa_device *dev);
    int (*set_features)(struct vdpa_device *dev, uint64_t features);

    /* Optional: device status */
    uint8_t (*get_status)(struct vdpa_device *dev);
    void (*set_status)(struct vdpa_device *dev, uint8_t status);

    /* Cleanup */
    void (*release)(struct vdpa_device *dev);
};

/* ── vDPA device descriptor (for registration) ──────────────────────── */
struct vdpa_device {
    const char       *name;
    const struct vdpa_ops *ops;
    void             *private_data;  /* driver-specific data */

    /* virtio-vdpa bus state */
    uint64_t          features;       /* negotiated features */
    uint8_t           status;         /* device status */
    int               num_vqs;        /* number of virtqueues */
    int               registered;     /* 1 = registered with the framework */
};

/* ── vDPA virtqueue descriptor (software-emulated) ──────────────────── */
struct vdpa_virtqueue {
    uint16_t          idx;
    uint16_t          size;
    uint64_t          desc_addr;    /* guest physical address of descriptor area */
    uint64_t          avail_addr;   /* guest physical address of avail ring */
    uint64_t          used_addr;    /* guest physical address of used ring */
    uint16_t          last_avail_idx;
    uint16_t          last_used_idx;
};

/* ── Software vDPA device (backed by kernel memory / block device) ──── */
struct vdpa_soft_device {
    struct vdpa_device    vdev;
    struct vdpa_virtqueue vqs[8];     /* up to 8 virtqueues */
    int                   num_vqs;
    uint64_t              features;
    uint8_t               status;
    uint8_t               config_space[256];

    /* Device-specific (block device backing) */
    uint8_t              *blk_data;
    uint64_t              blk_sectors;
    int                   readonly;
    char                  serial[20];
};

/* ── Public API ─────────────────────────────────────────────────────── */
int vdpa_init(void);
int vdpa_device_register(struct vdpa_device *dev);
int vdpa_device_unregister(struct vdpa_device *dev);

/* Software (no-hardware) vDPA device creation */
struct vdpa_soft_device *vdpa_soft_device_create(const char *name,
                                                   uint8_t *blk_data,
                                                   uint64_t blk_sectors);
int vdpa_soft_device_destroy(struct vdpa_soft_device *sdev);

/* virtio-vdpa bus adapter: makes a vDPA device appear as a virtio device */
int virtio_vdpa_init(void);
int virtio_vdpa_add_device(struct vdpa_device *vdev);

#endif /* VDPA_H */
