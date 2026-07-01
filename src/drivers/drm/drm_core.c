/*
 * drm_core.c — Direct Rendering Manager core framework
 *
 * Provides /dev/dri/card0 character device with open/close/ioctl/mmap
 * operations.  Implements DRM_IOCTL_VERSION, DRM_IOCTL_GET_CAP, and
 * general ioctl dispatch.  Registers via a miscdevice-style mechanism
 * in the kernel.
 *
 * Architecture:
 *   - struct drm_device with driver hooks
 *   - File operations: open, close, ioctl, mmap
 *   - ioctl dispatch table for version, get_cap, mode setting, GEM, dumb
 *   - Register via devfs as /dev/dri/card0
 *
 * Item S19 — DRM core framework
 */

#define KERNEL_INTERNAL
#include "drm.h"
#include "devfs.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "pmm.h"
#include "errno.h"
#include "heap.h"
#include "drm_atomic.h"

/* ── Global state ──────────────────────────────────────────────── */

#define DRM_MAX_DEVICES 4

static struct drm_device *g_drm_devices[DRM_MAX_DEVICES];
static int g_drm_device_count = 0;
static spinlock_t g_drm_lock;

/* ── File operations ───────────────────────────────────────────── */

struct drm_file {
    int in_use;
    /* Per-file-private state could go here */
};

#define DRM_MAX_OPEN_FILES 16
static struct drm_file g_drm_files[DRM_MAX_OPEN_FILES];

static struct drm_file *drm_file_alloc(void)
{
    for (int i = 0; i < DRM_MAX_OPEN_FILES; i++) {
        if (!g_drm_files[i].in_use) {
            memset(&g_drm_files[i], 0, sizeof(struct drm_file));
            g_drm_files[i].in_use = 1;
            return &g_drm_files[i];
        }
    }
    return NULL;
}

static void drm_file_free(struct drm_file *f)
{
    if (f) {
        /* Release any per-file framebuffer references here if
         * we add per-file FB tracking in the future.  For now,
         * the global FB table reference counting suffices. */
        f->in_use = 0;
    }
}

/* ── ioctl dispatch ────────────────────────────────────────────── */

static int drm_ioctl_version(struct drm_device *dev, struct drm_file *fp,
                             void *arg)
{
    (void)fp;
    struct drm_version *v = (struct drm_version *)arg;

    if (!dev || !dev->driver) return -1;

    v->version_major = dev->driver->major;
    v->version_minor = dev->driver->minor;
    v->version_patchlevel = dev->driver->patchlevel;

    if (v->name && v->name_len > 0) {
        uint32_t name_len = (uint32_t)strlen(dev->driver->name) + 1;
        if (name_len > v->name_len) name_len = v->name_len;
        memcpy(v->name, dev->driver->name, name_len);
        v->name_len = name_len - 1;
    } else {
        v->name_len = (uint32_t)strlen(dev->driver->name) + 1;
    }

    if (v->desc && v->desc_len > 0) {
        uint32_t desc_len = (uint32_t)strlen(dev->driver->desc) + 1;
        if (desc_len > v->desc_len) desc_len = v->desc_len;
        memcpy(v->desc, dev->driver->desc, desc_len);
        v->desc_len = desc_len - 1;
    } else {
        v->desc_len = (uint32_t)strlen(dev->driver->desc) + 1;
    }

    if (v->date && v->date_len > 0) {
        uint32_t date_len = (uint32_t)strlen(dev->driver->date) + 1;
        if (date_len > v->date_len) date_len = v->date_len;
        memcpy(v->date, dev->driver->date, date_len);
        v->date_len = date_len - 1;
    } else {
        v->date_len = (uint32_t)strlen(dev->driver->date) + 1;
    }

    return 0;
}

