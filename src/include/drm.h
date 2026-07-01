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
#define DRM_IOCTL_MODE_GETCRTC     DRM_IOWR(0xA1, struct drm_mode_crtc)
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

#define DRM_IOCTL_GEM_CLOSE     DRM_IOWR(0x09, uint32_t)
#define DRM_IOCTL_GEM_FLINK     DRM_IOWR(0x0A, struct drm_gem_flink)
#define DRM_IOCTL_GEM_OPEN      DRM_IOWR(0x0B, struct drm_gem_open)

/* DRM PRIME FD sharing */
#define DRM_IOCTL_PRIME_HANDLE_TO_FD DRM_IOWR(0x0D, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE DRM_IOWR(0x0E, struct drm_prime_handle)

#define DRM_CAP_DUMB_BUFFER      0x01
#define DRM_CAP_VBLANK_HIGH_CRTC 0x02
#define DRM_CAP_PRIME            0x05

/* PRIME capability flags (returned in DRM_CAP_PRIME value) */
#define DRM_PRIME_CAP_EXPORT    1
#define DRM_PRIME_CAP_IMPORT    2

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

/* ── Display mode structures ───────────────────────────────────── */

#define DRM_DISPLAY_MODE_FLAG_PHSYNC    (1U << 0)
#define DRM_DISPLAY_MODE_FLAG_NHSYNC    (1U << 1)
#define DRM_DISPLAY_MODE_FLAG_PVSYNC    (1U << 2)
#define DRM_DISPLAY_MODE_FLAG_NVSYNC    (1U << 3)
#define DRM_DISPLAY_MODE_FLAG_INTERLACE (1U << 4)
#define DRM_DISPLAY_MODE_FLAG_DBLSCAN   (1U << 5)
#define DRM_DISPLAY_MODE_FLAG_CSYNC     (1U << 6)

/* Type flags for display modes */
#define DRM_MODE_TYPE_BUILTIN   (1U << 0)
#define DRM_MODE_TYPE_PREFERRED (1U << 1)
#define DRM_MODE_TYPE_DEFAULT   (1U << 2)
#define DRM_MODE_TYPE_USERDEF   (1U << 3)
#define DRM_MODE_TYPE_DRIVER    (1U << 4)

/* Max display modes per connector */
#define DRM_MAX_DISPLAY_MODES 64

struct drm_display_mode {
    int      in_use;
    /* Horizontal timing (pixels) */
    uint32_t clock;            /* pixel clock in kHz */
    uint16_t hdisplay;         /* horizontal active pixels */
    uint16_t hsync_start;      /* hdisplay + hfront_porch */
    uint16_t hsync_end;        /* hdisplay + hfront_porch + hsync_width */
    uint16_t htotal;           /* total horizontal pixels */
    /* Vertical timing (lines) */
    uint16_t vdisplay;         /* vertical active lines */
    uint16_t vsync_start;      /* vdisplay + vfront_porch */
    uint16_t vsync_end;        /* vdisplay + vfront_porch + vsync_width */
    uint16_t vtotal;           /* total vertical lines */
    uint32_t vrefresh;         /* vertical refresh rate in mHz */
    uint32_t flags;            /* DRM_DISPLAY_MODE_FLAG_* */
    uint32_t type;             /* DRM_MODE_TYPE_* */
    char     name[32];         /* human-readable mode name */
};

/* ── CVT/EDID mode generation API ───────────────────────────── */

/* Forward declarations */
struct drm_connector;
struct drm_device;
struct drm_file;

/*
 * drm_display_init / drm_display_exit — Initialise display mode subsystem.
 */
int  drm_display_init(void);
void drm_display_exit(void);

/*
 * drm_display_add_mode — Add a display mode to a connector's mode list.
 * Returns 0 on success, negative errno on failure.
 */
int  drm_display_add_mode(struct drm_connector *conn,
                          const struct drm_display_mode *mode);

/*
 * drm_display_clear_modes — Clear all modes from a connector.
 */
void drm_display_clear_modes(struct drm_connector *conn);

/*
 * drm_display_cvt_mode — Generate a CVT-RB (reduced blanking) mode
 *                        for the given resolution and refresh rate.
 *
 * @width:   Horizontal active pixels (e.g. 1920)
 * @height:  Vertical active lines (e.g. 1080)
 * @refresh: Refresh rate in Hz (e.g. 60)
 * @reduced: 1 == reduced blanking (CVT-RB), 0 == standard CVT
 * @out:     Output mode structure filled with computed timings.
 *
 * Returns 0 on success, negative errno on failure.
 */
