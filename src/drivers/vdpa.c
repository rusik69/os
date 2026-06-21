/*
 * src/drivers/vdpa.c — vDPA (virtio Data Path Acceleration) framework
 *
 * Provides an abstract vDPA device model with operation callbacks,
 * device registration, and a virtio-vdpa bus adapter that makes
 * vDPA devices appear as standard virtio devices.
 * Simple software vDPA implementation (no hardware offload).
 */

#include "vdpa.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"
#include "heap.h"

/* ── Internal state ────────────────────────────────────────────────── */

#define VDPA_MAX_DEVICES 8

static struct vdpa_device *vdpa_devices[VDPA_MAX_DEVICES];
static int vdpa_num_devices = 0;
static int vdpa_initialized = 0;

/* ── Default operation callbacks for software vDPA device ──────────── */

static int soft_get_vq_num(struct vdpa_device *dev)
{
    (void)dev;
    return 1; /* single queue */
}

static int soft_kick_vq(struct vdpa_device *dev, int vq_idx)
{
    struct vdpa_soft_device *sdev = (struct vdpa_soft_device *)dev;
    if (vq_idx < 0 || vq_idx >= sdev->num_vqs) return -1;

    kprintf("[vdpa] soft device '%s' kick vq %d\n",
            dev->name, vq_idx);

    /* In a real implementation, this would process the virtqueue:
     *  1. Read descriptors from the avail ring
     *  2. Process the request (read/write to backing store)
     *  3. Update the used ring
     */
    return 0;
}

static int soft_set_map(struct vdpa_device *dev, uint64_t iova,
                         uint64_t size, uint64_t pa, uint32_t flags)
{
    kprintf("[vdpa] soft device '%s' set_map: iova=0x%llx size=%llu "
            "pa=0x%llx flags=0x%x\n",
            dev->name, iova, size, pa, flags);
    return 0;
}

static uint64_t soft_get_features(struct vdpa_device *dev)
{
    struct vdpa_soft_device *sdev = (struct vdpa_soft_device *)dev;
    return sdev->features;
}

static int soft_set_features(struct vdpa_device *dev, uint64_t features)
{
    struct vdpa_soft_device *sdev = (struct vdpa_soft_device *)dev;
    sdev->features = features & dev->features;
    return 0;
}

static uint8_t soft_get_status(struct vdpa_device *dev)
{
    struct vdpa_soft_device *sdev = (struct vdpa_soft_device *)dev;
    return sdev->status;
}

static void soft_set_status(struct vdpa_device *dev, uint8_t status)
{
    struct vdpa_soft_device *sdev = (struct vdpa_soft_device *)dev;
    sdev->status = status;
}

static void soft_release(struct vdpa_device *dev)
{
    (void)dev;
    kprintf("[vdpa] soft device released\n");
}

/* Default vdpa_ops for software devices */
static const struct vdpa_ops vdpa_soft_ops = {
    .get_vq_num    = soft_get_vq_num,
    .kick_vq       = soft_kick_vq,
    .set_map       = soft_set_map,
    .get_features  = soft_get_features,
    .set_features  = soft_set_features,
    .get_status    = soft_get_status,
    .set_status    = soft_set_status,
    .release       = soft_release,
};

/* ── Software vDPA device creation/destruction ─────────────────────── */

