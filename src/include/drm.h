#ifndef DRM_H
#define DRM_H

#include "types.h"

/* ═══════════════════════════════════════════════════════════════════
 *  DRM public types and ioctl definitions
 * ═══════════════════════════════════════════════════════════════════ */

/* ── DRM ioctl numbers (simplified subset) ────────────────────── */

#define DRM_IOCTL_BASE      0x64

/* Struct-based ioctl encoding */
#define DRM_IOC_NONE        0U
#define DRM_IOC_READ        2U
#define DRM_IOC_WRITE       1U
#define DRM_IOC(dir, type, nr, size) \
    (((dir) << 30) | ((type) << 8) | ((nr) << 0) | ((size) << 16))
#define DRM_IO(nr)          DRM_IOC(DRM_IOC_NONE,  DRM_IOCTL_BASE, nr, 0)
#define DRM_IOR(nr, size)   DRM_IOC(DRM_IOC_READ,  DRM_IOCTL_BASE, nr, sizeof(size))
#define DRM_IOW(nr, size)   DRM_IOC(DRM_IOC_WRITE, DRM_IOCTL_BASE, nr, sizeof(size))
#define DRM_IOWR(nr, size)  DRM_IOC(DRM_IOC_READ|DRM_IOC_WRITE, DRM_IOCTL_BASE, nr, sizeof(size))

/* ── DRM ioctl commands ───────────────────────────────────────── */

#define DRM_IOCTL_VERSION       DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_GET_CAP       DRM_IOWR(0x0C, struct drm_get_cap)
#define DRM_IOCTL_MODE_GETRESOURCES DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCONNECTOR DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB    DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB     DRM_IOWR(0xAF, uint32_t)
#define DRM_IOCTL_MODE_PAGE_FLIP DRM_IOWR(0xB0, struct drm_mode_crtc_page_flip)
#define DRM_IOCTL_MODE_DIRTYFB  DRM_IOWR(0xB1, struct drm_mode_fb_dirty_cmd)
#define DRM_IOCTL_MODE_CREATE_DUMB DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB    DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)
#define DRM_IOCTL_MODE_GETPROPERTY  DRM_IOWR(0xAA, struct drm_mode_get_property)
#define DRM_IOCTL_MODE_SETPROPERTY  DRM_IOWR(0xAB, struct drm_mode_set_property)
#define DRM_IOCTL_MODE_GETPROPBLOB  DRM_IOWR(0xAC, struct drm_mode_get_blob)
#define DRM_IOCTL_MODE_ATOMIC       DRM_IOWR(0xBC, struct drm_mode_atomic)

#define DRM_CAP_DUMB_BUFFER    0x01
#define DRM_CAP_VBLANK_HIGH_CRTC 0x02
#define DRM_CAP_PRIME          0x05

/* ── DRM version ──────────────────────────────────────────────── */

struct drm_version {
    int     version_major;
    int     version_minor;
    int     version_patchlevel;
    uint32_t name_len;
    char    *name;
    uint32_t date_len;
    char    *date;
    uint32_t desc_len;
    char    *desc;
};

/* ── DRM capability query ─────────────────────────────────────── */

struct drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

/* ── Mode setting structures ──────────────────────────────────── */

struct drm_mode_card_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width;
    uint32_t max_width;
    uint32_t min_height;
    uint32_t max_height;
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mm_width;
    uint32_t mm_height;
    uint32_t subpixel;
    uint32_t pad;
};

struct drm_mode_fb_cmd {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
};

/* ── Dumb buffer structures ───────────────────────────────────── */

struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct drm_mode_destroy_dumb {
    uint32_t handle;
};

/* ── DRM device flags ─────────────────────────────────────────── */

#define DRIVER_HAVE_DUMB      (1U << 0)
#define DRIVER_MODESET        (1U << 1)
#define DRIVER_GEM            (1U << 2)

/* ═══════════════════════════════════════════════════════════════════
 *  DRM driver interface
 * ═══════════════════════════════════════════════════════════════════ */

struct drm_device;
struct drm_file;

struct drm_driver {
    const char *name;
    const char *desc;
    const char *date;
    int         major;
    int         minor;
    int         patchlevel;
    uint32_t    driver_features;