int  drm_display_cvt_mode(uint32_t width, uint32_t height,
                           uint32_t refresh, int reduced,
                           struct drm_display_mode *out);

/*
 * drm_display_edid_parse — Parse a 128-byte EDID block and add all
 *                          valid modes to the given connector.
 *
 * @conn:  Connector to add modes to.
 * @edid:  Pointer to 128-byte EDID block data.
 *
 * Returns number of modes added, or negative errno on failure.
 */
int  drm_display_edid_parse(struct drm_connector *conn,
                             const uint8_t *edid);

/*
 * drm_display_fill_modes — Populate a connector with standard modes
 *                          (used when no EDID is available).
 *
 * Generates a set of common resolutions from VGA to 4K.
 * Returns number of modes added.
 */
int  drm_display_fill_modes(struct drm_connector *conn,
                             int max_width, int max_height);

/* ── Mode setting structures (ioctl-facing) ──────────────────── */

/*
 * DRM userspace mode info — the structure returned via
 * DRM_IOCTL_MODE_GETCONNECTOR for each display mode.
 */
struct drm_mode_modeinfo {
    uint32_t clock;       /* pixel clock in kHz */
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t hskew;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint16_t vscan;
    uint32_t vrefresh;    /* vertical refresh rate in mHz */
    uint32_t flags;       /* DRM_MODE_FLAG_* (same as DRM_DISPLAY_MODE_FLAG_*) */
    uint32_t type;        /* DRM_MODE_TYPE_* (same as above) */
    char     name[32];
};

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

struct drm_mode_fb_dirty_cmd {
    uint32_t fb_id;
    uint32_t flags;
    uint32_t color;
    uint32_t num_clips;
    uint64_t clips_ptr;
    uint32_t pad;
};

/* ── CRTC page flip structure ──────────────────────────────────── */

struct drm_mode_crtc_page_flip {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    uint32_t reserved;
    uint64_t user_data;
};

/* ── CRTC query structure ──────────────────────────────────────── */

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct drm_mode_modeinfo mode;
};

/* ── DRM PRIME FD sharing structure ─────────────────────────── */

struct drm_prime_handle {
    uint32_t handle;    /* GEM handle (for export) / returned handle (for import) */
    uint32_t flags;     /* DRM_CLOEXEC etc. (reserved for future use) */
    int      prime_fd;  /* returned prime FD (for export) / input prime FD (for import) */
};

/* ── DRM device flags ─────────────────────────────────────────── */

#define DRIVER_HAVE_DUMB      (1U << 0)
#define DRIVER_MODESET        (1U << 1)
#define DRIVER_GEM            (1U << 2)
#define DRIVER_PRIME          (1U << 3)  /* PRIME FD sharing supported */

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

    /* Dumb buffer callbacks — drivers may override these to use
     * driver-specific backing memory (e.g. LFB for bochs).
     * When NULL, the default drm_dumb_{create,map_offset,destroy} is used. */
    int (*dumb_create)(struct drm_device *dev,
                       struct drm_mode_create_dumb *args);
    int (*dumb_map_offset)(struct drm_device *dev,
                           struct drm_mode_map_dumb *args);
    int (*dumb_destroy)(struct drm_device *dev,
                        struct drm_mode_destroy_dumb *args);
};

/* ── DRM device ───────────────────────────────────────────────── */

#define DRM_MAX_MINORS    16
#define DRM_MAX_FB       16
#define DRM_MAX_CRTC     4
#define DRM_MAX_CONNECTOR 4

struct drm_framebuffer {
    int      in_use;
    int      refcount;  /* reference count — released when 0 */
    uint32_t fb_id;
    uint32_t handle;   /* GEM handle */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    /* Damage tracking — accumulated dirty region for efficient updates */
    int      damage_x1;     /* top-left x of dirty region (inclusive) */
    int      damage_y1;     /* top-left y of dirty region (inclusive) */
    int      damage_x2;     /* bottom-right x of dirty region (exclusive) */
    int      damage_y2;     /* bottom-right y of dirty region (exclusive) */
    int      damage_valid;  /* 1 if damage region has been set */
};

struct drm_crtc {
    int      in_use;
    uint32_t crtc_id;
    int      enabled;
    uint32_t fb_id;     /* current framebuffer */
    uint32_t x, y;
    uint32_t mode_valid;
    uint32_t connector_mask;  /* bitmask: bit N = drives dev->connectors[N] */
};