struct vdpa_soft_device *vdpa_soft_device_create(const char *name,
                                                   uint8_t *blk_data,
                                                   uint64_t blk_sectors)
{
    /* Allocate from static pool (no heap after init) */
    static struct vdpa_soft_device sdev_pool[VDPA_MAX_DEVICES];
    static int pool_used[VDPA_MAX_DEVICES];

    int idx = -1;
    for (int i = 0; i < VDPA_MAX_DEVICES; i++) {
        if (!pool_used[i]) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return NULL;

    struct vdpa_soft_device *sdev = &sdev_pool[idx];
    memset(sdev, 0, sizeof(*sdev));
    pool_used[idx] = 1;

    /* Set up vdpa_device base */
    sdev->vdev.name         = name;
    sdev->vdev.ops          = &vdpa_soft_ops;
    sdev->vdev.private_data = sdev;
    sdev->vdev.features     = 0;
    sdev->vdev.status       = 0;
    sdev->vdev.num_vqs      = 1;
    sdev->vdev.registered   = 0;

    /* Set up device-specific data */
    sdev->num_vqs     = 1;
    sdev->blk_data    = blk_data;
    sdev->blk_sectors = blk_sectors;

    /* Feature bits */
    sdev->features = (1ULL << 0); /* basic feature */

    kprintf("[vdpa] soft device '%s' created (%llu sectors)\n",
            name, blk_sectors);
    return sdev;
}

int vdpa_soft_device_destroy(struct vdpa_soft_device *sdev)
{
    if (!sdev) return -1;

    if (sdev->vdev.registered)
        vdpa_device_unregister(&sdev->vdev);

    if (sdev->vdev.ops && sdev->vdev.ops->release)
        sdev->vdev.ops->release(&sdev->vdev);

    memset(sdev, 0, sizeof(*sdev));
    kprintf("[vdpa] soft device destroyed\n");
    return 0;
}

/* ── vDPA device registration ─────────────────────────────────────── */

int vdpa_device_register(struct vdpa_device *dev)
{
    if (!dev) return -1;
    if (vdpa_num_devices >= VDPA_MAX_DEVICES) return -1;

    if (!dev->ops) {
        kprintf("[vdpa] ERROR: device '%s' has no ops\n",
                dev->name ? dev->name : "unnamed");
        return -1;
    }

    /* Validate required ops */
    if (!dev->ops->get_vq_num || !dev->ops->kick_vq || !dev->ops->set_map) {
        kprintf("[vdpa] ERROR: device '%s' missing required ops\n",
                dev->name ? dev->name : "unnamed");
        return -1;
    }

    vdpa_devices[vdpa_num_devices++] = dev;
    dev->registered = 1;

    kprintf("[vdpa] device '%s' registered (%d devices total)\n",
            dev->name ? dev->name : "unnamed", vdpa_num_devices);
    return 0;
}

int vdpa_device_unregister(struct vdpa_device *dev)
{
    if (!dev) return -1;

    for (int i = 0; i < vdpa_num_devices; i++) {
        if (vdpa_devices[i] == dev) {
            /* Remove by shifting */
            for (int j = i; j < vdpa_num_devices - 1; j++)
                vdpa_devices[j] = vdpa_devices[j + 1];
            vdpa_num_devices--;
            dev->registered = 0;
            kprintf("[vdpa] device '%s' unregistered\n",
                    dev->name ? dev->name : "unnamed");
            return 0;
        }
    }
    return -1;
}

/* ── virtio-vdpa bus adapter ───────────────────────────────────────── */

/* The virtio-vdpa bus adapter makes a vDPA device appear as a standard
 * virtio device. It translates virtio configuration space reads/writes,
 * feature negotiation, and virtqueue operations into vDPA ops calls. */

struct virtio_vdpa_adapter {
    struct vdpa_device *vdev;
    int                 registered;
    /* Emulated virtio PCI config space */
    uint32_t            host_features;
    uint32_t            guest_features;
    uint8_t             status;
    uint16_t            queue_sel;
    uint16_t            queue_size;
    uint32_t            queue_pfn;
    /* For each vq: recent state cache */
    uint16_t            vq_last_avail[8];
};

static struct virtio_vdpa_adapter virtio_vdpa_adapters[VDPA_MAX_DEVICES];
static int virtio_vdpa_num = 0;

/* Initialize the virtio-vdpa bus */
int virtio_vdpa_init(void)
{
    memset(virtio_vdpa_adapters, 0, sizeof(virtio_vdpa_adapters));
    virtio_vdpa_num = 0;
    kprintf("[vdpa] virtio-vdpa bus adapter initialized\n");
    return 0;
}

/* Add a vDPA device as a virtio device via the bus adapter */
int virtio_vdpa_add_device(struct vdpa_device *vdev)
{
    if (!vdev) return -1;
    if (virtio_vdpa_num >= VDPA_MAX_DEVICES) return -1;

    struct virtio_vdpa_adapter *adap = &virtio_vdpa_adapters[virtio_vdpa_num];
    memset(adap, 0, sizeof(*adap));
    adap->vdev = vdev;
    adap->registered = 1;

    /* Get host features from vDPA device */
    if (vdev->ops->get_features) {
        uint64_t features64 = vdev->ops->get_features(vdev);
        adap->host_features = (uint32_t)features64;
    }

    /* Determine queue size */
    if (vdev->ops->get_vq_num) {
        int n = vdev->ops->get_vq_num(vdev);
        adap->queue_size = (uint16_t)(n > 0 ? 256 : 0);
    }

    virtio_vdpa_num++;

    kprintf("[vdpa] virtio-vdpa: device '%s' added as virtio device\n",
            vdev->name ? vdev->name : "unnamed");
    return 0;
}

/* ── vDPA framework init ──────────────────────────────────────────── */

int vdpa_init(void)
{
    memset(vdpa_devices, 0, sizeof(vdpa_devices));
    vdpa_num_devices = 0;
    vdpa_initialized = 1;

    kprintf("[vdpa] vDPA framework initialized\n");

    /* Create a default software vDPA block device */
    uint64_t num_pages = (32ULL * 1024 * 1024) / PAGE_SIZE;
    uint64_t blk_mem = (uint64_t)pmm_alloc_frames((size_t)num_pages);
    uint64_t blk_sectors = (32ULL * 1024 * 1024) / 512;

    struct vdpa_soft_device *sdev;
    if (blk_mem) {
        sdev = vdpa_soft_device_create("vdpa-blk0",
                                        (uint8_t *)PHYS_TO_VIRT(blk_mem),
                                        blk_sectors);
        if (sdev) {
            vdpa_device_register(&sdev->vdev);
            virtio_vdpa_add_device(&sdev->vdev);
        }
    }

    return 0;
}

/* Forward declarations for stub functions */
struct vdpa_vq_state;

/* ── Stub: vdpa_set_status ─────────────────────────────── */
int vdpa_set_status(struct vdpa_device *dev, uint8_t status)
{
    (void)dev;
    (void)status;
    kprintf("[vdpa] vdpa_set_status: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: vdpa_set_features ─────────────────────────────── */
int vdpa_set_features(struct vdpa_device *dev, uint64_t features)
{
    (void)dev;
    (void)features;
    kprintf("[vdpa] vdpa_set_features: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: vdpa_set_config ─────────────────────────────── */
int vdpa_set_config(struct vdpa_device *dev, uint32_t offset, const void *buf, uint32_t len)
{
    (void)dev;
    (void)offset;
    (void)buf;
    (void)len;
    kprintf("[vdpa] vdpa_set_config: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: vdpa_get_config ─────────────────────────────── */
int vdpa_get_config(struct vdpa_device *dev, uint32_t offset, void *buf, uint32_t len)
{
    (void)dev;
    (void)offset;
    (void)buf;
    (void)len;
    kprintf("[vdpa] vdpa_get_config: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: vdpa_get_vq_state ─────────────────────────────── */
int vdpa_get_vq_state(struct vdpa_device *dev, uint16_t vq_idx, struct vdpa_vq_state *state)
{
    (void)dev;
    (void)vq_idx;
    (void)state;
    kprintf("[vdpa] vdpa_get_vq_state: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: vdpa_set_vq_state ─────────────────────────────── */
int vdpa_set_vq_state(struct vdpa_device *dev, uint16_t vq_idx, const struct vdpa_vq_state *state)
{
    (void)dev;
    (void)vq_idx;
    (void)state;
    kprintf("[vdpa] vdpa_set_vq_state: not yet implemented\n");
    return -ENOSYS;
}

#include "module.h"
module_init(vdpa_init);