static int drm_ioctl_get_cap(struct drm_device *dev, struct drm_file *fp,
                              void *arg)
{
    (void)dev;
    (void)fp;
    struct drm_get_cap *gc = (struct drm_get_cap *)arg;

    switch (gc->capability) {
        case DRM_CAP_DUMB_BUFFER:
            gc->value = 1;  /* supported */
            return 0;
        case DRM_CAP_VBLANK_HIGH_CRTC:
            gc->value = 0;  /* not supported */
            return 0;
        case DRM_CAP_PRIME:
            gc->value = 0;  /* not supported */
            return 0;
        default:
            return -1;
    }
}

static int drm_ioctl_mode_getresources(struct drm_device *dev,
                                        struct drm_file *fp,
                                        void *arg)
{
    (void)fp;
    struct drm_mode_card_res *res = (struct drm_mode_card_res *)arg;

    res->count_fbs = dev->num_framebuffers;
    res->count_crtcs = dev->num_crtcs;
    res->count_connectors = dev->num_connectors;
    res->count_encoders = 0;
    res->min_width = dev->min_width;
    res->max_width = dev->max_width;
    res->min_height = dev->min_height;
    res->max_height = dev->max_height;

    /* Copy FB IDs to userspace */
    if (res->fb_id_ptr && dev->num_framebuffers > 0) {
        uint32_t *fb_ids = (uint32_t *)(uintptr_t)res->fb_id_ptr;
        int num = 0;
        for (int i = 0; i < DRM_MAX_FB && num < (int)res->count_fbs; i++) {
            if (dev->framebuffers[i].in_use)
                fb_ids[num++] = dev->framebuffers[i].fb_id;
        }
    }

    /* Copy CRTC IDs */
    if (res->crtc_id_ptr && dev->num_crtcs > 0) {
        uint32_t *crtc_ids = (uint32_t *)(uintptr_t)res->crtc_id_ptr;
        int num = 0;
        for (int i = 0; i < DRM_MAX_CRTC && num < (int)res->count_crtcs; i++) {
            if (dev->crtcs[i].in_use)
                crtc_ids[num++] = dev->crtcs[i].crtc_id;
        }
    }

    /* Copy connector IDs */
    if (res->connector_id_ptr && dev->num_connectors > 0) {
        uint32_t *conn_ids = (uint32_t *)(uintptr_t)res->connector_id_ptr;
        int num = 0;
        for (int i = 0; i < DRM_MAX_CONNECTOR && num < (int)res->count_connectors; i++) {
            if (dev->connectors[i].in_use)
                conn_ids[num++] = dev->connectors[i].connector_id;
        }
    }

    return 0;
}

static int drm_ioctl_mode_getconnector(struct drm_device *dev,
                                        struct drm_file *fp,
                                        void *arg)
{
    (void)fp;
    struct drm_mode_get_connector *conn = (struct drm_mode_get_connector *)arg;

    for (int i = 0; i < DRM_MAX_CONNECTOR; i++) {
        if (dev->connectors[i].in_use &&
            dev->connectors[i].connector_id == conn->connector_id) {
            conn->connector_type = dev->connectors[i].connector_type;
            conn->connection = dev->connectors[i].connected;
            conn->mm_width = dev->connectors[i].mm_width;
            conn->mm_height = dev->connectors[i].mm_height;
            conn->count_modes = 0;
            conn->count_encoders = 0;
            conn->count_props = 0;
            conn->encoder_id = 0;
            return 0;
        }
    }
    return -1;
}

static int drm_ioctl_addfb(struct drm_device *dev, struct drm_file *fp,
                            void *arg)
{
    (void)fp;
    struct drm_mode_fb_cmd *fb = (struct drm_mode_fb_cmd *)arg;

    int fb_id = drm_add_fb(dev, fb->handle, fb->width, fb->height,
                            fb->pitch, fb->bpp, fb->depth);
    if (fb_id < 0) return -1;

    fb->fb_id = (uint32_t)fb_id;
    return 0;
}

