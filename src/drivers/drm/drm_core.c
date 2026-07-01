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
#include "drm_irq.h"
#include "drm_fence.h"

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
            gc->value = DRM_PRIME_CAP_EXPORT | DRM_PRIME_CAP_IMPORT;
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
            conn->count_encoders = 0;
            conn->count_props = 0;
            conn->encoder_id = 0;

            /* Copy display modes to userspace */
            int num_modes = dev->connectors[i].num_modes;
            conn->count_modes = (uint32_t)num_modes;

            if (conn->modes_ptr && num_modes > 0) {
                /* Write a drm_mode_modeinfo for each mode */
                struct drm_mode_modeinfo *km =
                    (struct drm_mode_modeinfo *)(uintptr_t)conn->modes_ptr;
                for (int j = 0; j < num_modes && j < DRM_MAX_DISPLAY_MODES; j++) {
                    struct drm_display_mode *src = &dev->connectors[i].modes[j];
                    if (!src->in_use)
                        continue;
                    km[j].clock    = src->clock;
                    km[j].hdisplay = src->hdisplay;
                    km[j].hsync_start = src->hsync_start;
                    km[j].hsync_end   = src->hsync_end;
                    km[j].htotal      = src->htotal;
                    km[j].hskew       = 0;
                    km[j].vdisplay    = src->vdisplay;
                    km[j].vsync_start = src->vsync_start;
                    km[j].vsync_end   = src->vsync_end;
                    km[j].vtotal      = src->vtotal;
                    km[j].vscan       = 0;
                    km[j].vrefresh    = src->vrefresh;
                    km[j].flags       = src->flags;
                    km[j].type        = src->type;
                    memcpy(km[j].name, src->name, 32);
                }
            }

            return 0;
        }
    }
    return -ENOENT;
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
        case DRM_IOCTL_MODE_GETCRTC:
            return drm_ioctl_mode_getcrtc(dev, fp, arg);
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
        case DRM_IOCTL_MODE_PAGE_FLIP:
            return drm_ioctl_mode_page_flip(dev, fp, arg);
        case DRM_IOCTL_MODE_DIRTYFB:
            return drm_dumb_dirtyfb(dev,
                       (struct drm_mode_fb_dirty_cmd *)arg);
        case DRM_IOCTL_GEM_CLOSE:
            return drm_gem_close_ioctl(dev, fp,
                       *(uint32_t *)arg);
        case DRM_IOCTL_GEM_FLINK:
            return drm_gem_flink_ioctl(dev, fp,
                       (struct drm_gem_flink *)arg);
        case DRM_IOCTL_GEM_OPEN:
            return drm_gem_open_ioctl(dev, fp,
                       (struct drm_gem_open *)arg);
        case DRM_IOCTL_PRIME_HANDLE_TO_FD:
            return drm_gem_prime_handle_to_fd(dev, fp,
                       (struct drm_prime_handle *)arg);
        case DRM_IOCTL_PRIME_FD_TO_HANDLE:
            return drm_gem_prime_fd_to_handle(dev, fp,
                       (struct drm_prime_handle *)arg);
        case DRM_IOCTL_MODE_SET_LAYOUT:
            return drm_ioctl_mode_setlayout(dev, fp, arg);
        case DRM_IOCTL_WAIT_VBLANK:
            return drm_wait_vblank_ioctl(dev, fp, arg);
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
    drm_damage_init();
    drm_atomic_init();
    drm_irq_init();
    drm_fence_init();
    drm_display_init();
    drm_prime_init();
    drm_multi_init();

    kprintf("[DRM] core initialised\n");
    return 0;
}