struct drm_connector {
    int      in_use;
    uint32_t connector_id;
    uint32_t connector_type;
    int      connected;
    int      mm_width;
    int      mm_height;
    uint32_t crtc_id;          /* CRTC driving this connector (0 = unassigned) */
    /* Display modes */
    struct drm_display_mode modes[DRM_MAX_DISPLAY_MODES];
    int                    num_modes;
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

/* Framebuffer reference counting — protects against premature free
 * while CRTCs or planes still reference the framebuffer.
 * drm_fb_ref() increments the refcount; drm_fb_unref() decrements
 * and frees the FB when refcount reaches 0. */
void drm_fb_ref(struct drm_framebuffer *fb);
void drm_fb_unref(struct drm_device *dev, struct drm_framebuffer *fb);

/* CRTC management */
int  drm_add_crtc(struct drm_device *dev);
int  drm_add_connector(struct drm_device *dev, uint32_t type);

/* ── Multi-display support: CRTC ↔ connector routing ──────────── */

/*
 * drm_crtc_assign_connector — Assign/reassign a connector to a CRTC.
 *
 * Sets @connector's crtc_id to @crtc_id and updates the CRTC's
 * connector_mask accordingly.  If the connector was previously
 * assigned to a different CRTC, that CRTC's mask is cleared.
 *
 * Returns 0 on success, negative errno on failure.
 */
int  drm_crtc_assign_connector(struct drm_device *dev,
                                uint32_t crtc_id,
                                uint32_t connector_id);

/*
 * drm_connector_get_crtc — Return the CRTC id driving @connector_id.
 * Returns 0 if the connector is unassigned, or negative errno on error.
 */
int  drm_connector_get_crtc(struct drm_device *dev,
                             uint32_t connector_id);

/*
 * drm_crtc_connector_mask — Return the connector bitmask for a CRTC.
 * Returns non-negative mask on success, negative errno if CRTC not found.
 */
int  drm_crtc_connector_mask(struct drm_device *dev,
                              uint32_t crtc_id);

/* ── Multi-display helpers: clone / extend ──────────────────── */

/*
 * drm_display_clone_config — Configure clone mode for a set of connectors.
 *
 * Assigns all connectors identified by @connector_ids (count = @num_connectors)
 * to the same CRTC (@crtc_id), so they all display identical content.
 *
 * Returns 0 on success, negative errno on failure.
 */
int  drm_display_clone_config(struct drm_device *dev,
                               uint32_t crtc_id,
                               const uint32_t *connector_ids,
                               int num_connectors);

/*
 * drm_display_extend_config — Configure extend mode for a set of
 *                             CRTC–connector pairs.
 *
 * Each entry in @pairs assigns @crtc_id to @connector_id, creating an
 * extended desktop across all configured displays.
 *
 * Returns 0 on success, negative errno on failure.
 */
struct crtc_connector_pair {
    uint32_t crtc_id;
    uint32_t connector_id;
};

int  drm_display_extend_config(struct drm_device *dev,
                                const struct crtc_connector_pair *pairs,
                                int num_pairs);

/* ═══════════════════════════════════════════════════════════════════
 *  Damage tracking API
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * drm_damage_init — Initialise the damage tracking subsystem.
 * Returns 0 on success, negative errno on failure.
 */
int  drm_damage_init(void);

/*
 * drm_damage_exit — Tear down the damage tracking subsystem.
 */
void drm_damage_exit(void);

/*
 * drm_damage_init_fb — Initialise damage tracking for a framebuffer.
 * Sets damage_valid to 0 (no damage recorded yet).
 */
void drm_damage_init_fb(struct drm_framebuffer *fb);

/*
 * drm_damage_mark — Mark a rectangular region of the framebuffer as
 *                   damaged / needing refresh.
 *
 * The rectangle is clipped to the framebuffer dimensions.  When the
 * new rectangle is merged with existing damage, the accumulated
 * damage region expands to cover both.
 *
 * @fb:  Target framebuffer.
 * @x1:  Left edge (inclusive, pixels).
 * @y1:  Top edge (inclusive, pixels).
 * @x2:  Right edge (exclusive, pixels).
 * @y2:  Bottom edge (exclusive, pixels).
 */
void drm_damage_mark(struct drm_framebuffer *fb,
                      int x1, int y1, int x2, int y2);

/*
 * drm_damage_mark_whole — Mark the entire framebuffer as damaged.
 * Equivalent to drm_damage_mark(fb, 0, 0, fb->width, fb->height).
 */
void drm_damage_mark_whole(struct drm_framebuffer *fb);

/*
 * drm_damage_get_rect — Retrieve the accumulated damage rectangle.
 *
 * Returns 1 if damage has been recorded, 0 if clean.
 * The rectangle is written to the output pointers only on
 * non-NULL pointers and when damage_valid is set.
 *
 * @fb:   Target framebuffer.
 * @x1:   Receives left edge (may be NULL).
 * @y1:   Receives top edge (may be NULL).
 * @x2:   Receives right edge (may be NULL).
 * @y2:   Receives bottom edge (may be NULL).
 *
 * Returns 1 if damage recorded, 0 if clean, negative errno on error.
 */
int  drm_damage_get_rect(struct drm_framebuffer *fb,
                          int *x1, int *y1, int *x2, int *y2);

/*
 * drm_damage_clear — Clear (reset) damage tracking for a framebuffer.
 * After calling this, the fb is considered clean.
 */
void drm_damage_clear(struct drm_framebuffer *fb);

/* ═══════════════════════════════════════════════════════════════════
 *  GEM API
 * ═══════════════════════════════════════════════════════════════════ */

/* GEM object flags */
#define DRM_GEM_OBJECT_IMPORTED  (1U << 0)  /* imported via prime/dma-buf */
#define DRM_GEM_OBJECT_EXPORTED  (1U << 1)  /* exported via prime */
#define DRM_GEM_OBJECT_NO_MMAP   (1U << 2)  /* no mmap allowed */
#define DRM_GEM_OBJECT_VMAPPED   (1U << 3)  /* currently mmap'd */

struct drm_gem_object {
    uint32_t handle;
    size_t   size;
    void    *vaddr;       /* kernel virtual address */
    uint64_t phys_addr;   /* physical address (for contiguous) */
    int      is_contig;   /* 1 = physically contiguous */
    int      refcount;
    uint32_t flags;       /* DRM_GEM_OBJECT_* flags */
    uint32_t name;        /* global flink name (0 = none) */
    uint32_t num_pages;   /* number of physical pages backing this object */
    uint32_t vm_count;    /* number of active mmap mappings */
    int      prime_fd;    /* PRIME export FD (-1 = not exported) */
};

/* Global name (flink) support */
struct drm_gem_flink {
    uint32_t handle;
    uint32_t name;
};

struct drm_gem_open {
    uint32_t name;
    uint32_t handle;
    uint64_t size;
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

/* Enhanced GEM API */
struct drm_gem_object *drm_gem_object_lookup(struct drm_device *dev,
                                              struct drm_file *file_priv,
                                              uint32_t handle);
int  drm_gem_flink(struct drm_device *dev,
                   struct drm_gem_object *obj,
                   uint32_t *name);
int  drm_gem_open(struct drm_device *dev,
                  struct drm_file *file_priv,
                  uint32_t name,
                  uint32_t *handle,
                  uint64_t *size);
int  drm_gem_get_pages(struct drm_gem_object *obj,
                       uint64_t **pages_out,
                       uint32_t *num_pages_out);
int  drm_gem_put_pages(struct drm_gem_object *obj);
void drm_gem_vm_open(struct drm_gem_object *obj);
void drm_gem_vm_close(struct drm_gem_object *obj);

/* GEM ioctl handlers */
int  drm_gem_close_ioctl(struct drm_device *dev,
                         struct drm_file *file_priv,
                         uint32_t handle);
int  drm_gem_flink_ioctl(struct drm_device *dev,
                         struct drm_file *file_priv,
                         struct drm_gem_flink *args);
int  drm_gem_open_ioctl(struct drm_device *dev,
                        struct drm_file *file_priv,
                        struct drm_gem_open *args);

/* DRM PRIME FD sharing */
int  drm_gem_prime_handle_to_fd(struct drm_device *dev,
                                struct drm_file *file_priv,
                                struct drm_prime_handle *args);
int  drm_gem_prime_fd_to_handle(struct drm_device *dev,
                                struct drm_file *file_priv,
                                struct drm_prime_handle *args);
int  drm_prime_init(void);
void drm_prime_exit(void);

/* Called by drm_gem_free_object to release any PRIME FD reference */
void drm_prime_gem_destroy(struct drm_gem_object *obj);

/* Dumb buffer helpers */
int  drm_dumb_create(struct drm_device *dev,
                     struct drm_mode_create_dumb *args);
int  drm_dumb_map_offset(struct drm_device *dev,
                         struct drm_mode_map_dumb *args);
int  drm_dumb_destroy(struct drm_device *dev,
                      struct drm_mode_destroy_dumb *args);
int  drm_dumb_mmap(struct drm_gem_object *obj, void **vaddr);
int  drm_dumb_dirtyfb(struct drm_device *dev,
                      struct drm_mode_fb_dirty_cmd *args);

/* CRTC query / page flip ioctl handlers for multi-display support */
int  drm_ioctl_mode_getcrtc(struct drm_device *dev,
                             struct drm_file *fp, void *arg);
int  drm_ioctl_mode_page_flip(struct drm_device *dev,
                               struct drm_file *fp, void *arg);

/* ═══════════════════════════════════════════════════════════════════
 *  Multi-display layout ioctl (clone / extend)
 * ═══════════════════════════════════════════════════════════════════ */

#define DRM_MODE_LAYOUT_CLONE   0
#define DRM_MODE_LAYOUT_EXTEND  1
#define DRM_MODE_LAYOUT_SINGLE  2   /* restore single-display */

#define DRM_IOCTL_MODE_SET_LAYOUT DRM_IOWR(0xBD, struct drm_mode_set_layout)

/*
 * Layout configuration from userspace for clone / extend modes.
 *
 * For clone mode: all connectors in @connector_ids_ptr are assigned to
 * the same @crtc_id, so they all display identical content.
 *
 * For extend mode: @crtc_id and the connectors in @connector_ids_ptr
 * form a 1:1 assignment — the i-th connector is assigned to
 * (crtc_id + i) mod the number of available CRTCs, enabling independent
 * per-display content across an extended desktop.
 *
 * For restore single mode: restores the default layout where each
 * connector maps to CRTC with the same index.
 */
struct drm_mode_set_layout {
    uint32_t layout_type;           /* DRM_MODE_LAYOUT_CLONE / EXTEND / SINGLE */
    uint32_t crtc_id;               /* CRTC id for clone (or base CRTC for extend) */
    uint64_t connector_ids_ptr;     /* pointer to uint32_t connector ID array */
    uint32_t count_connectors;      /* number of connectors in the array */
    uint32_t pad;                   /* reserved, must be 0 */
};

/* Multi-display management module API */
int  drm_multi_init(void);
void drm_multi_exit(void);
int  drm_multi_set_layout(struct drm_device *dev,
                          const struct drm_mode_set_layout *layout);
int  drm_ioctl_mode_setlayout(struct drm_device *dev,
                              struct drm_file *fp, void *arg);

/* ═══════════════════════════════════════════════════════════════════
 *  DRM IRQ / vblank handling
 * ═══════════════════════════════════════════════════════════════════ */

/* WAIT_VBLANK ioctl — the standard DRM vblank synchronisation ioctl */
#define DRM_IOCTL_WAIT_VBLANK   DRM_IOWR(0x3A, union drm_wait_vblank)

/* VBLANK flags for drm_wait_vblank.request.type */
#define DRM_VBLANK_CRTC_MASK    0x000000FF  /* mask for CRTC index */
#define DRM_VBLANK_SIGNAL       0x10000000  /* deliver signal on vblank */
#define DRM_VBLANK_EVENT        0x20000000  /* deliver event on vblank */
#define DRM_VBLANK_HIGH_CRTC_MASK 0x0000003F

#define DRM_VBLANK_SECONDARY    0x20000000  /* secondary display */
#define DRM_VBLANK_NEXTONMISS   0x10000000  /* if missed seq, wait for next */

/* Type values for drm_wait_vblank.request.type & ~DRM_VBLANK_CRTC_MASK
 * (one of these OR'd with the CRTC index) */
#define _DRM_VBLANK_RELATIVE    0x00000000  /* seq is relative */
#define _DRM_VBLANK_ABSOLUTE    0x00000100  /* seq is absolute count */

struct drm_wait_vblank_request {
	uint32_t type;         /* DRM_VBLANK_* flags */
	uint32_t sequence;     /* requested sequence number */
	uint64_t signal;       /* reserved (signalling) */
};

struct drm_wait_vblank_reply {
	uint32_t type;         /* copy of request type */
	uint32_t sequence;     /* vblank count at time of reply */
	uint32_t tstamp_sec;   /* timestamp, seconds */
	uint32_t tstamp_usec;  /* timestamp, microseconds */
};

union drm_wait_vblank {
	struct drm_wait_vblank_request request;
	struct drm_wait_vblank_reply   reply;
};

/* Add TODO comment for IRQ lifecycle — these are called from drm_core.c */
int  drm_irq_init(void);
void drm_irq_exit(void);

#endif /* DRM_H */