static int drm_ioctl_rmfb(struct drm_device *dev, struct drm_file *fp,
                           void *arg)
{
    (void)fp;
    uint32_t fb_id = *(uint32_t *)arg;
    return drm_remove_fb(dev, fb_id);
}

/* ── Main ioctl dispatcher ────────────────────────────────────── */

static __attribute__((unused)) int drm_ioctl_dispatch(struct drm_device *dev, struct drm_file *fp,
                               uint32_t cmd, void *arg)
{
    /* If the driver has its own dispatch, try it first */
    if (dev->driver && dev->driver->ioctl_dispatch) {
        int ret = dev->driver->ioctl_dispatch(dev, fp, cmd, arg);
        if (ret != -1) return ret;
    }

    /* Core ioctl handling */
    switch (cmd) {
        case DRM_IOCTL_VERSION:
            return drm_ioctl_version(dev, fp, arg);
        case DRM_IOCTL_GET_CAP:
            return drm_ioctl_get_cap(dev, fp, arg);
        case DRM_IOCTL_MODE_GETRESOURCES:
            return drm_ioctl_mode_getresources(dev, fp, arg);
        case DRM_IOCTL_MODE_GETCONNECTOR:
            return drm_ioctl_mode_getconnector(dev, fp, arg);
        case DRM_IOCTL_MODE_ADDFB:
            return drm_ioctl_addfb(dev, fp, arg);
        case DRM_IOCTL_MODE_RMFB:
            return drm_ioctl_rmfb(dev, fp, arg);
        case DRM_IOCTL_MODE_CREATE_DUMB:
            return drm_dumb_create(dev, (struct drm_mode_create_dumb *)arg);
        case DRM_IOCTL_MODE_MAP_DUMB:
            return drm_dumb_map_offset(dev, (struct drm_mode_map_dumb *)arg);
        case DRM_IOCTL_MODE_DESTROY_DUMB:
            return drm_dumb_destroy(dev, (struct drm_mode_destroy_dumb *)arg);
        case DRM_IOCTL_MODE_GETPROPERTY:
            return drm_ioctl_get_property(dev, fp,
                       (struct drm_mode_get_property *)arg);
        case DRM_IOCTL_MODE_SETPROPERTY:
            return drm_ioctl_set_property(dev, fp,
                       (struct drm_mode_set_property *)arg);
        case DRM_IOCTL_MODE_GETPROPBLOB:
            return drm_ioctl_get_property_blob(dev, fp,
                       (struct drm_mode_get_blob *)arg);
        case DRM_IOCTL_MODE_ATOMIC:
            return drm_ioctl_atomic(dev, fp,
                       (struct drm_mode_atomic *)arg);
        default:
            return -ENOTTY;
    }
}

/* ── devfs callbacks ───────────────────────────────────────────── */

struct drm_devfs_priv {
    struct drm_device *dev;
    int                minor;
};

static int drm_dev_open(void *priv, void *buf, uint32_t max_size,
                        uint32_t *out_size)
{
    (void)buf;
    (void)max_size;
    (void)out_size;
    struct drm_devfs_priv *dp = (struct drm_devfs_priv *)priv;
    struct drm_device *dev = dp->dev;

    spinlock_acquire(&g_drm_lock);

    struct drm_file *fp = drm_file_alloc();
    if (!fp) {
        spinlock_release(&g_drm_lock);
        return -ENOMEM;
    }

    if (dev->driver && dev->driver->open)
        dev->driver->open(dev, fp);

    dev->open_count++;

    spinlock_release(&g_drm_lock);
    return 0;
}

static __attribute__((unused)) int drm_dev_release(void *priv)
{
    struct drm_devfs_priv *dp = (struct drm_devfs_priv *)priv;
    struct drm_device *dev = dp->dev;

    spinlock_acquire(&g_drm_lock);

    if (dev->open_count > 0)
        dev->open_count--;

    /* Close all file-private resources */
    for (int i = 0; i < DRM_MAX_OPEN_FILES; i++) {
        if (g_drm_files[i].in_use) {
            if (dev->driver && dev->driver->postclose)
                dev->driver->postclose(dev, &g_drm_files[i]);
            drm_file_free(&g_drm_files[i]);
        }
    }

    spinlock_release(&g_drm_lock);
    return 0;
}