    int (*load)(struct drm_device *dev, unsigned long flags);
    void (*unload)(struct drm_device *dev);
    int (*open)(struct drm_device *dev, struct drm_file *file_priv);
    void (*postclose)(struct drm_device *dev, struct drm_file *file_priv);
    int (*ioctl_dispatch)(struct drm_device *dev, struct drm_file *file_priv,
                          uint32_t cmd, void *arg);
};

/* ── DRM device ───────────────────────────────────────────────── */

#define DRM_MAX_MINORS    16
#define DRM_MAX_FB       16
#define DRM_MAX_CRTC     4
#define DRM_MAX_CONNECTOR 4

struct drm_framebuffer {
    int      in_use;
    uint32_t fb_id;
    uint32_t handle;   /* GEM handle */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
};

struct drm_crtc {
    int      in_use;
    uint32_t crtc_id;
    int      enabled;
    uint32_t fb_id;     /* current framebuffer */
    uint32_t x, y;
    uint32_t mode_valid;
};

struct drm_connector {
    int      in_use;
    uint32_t connector_id;
    uint32_t connector_type;
    int      connected;
    int      mm_width;
    int      mm_height;
};

struct drm_device {
    const char           *name;
    struct drm_driver    *driver;
    int                   minor;
    int                   open_count;

    /* Framebuffers */
    struct drm_framebuffer framebuffers[DRM_MAX_FB];
    int                    num_framebuffers;
    uint32_t               next_fb_id;

    /* CRTCs */
    struct drm_crtc       crtcs[DRM_MAX_CRTC];
    int                   num_crtcs;
    uint32_t              next_crtc_id;

    /* Connectors */
    struct drm_connector  connectors[DRM_MAX_CONNECTOR];
    int                   num_connectors;
    uint32_t              next_connector_id;

    /* Mode info */
    uint32_t              min_width;
    uint32_t              max_width;
    uint32_t              min_height;
    uint32_t              max_height;

    void                 *priv;  /* driver-private data */
};

/* ═══════════════════════════════════════════════════════════════════
 *  DRM core API
 * ═══════════════════════════════════════════════════════════════════ */

int  drm_init(void);
void drm_exit(void);
int  drm_register_device(struct drm_device *dev);
int  drm_unregister_device(struct drm_device *dev);

/* Framebuffer management */
int  drm_add_fb(struct drm_device *dev, uint32_t handle,
                uint32_t width, uint32_t height,
                uint32_t pitch, uint32_t bpp, uint32_t depth);
int  drm_remove_fb(struct drm_device *dev, uint32_t fb_id);
struct drm_framebuffer *drm_fb_lookup(struct drm_device *dev, uint32_t fb_id);

/* CRTC management */
int  drm_add_crtc(struct drm_device *dev);
int  drm_add_connector(struct drm_device *dev, uint32_t type);

/* ═══════════════════════════════════════════════════════════════════
 *  GEM API
 * ═══════════════════════════════════════════════════════════════════ */

struct drm_gem_object {
    uint32_t handle;
    size_t   size;
    void    *vaddr;       /* kernel virtual address */
    uint64_t phys_addr;   /* physical address (for contiguous) */
    int      is_contig;   /* 1 = physically contiguous */
    int      refcount;
};

int  drm_gem_init(void);
int  drm_gem_create_object(size_t size, int is_contig,
                           struct drm_gem_object **out_obj);
void drm_gem_free_object(struct drm_gem_object *obj);
void drm_gem_ref(struct drm_gem_object *obj);
void drm_gem_unref(struct drm_gem_object *obj);
int  drm_gem_handle_create(struct drm_device *dev,
                           struct drm_gem_object *obj,
                           uint32_t *handle);
struct drm_gem_object *drm_gem_handle_lookup(struct drm_device *dev,
                                              uint32_t handle);
int  drm_gem_handle_close(struct drm_device *dev, uint32_t handle);
int  drm_gem_mmap(struct drm_gem_object *obj, uint64_t *offset);

/* Dumb buffer helpers */
int  drm_dumb_create(struct drm_device *dev,
                     struct drm_mode_create_dumb *args);
int  drm_dumb_map_offset(struct drm_device *dev,
                         struct drm_mode_map_dumb *args);
int  drm_dumb_destroy(struct drm_device *dev,
                      struct drm_mode_destroy_dumb *args);

#endif /* DRM_H */
