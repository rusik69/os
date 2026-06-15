/*
 * src/drivers/vfio.c — VFIO framework
 *
 * Userspace driver interface via /dev/vfio/vfio character device.
 * Provides container/group management for device assignment.
 * Simple: no IOMMU group isolation for now.
 */

#include "vfio.h"
#include "printf.h"
#include "string.h"
#include "devfs.h"
#include "pci.h"

/* ── Internal state ────────────────────────────────────────────────── */

#define VFIO_MAX_CONTAINERS  4
#define VFIO_MAX_GROUPS      8
#define VFIO_MAX_DEVICES     16

struct vfio_container {
    int   used;
    int   id;
    int   iommu_type;   /* VFIO_TYPE1_IOMMU, VFIO_NOIOMMU_IOMMU, etc. */
    int   group_ids[VFIO_MAX_GROUPS];
    int   num_groups;
};

struct vfio_group {
    int   used;
    int   id;
    int   container_id;  /* -1 = not attached */
    int   viable;
    int   num_devices;
    char  device_names[VFIO_MAX_DEVICES][64];
};

struct vfio_device {
    int   used;
    int   group_id;
    char  name[64];
    /* Device info (simplified) */
    struct pci_device pci_dev;
};

static struct vfio_container vfio_containers[VFIO_MAX_CONTAINERS];
static struct vfio_group     vfio_groups[VFIO_MAX_GROUPS];
static struct vfio_device    vfio_devices[VFIO_MAX_DEVICES];

static int vfio_initialized = 0;
static int vfio_container_count = 0;
static int vfio_group_count = 0;
static int vfio_device_count = 0;

/* ── VFIO character device I/O handlers ────────────────────────────── */

/* /dev/vfio/vfio read handler */
static int vfio_dev_read(void *priv, void *buf, uint32_t max_size, uint32_t *out_size)
{
    (void)priv;
    /* Return version info */
    static const char version[] = "VFIO API v0";
    uint32_t len = (max_size < sizeof(version)) ? max_size : (uint32_t)sizeof(version);
    memcpy(buf, version, len);
    *out_size = len;
    return 0;
}

/* /dev/vfio/vfio write handler */
static int vfio_dev_write(void *priv, const void *data, uint32_t size)
{
    (void)priv; (void)data;
    return (int)size; /* silently accept */
}

/* /dev/vfio/$N group device read */
static int vfio_group_read(void *priv, void *buf, uint32_t max_size, uint32_t *out_size)
{
    int group_id = (int)(uintptr_t)priv;
    if (group_id < 0 || group_id >= VFIO_MAX_GROUPS || !vfio_groups[group_id].used) {
        *out_size = 0;
        return -1;
    }
    static const char ginfo[] = "VFIO group";
    uint32_t len = (max_size < sizeof(ginfo)) ? max_size : (uint32_t)sizeof(ginfo);
    memcpy(buf, ginfo, len);
    *out_size = len;
    return 0;
}

static int vfio_group_write(void *priv, const void *data, uint32_t size)
{
    (void)priv; (void)data;
    return (int)size;
}

/* ── VFIO ioctl handler (called from enhanced sys_ioctl) ───────────── */

int vfio_ioctl(int container_id, int cmd, uint64_t arg)
{
    if (!vfio_initialized) return -1;

    switch (cmd) {
    case VFIO_GET_API_VERSION:
        return VFIO_API_VERSION;

    case VFIO_CHECK_EXTENSION: {
        int extension = (int)arg;
        switch (extension) {
        case VFIO_TYPE1_IOMMU:   return 1;
        case VFIO_NOIOMMU_IOMMU: return 1;
        default:                 return 0;
        }
    }

    case VFIO_SET_IOMMU: {
        int iommu_type;
        memcpy(&iommu_type, (void*)(uintptr_t)arg, sizeof(iommu_type));

        if (container_id < 0 || container_id >= VFIO_MAX_CONTAINERS)
            return -1;
        if (!vfio_containers[container_id].used)
            return -1;

        vfio_containers[container_id].iommu_type = iommu_type;
        kprintf("[vfio] container %d: IOMMU type set to %d\n",
                container_id, iommu_type);
        return 0;
    }

    case VFIO_GROUP_GET_STATUS: {
        /* Return group status */
        struct vfio_group_status status;
        status.argsz = sizeof(status);
        status.flags = VFIO_GROUP_FLAGS_VIABLE;
        /* If container is set, also set CONTAINER_SET */
        if (container_id >= 0 && container_id < VFIO_MAX_CONTAINERS &&
            vfio_containers[container_id].used) {
            /* Check if any group is attached */
            for (int i = 0; i < VFIO_MAX_GROUPS; i++) {
                if (vfio_groups[i].used &&
                    vfio_groups[i].container_id == container_id) {
                    status.flags |= VFIO_GROUP_FLAGS_CONTAINER_SET;
                    break;
                }
            }
        }
        memcpy((void*)(uintptr_t)arg, &status, sizeof(status));
        return 0;
    }

    case VFIO_GROUP_SET_CONTAINER: {
        int cont_fd;
        memcpy(&cont_fd, (void*)(uintptr_t)arg, sizeof(cont_fd));

        /* In our simple model, cont_fd is the container index */
        if (cont_fd < 0 || cont_fd >= VFIO_MAX_CONTAINERS)
            return -1;
        if (!vfio_containers[cont_fd].used)
            return -1;

        /* Find the group for this device */
        /* (container_id here is actually the group fd) */
        int group_id = container_id;
        if (group_id < 0 || group_id >= VFIO_MAX_GROUPS ||
            !vfio_groups[group_id].used)
            return -1;

        vfio_groups[group_id].container_id = cont_fd;
        kprintf("[vfio] group %d attached to container %d\n",
                group_id, cont_fd);
        return 0;
    }

    case VFIO_GROUP_UNSET_CONTAINER: {
        int group_id = container_id;
        if (group_id >= 0 && group_id < VFIO_MAX_GROUPS &&
            vfio_groups[group_id].used) {
            vfio_groups[group_id].container_id = -1;
        }
        return 0;
    }

    case VFIO_GROUP_GET_DEVICE_FD: {
        char dev_name[256];
        memcpy(dev_name, (void*)(uintptr_t)arg,
               (sizeof(dev_name) < 256) ? sizeof(dev_name) : 256);
        dev_name[255] = '\0';

        /* Find or create a device entry */
        if (vfio_device_count >= VFIO_MAX_DEVICES) return -1;

        struct vfio_device *dev = &vfio_devices[vfio_device_count];
        memset(dev, 0, sizeof(*dev));
        dev->used = 1;
        dev->group_id = container_id;
        memcpy(dev->name, dev_name, strlen(dev_name) + 1);

        /* Try to find PCI device */
        struct pci_device pci_dev;
        if (pci_find_device(0xFFFF, 0xFFFF, &pci_dev) == 0) {
            /* Just use whatever was found for demo purposes */
            memcpy(&dev->pci_dev, &pci_dev, sizeof(pci_dev));
        }

        int dev_fd = vfio_device_count;
        vfio_device_count++;

        /* Add to group */
        if (container_id >= 0 && container_id < VFIO_MAX_GROUPS &&
            vfio_groups[container_id].used &&
            vfio_groups[container_id].num_devices < VFIO_MAX_DEVICES) {
            int nd = vfio_groups[container_id].num_devices;
            memcpy(vfio_groups[container_id].device_names[nd],
                   dev_name, strlen(dev_name) + 1);
            vfio_groups[container_id].num_devices++;
        }

        kprintf("[vfio] device '%s' opened on group %d, fd=%d\n",
                dev_name, container_id, dev_fd);
        return dev_fd; /* return device fd */
    }

    default:
        kprintf("[vfio] Unknown ioctl cmd=0x%x on container\n",
                (unsigned int)cmd);
        return -1;
    }
}