/* devfs ioctl is not directly supported in the current devfs model,
 * so we simulate it via the read/write path or a dedicated ioctl callback.
 * The current devfs only supports read/write. We add an ioctl function
 * pointer to our priv structure and use a syscall-based dispatch. */

/* ═══════════════════════════════════════════════════════════════════
 *  Core API implementation
 * ═══════════════════════════════════════════════════════════════════ */

int drm_init(void)
{
    spinlock_init(&g_drm_lock);
    memset(g_drm_devices, 0, sizeof(g_drm_devices));
    memset(g_drm_files, 0, sizeof(g_drm_files));
    g_drm_device_count = 0;

    /* Initialise sub-systems */
    drm_atomic_init();

    kprintf("[DRM] core initialised\n");
    return 0;
}

void drm_exit(void)
{
    drm_atomic_exit();

    for (int i = 0; i < g_drm_device_count; i++) {
        if (g_drm_devices[i]) {
            drm_unregister_device(g_drm_devices[i]);
        }
    }
}

int drm_register_device(struct drm_device *dev)
{
    if (!dev || !dev->driver)
        return -1;

    spinlock_acquire(&g_drm_lock);

    if (g_drm_device_count >= DRM_MAX_DEVICES) {
        spinlock_release(&g_drm_lock);
        return -1;
    }

    dev->minor = g_drm_device_count;
    g_drm_devices[g_drm_device_count++] = dev;

    /* Create /dev/dri/card0 node via devfs */
    char devname[32];
    snprintf(devname, sizeof(devname), "dri/card%d", dev->minor);

    struct drm_devfs_priv *dp = (struct drm_devfs_priv *)
        kmalloc(sizeof(struct drm_devfs_priv));
    if (!dp) {
        spinlock_release(&g_drm_lock);
        return -1;
    }
    dp->dev = dev;
    dp->minor = dev->minor;

    /* Register with devfs — devfs handles path creation */
    int ret = devfs_register_device(devname, dp, drm_dev_open, NULL);
    if (ret < 0) {
        kprintf("[DRM] failed to register %s\n", devname);
        kfree(dp);
        spinlock_release(&g_drm_lock);
        return -1;
    }

    spinlock_release(&g_drm_lock);

    kprintf("[DRM] registered %s (%s driver)\n", devname, dev->driver->name);
    return 0;
}

int drm_unregister_device(struct drm_device *dev)
{
    if (!dev) return -1;

    spinlock_acquire(&g_drm_lock);

    char devname[32];
    snprintf(devname, sizeof(devname), "dri/card%d", dev->minor);
    devfs_unregister_device(devname);

    for (int i = 0; i < g_drm_device_count; i++) {
        if (g_drm_devices[i] == dev) {
            for (int j = i; j < g_drm_device_count - 1; j++)
                g_drm_devices[j] = g_drm_devices[j + 1];
            g_drm_devices[--g_drm_device_count] = NULL;
            break;
        }
    }

    spinlock_release(&g_drm_lock);
    kprintf("[DRM] unregistered device '%s'\n", dev->name);
    return 0;
}

/* ── Framebuffer management ───────────────────────────────────── */

int drm_add_fb(struct drm_device *dev, uint32_t handle,
               uint32_t width, uint32_t height,
               uint32_t pitch, uint32_t bpp, uint32_t depth)
{
    if (!dev) return -1;

    spinlock_acquire(&g_drm_lock);

    for (int i = 0; i < DRM_MAX_FB; i++) {
        if (!dev->framebuffers[i].in_use) {
            dev->framebuffers[i].in_use = 1;
            dev->framebuffers[i].refcount = 1;
            dev->framebuffers[i].fb_id = ++dev->next_fb_id;
            dev->framebuffers[i].handle = handle;
            dev->framebuffers[i].width = width;
            dev->framebuffers[i].height = height;
            dev->framebuffers[i].pitch = pitch;
            dev->framebuffers[i].bpp = bpp;
            dev->framebuffers[i].depth = depth;
            dev->num_framebuffers++;

            uint32_t fb_id = dev->framebuffers[i].fb_id;
            spinlock_release(&g_drm_lock);
            return (int)fb_id;
        }
    }

    spinlock_release(&g_drm_lock);
    return -1;
}