void drm_exit(void)
{
    drm_damage_exit();
    drm_atomic_exit();
    drm_irq_exit();
    drm_fence_exit();
    drm_display_exit();
    drm_prime_exit();
    drm_multi_exit();

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
            drm_damage_init_fb(&dev->framebuffers[i]);

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

/* ═══════════════════════════════════════════════════════════════════
 *  Multi-display support: CRTC ↔ connector routing
 * ═══════════════════════════════════════════════════════════════════ */

int drm_crtc_assign_connector(struct drm_device *dev,
                               uint32_t crtc_id,
                               uint32_t connector_id)
{
    if (!dev)
        return -EINVAL;

    spinlock_acquire(&g_drm_lock);

    /* Find the CRTC */
    struct drm_crtc *crtc = NULL;
    struct drm_connector *conn = NULL;
    int conn_idx = -1;

    for (int i = 0; i < DRM_MAX_CRTC; i++) {
        if (dev->crtcs[i].in_use && dev->crtcs[i].crtc_id == crtc_id) {
            crtc = &dev->crtcs[i];
            break;
        }
    }
    if (!crtc) {
        spinlock_release(&g_drm_lock);
        return -ENOENT;
    }

    /* Find the connector */
    for (int i = 0; i < DRM_MAX_CONNECTOR; i++) {
        if (dev->connectors[i].in_use &&
            dev->connectors[i].connector_id == connector_id) {
            conn = &dev->connectors[i];
            conn_idx = i;
            break;
        }
    }
    if (!conn) {
        spinlock_release(&g_drm_lock);
        return -ENOENT;
    }

    /* If the connector was previously assigned to a different CRTC,
     * clear that CRTC's connector_mask bit. */
    if (conn->crtc_id != 0 && conn->crtc_id != crtc_id) {
        for (int i = 0; i < DRM_MAX_CRTC; i++) {
            if (dev->crtcs[i].in_use &&
                dev->crtcs[i].crtc_id == conn->crtc_id) {
                dev->crtcs[i].connector_mask &= ~(1U << (unsigned)conn_idx);
                break;
            }
        }
    }

    /* Update the connector's crtc_id */
    conn->crtc_id = crtc_id;

    /* Set the bit in the CRTC's connector_mask */
    crtc->connector_mask |= (1U << (unsigned)conn_idx);

    spinlock_release(&g_drm_lock);

    kprintf("[DRM] connector %u → CRTC %u (mask now 0x%x)\n",
            connector_id, crtc_id, crtc->connector_mask);
    return 0;
}

int drm_connector_get_crtc(struct drm_device *dev,
                            uint32_t connector_id)
{
    if (!dev)
        return -EINVAL;

    for (int i = 0; i < DRM_MAX_CONNECTOR; i++) {
        if (dev->connectors[i].in_use &&
            dev->connectors[i].connector_id == connector_id) {
            return (int)dev->connectors[i].crtc_id;
        }
    }
    return -ENOENT;
}

int drm_crtc_connector_mask(struct drm_device *dev,
                             uint32_t crtc_id)
{
    if (!dev)
        return -EINVAL;

    for (int i = 0; i < DRM_MAX_CRTC; i++) {
        if (dev->crtcs[i].in_use &&
            dev->crtcs[i].crtc_id == crtc_id) {
            return (int)dev->crtcs[i].connector_mask;
        }
    }
    return -ENOENT;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Clone / extend mode helpers
 * ═══════════════════════════════════════════════════════════════════ */

int drm_display_clone_config(struct drm_device *dev,
                              uint32_t crtc_id,
                              const uint32_t *connector_ids,
                              int num_connectors)
{
    if (!dev || !connector_ids || num_connectors <= 0)
        return -EINVAL;

    /* Check the CRTC exists first */
    int crtc_found = 0;
    for (int i = 0; i < DRM_MAX_CRTC; i++) {
        if (dev->crtcs[i].in_use && dev->crtcs[i].crtc_id == crtc_id) {
            crtc_found = 1;
            break;
        }
    }
    if (!crtc_found)
        return -ENOENT;

    /* Assign all connectors to the same CRTC */
    for (int i = 0; i < num_connectors; i++) {
        int ret = drm_crtc_assign_connector(dev, crtc_id,
                                             connector_ids[i]);
        if (ret < 0) {
            kprintf("[DRM] clone: failed to assign connector %u "
                    "to CRTC %u (ret=%d)\n",
                    connector_ids[i], crtc_id, ret);
            return ret;
        }
    }

    kprintf("[DRM] clone mode: CRTC %u driving %d connectors\n",
            crtc_id, num_connectors);
    return 0;
}

int drm_display_extend_config(struct drm_device *dev,
                               const struct crtc_connector_pair *pairs,
                               int num_pairs)
{
    if (!dev || !pairs || num_pairs <= 0)
        return -EINVAL;

    for (int i = 0; i < num_pairs; i++) {
        int ret = drm_crtc_assign_connector(dev,
                                             pairs[i].crtc_id,
                                             pairs[i].connector_id);
        if (ret < 0) {
            kprintf("[DRM] extend: assignment failed for "
                    "CRTC %u → connector %u (ret=%d)\n",
                    pairs[i].crtc_id,
                    pairs[i].connector_id, ret);
            return ret;
        }
    }

    kprintf("[DRM] extend mode: %d CRTC–connector pairs configured\n",
            num_pairs);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  CRTC query ioctl (DRM_IOCTL_MODE_GETCRTC)
 * ═══════════════════════════════════════════════════════════════════ */

int drm_ioctl_mode_getcrtc(struct drm_device *dev,
                            struct drm_file *fp, void *arg)
{
    (void)fp;
    struct drm_mode_crtc *cr_arg = (struct drm_mode_crtc *)arg;

    if (!dev || !cr_arg)
        return -EINVAL;

    spinlock_acquire(&g_drm_lock);

    for (int i = 0; i < DRM_MAX_CRTC; i++) {
        if (dev->crtcs[i].in_use &&
            dev->crtcs[i].crtc_id == cr_arg->crtc_id) {
            struct drm_crtc *crtc = &dev->crtcs[i];

            cr_arg->fb_id = crtc->fb_id;
            cr_arg->mode_valid = crtc->mode_valid;
            cr_arg->gamma_size = 0;
            cr_arg->count_connectors = 0;

            /* Count connectors driven by this CRTC */
            uint32_t mask = crtc->connector_mask;
            for (int j = 0; j < DRM_MAX_CONNECTOR; j++) {
                if (mask & (1U << j))
                    cr_arg->count_connectors++;
            }

            /* Copy current mode if valid */
            if (crtc->mode_valid) {
                /* Find the first connector to extract the current mode */
                for (int j = 0; j < DRM_MAX_CONNECTOR; j++) {
                    if (mask & (1U << j) &&
                        dev->connectors[j].in_use &&
                        dev->connectors[j].num_modes > 0) {
                        /* Use the first mode as the reported current mode */
                        struct drm_display_mode *m =
                            &dev->connectors[j].modes[0];
                        if (m->in_use) {
                            cr_arg->mode.clock = m->clock;
                            cr_arg->mode.hdisplay = m->hdisplay;
                            cr_arg->mode.hsync_start = m->hsync_start;
                            cr_arg->mode.hsync_end = m->hsync_end;
                            cr_arg->mode.htotal = m->htotal;
                            cr_arg->mode.hskew = 0;
                            cr_arg->mode.vdisplay = m->vdisplay;
                            cr_arg->mode.vsync_start = m->vsync_start;
                            cr_arg->mode.vsync_end = m->vsync_end;
                            cr_arg->mode.vtotal = m->vtotal;
                            cr_arg->mode.vscan = 0;
                            cr_arg->mode.vrefresh = m->vrefresh;
                            cr_arg->mode.flags = m->flags;
                            cr_arg->mode.type = m->type;
                            memcpy(cr_arg->mode.name, m->name, 32);
                        }
                        break;
                    }
                }
            }

            /* Copy connector IDs if userspace provided a buffer */
            if (cr_arg->set_connectors_ptr && mask != 0) {
                uint32_t *conn_ids =
                    (uint32_t *)(uintptr_t)cr_arg->set_connectors_ptr;
                int out_idx = 0;
                for (int j = 0; j < DRM_MAX_CONNECTOR && out_idx < (int)cr_arg->count_connectors; j++) {
                    if (mask & (1U << j) && dev->connectors[j].in_use)
                        conn_ids[out_idx++] = dev->connectors[j].connector_id;
                }
            }

            spinlock_release(&g_drm_lock);
            return 0;
        }
    }

    spinlock_release(&g_drm_lock);
    return -ENOENT;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Page flip ioctl (DRM_IOCTL_MODE_PAGE_FLIP)
 * ═══════════════════════════════════════════════════════════════════
 *
 *  In clone mode a single page flip updates the CRTC's fb_id; all
 *  connectors driven by that CRTC display the new buffer on the next
 *  vblank.  In extend mode each CRTC has its own fb_id and is flipped
 *  independently.
 *
 *  For now, the flip is recorded immediately (synchronous flip).
 *  A full implementation would defer to vblank IRQ.
 * ═══════════════════════════════════════════════════════════════════ */

int drm_ioctl_mode_page_flip(struct drm_device *dev,
                              struct drm_file *fp, void *arg)
{
    (void)fp;
    struct drm_mode_crtc_page_flip *flip =
        (struct drm_mode_crtc_page_flip *)arg;

    if (!dev || !flip)
        return -EINVAL;

    spinlock_acquire(&g_drm_lock);

    /* Find the CRTC */
    for (int i = 0; i < DRM_MAX_CRTC; i++) {
        if (dev->crtcs[i].in_use &&
            dev->crtcs[i].crtc_id == flip->crtc_id) {
            struct drm_crtc *crtc = &dev->crtcs[i];

            /* Validate the new framebuffer exists */
            struct drm_framebuffer *new_fb =
                drm_fb_lookup(dev, flip->fb_id);
            if (!new_fb) {
                spinlock_release(&g_drm_lock);
                return -ENOENT;
            }

            /* Store current fb_id for reference management */
            uint32_t old_fb_id = crtc->fb_id;

            /* Take a reference on the new framebuffer (CRTC now holds it) */
            drm_fb_ref(new_fb);
            crtc->fb_id = flip->fb_id;

            /* Drop the reference on the old framebuffer (CRTC releasing it) */
            if (old_fb_id != 0 && old_fb_id != flip->fb_id) {
                struct drm_framebuffer *old_fb =
                    drm_fb_lookup(dev, old_fb_id);
                if (old_fb) {
                    drm_fb_unref(dev, old_fb);
                }
            }

            spinlock_release(&g_drm_lock);

            kprintf("[DRM] page flip: CRTC %u fb %u → %u "
                    "(clone mask 0x%x)\n",
                    flip->crtc_id, old_fb_id,
                    flip->fb_id, crtc->connector_mask);
            return 0;
        }
    }

    spinlock_release(&g_drm_lock);
    return -ENOENT;
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