/* ── Public API ────────────────────────────────────────────────────── */

int vfio_container_open(int container_id)
{
    if (container_id < 0 || container_id >= VFIO_MAX_CONTAINERS)
        return -1;
    if (vfio_containers[container_id].used)
        return -1;

    struct vfio_container *c = &vfio_containers[container_id];
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->id = container_id;
    c->iommu_type = VFIO_NOIOMMU_IOMMU;
    c->num_groups = 0;

    if (container_id >= vfio_container_count)
        vfio_container_count = container_id + 1;

    kprintf("[vfio] container %d opened\n", container_id);
    return 0;
}

int vfio_group_get_device_fd(int group_id, const char *device_name)
{
    if (group_id < 0 || group_id >= VFIO_MAX_GROUPS)
        return -1;
    if (!vfio_groups[group_id].used)
        return -1;

    if (vfio_device_count >= VFIO_MAX_DEVICES) return -1;

    struct vfio_device *dev = &vfio_devices[vfio_device_count];
    memset(dev, 0, sizeof(*dev));
    dev->used = 1;
    dev->group_id = group_id;
    memcpy(dev->name, device_name, strlen(device_name) + 1);

    int dev_fd = vfio_device_count++;
    return dev_fd;
}

/* ── Cleanup ───────────────────────────────────────────────────────── */

void vfio_cleanup(void)
{
    memset(vfio_containers, 0, sizeof(vfio_containers));
    memset(vfio_groups, 0, sizeof(vfio_groups));
    memset(vfio_devices, 0, sizeof(vfio_devices));
    vfio_container_count = 0;
    vfio_group_count = 0;
    vfio_device_count = 0;
    vfio_initialized = 0;
    kprintf("[vfio] cleaned up\n");
}

/* ── Init ──────────────────────────────────────────────────────────── */

int vfio_init(void)
{
    memset(vfio_containers, 0, sizeof(vfio_containers));
    memset(vfio_groups, 0, sizeof(vfio_groups));
    memset(vfio_devices, 0, sizeof(vfio_devices));

    /* Register /dev/vfio/vfio character device */
    if (devfs_register_device("vfio/vfio", NULL,
                              vfio_dev_read, vfio_dev_write) < 0) {
        kprintf("[vfio] Failed to register /dev/vfio/vfio\n");
        return -1;
    }

    /* Create a default group (/dev/vfio/0) */
    vfio_groups[0].used = 1;
    vfio_groups[0].id = 0;
    vfio_groups[0].container_id = -1;
    vfio_groups[0].viable = 1;
    vfio_groups[0].num_devices = 0;
    vfio_group_count = 1;

    {
        char name[48];
        snprintf(name, sizeof(name), "vfio/%d", 0);
        devfs_register_device(name, (void*)(uintptr_t)0,
                              vfio_group_read, vfio_group_write);
    }

    /* Create default container */
    vfio_containers[0].used = 1;
    vfio_containers[0].id = 0;
    vfio_containers[0].iommu_type = VFIO_NOIOMMU_IOMMU;
    vfio_container_count = 1;

    vfio_initialized = 1;
    kprintf("[vfio] VFIO framework initialized: %d container(s), %d group(s)\n",
            vfio_container_count, vfio_group_count);
    return 0;
}