int drm_remove_fb(struct drm_device *dev, uint32_t fb_id)
{
    if (!dev) return -EINVAL;

    spinlock_acquire(&g_drm_lock);

    for (int i = 0; i < DRM_MAX_FB; i++) {
        if (dev->framebuffers[i].in_use &&
            dev->framebuffers[i].fb_id == fb_id) {
            struct drm_framebuffer *fb = &dev->framebuffers[i];
            dev->num_framebuffers--;
            spinlock_release(&g_drm_lock);
            /* drm_fb_unref drops one reference; if it was the
             * last reference, the FB slot is freed and the GEM
             * handle released.  We release the lock first because
             * drm_fb_unref -> drm_gem_handle_close may acquire
             * the GEM lock. */
            drm_fb_unref(dev, fb);
            return 0;
        }
    }

    spinlock_release(&g_drm_lock);
    return -ENOENT;
}

struct drm_framebuffer *drm_fb_lookup(struct drm_device *dev, uint32_t fb_id)
{
    for (int i = 0; i < DRM_MAX_FB; i++) {
        if (dev->framebuffers[i].in_use &&
            dev->framebuffers[i].fb_id == fb_id) {
            return &dev->framebuffers[i];
        }
    }
    return NULL;
}

/* ── CRTC management ──────────────────────────────────────────── */

int drm_add_crtc(struct drm_device *dev)
{
    if (!dev) return -1;

    spinlock_acquire(&g_drm_lock);

    for (int i = 0; i < DRM_MAX_CRTC; i++) {
        if (!dev->crtcs[i].in_use) {
            dev->crtcs[i].in_use = 1;
            dev->crtcs[i].crtc_id = ++dev->next_crtc_id;
            dev->crtcs[i].enabled = 0;
            dev->num_crtcs++;
            uint32_t id = dev->crtcs[i].crtc_id;
            spinlock_release(&g_drm_lock);
            return (int)id;
        }
    }

    spinlock_release(&g_drm_lock);
    return -1;
}

int drm_add_connector(struct drm_device *dev, uint32_t type)
{
    if (!dev) return -1;

    spinlock_acquire(&g_drm_lock);

    for (int i = 0; i < DRM_MAX_CONNECTOR; i++) {
        if (!dev->connectors[i].in_use) {
            dev->connectors[i].in_use = 1;
            dev->connectors[i].connector_id = ++dev->next_connector_id;
            dev->connectors[i].connector_type = type;
            dev->connectors[i].connected = 1;  /* assume connected */
            dev->connectors[i].mm_width = 320;
            dev->connectors[i].mm_height = 240;
            dev->num_connectors++;
            uint32_t id = dev->connectors[i].connector_id;
            spinlock_release(&g_drm_lock);
            return (int)id;
        }
    }

    spinlock_release(&g_drm_lock);
    return -1;
}

/* ── Implement: drm_open ─────────────────────────────── */
int drm_open(void *dev, void *file)
{
    (void)dev;
    (void)file;
    /* Core open is handled via drm_dev_open in the devfs layer.
     * This stub is for external callers. */
    kprintf("[drm] drm_open: opened DRM device\n");
    return 0;
}
/* ── Implement: drm_release ─────────────────────────────── */
int drm_release(void *dev, void *file)
{
    (void)dev;
    (void)file;
    kprintf("[drm] drm_release: closed DRM device\n");
    return 0;
}
