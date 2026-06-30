#ifndef DRM_ATOMIC_H
#define DRM_ATOMIC_H

#include "types.h"

/* ═══════════════════════════════════════════════════════════════════
 *  DRM atomic modesetting property types
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Property flags ─────────────────────────────────────────────── */

#define DRM_MODE_PROP_RANGE        (1U << 0)
#define DRM_MODE_PROP_ENUM         (1U << 1)
#define DRM_MODE_PROP_BLOB         (1U << 2)
#define DRM_MODE_PROP_BITMASK      (1U << 3)
#define DRM_MODE_PROP_OBJECT       (1U << 4)
#define DRM_MODE_PROP_SIGNED_RANGE (1U << 5)
#define DRM_MODE_PROP_IMMUTABLE    (1U << 30)
#define DRM_MODE_PROP_ATOMIC       (1U << 31)

/* ── Atomic ioctl flags ─────────────────────────────────────────── */

#define DRM_MODE_ATOMIC_TEST_ONLY      (1U << 0)
#define DRM_MODE_ATOMIC_NONBLOCK       (1U << 1)
#define DRM_MODE_ATOMIC_ALLOW_MODESET  (1U << 2)

/* ── Object type identifiers for property lookup ────────────────── */

#define DRM_MODE_OBJECT_CRTC      0xCCCCCCCC  /* magic for CRTC */
#define DRM_MODE_OBJECT_CONNECTOR 0xCCCCCCCD
#define DRM_MODE_OBJECT_PLANE     0xCCCCCCCE
#define DRM_MODE_OBJECT_BLOB      0xCCCCCCCF
#define DRM_MODE_OBJECT_ANY       0xCCCCCCD0

/* ── ioctl commands ─────────────────────────────────────────────── */

/* These use DRM_IOWR with struct drm_mode_get_property and friends */
#define DRM_IOCTL_MODE_GETPROPERTY   DRM_IOWR(0xAA, struct drm_mode_get_property)
#define DRM_IOCTL_MODE_SETPROPERTY   DRM_IOWR(0xAB, struct drm_mode_set_property)
#define DRM_IOCTL_MODE_GETPROPBLOB   DRM_IOWR(0xAC, struct drm_mode_get_blob)
#define DRM_IOCTL_MODE_ATOMIC        DRM_IOWR(0xBC, struct drm_mode_atomic)

/* ═══════════════════════════════════════════════════════════════════
 *  Userspace-visible structures for atomic/property ioctls
 * ═══════════════════════════════════════════════════════════════════ */

/* Property definition (returned by GETPROPERTY) */
struct drm_mode_property_enum {
    uint64_t value;
    uint32_t name_len;
    uint32_t pad;
    char     name[32];
};

struct drm_mode_get_property {
    uint64_t values_ptr;      /* values (uint64_t array) */
    uint64_t enum_blob_ptr;   /* enum/blob data */
    uint32_t prop_id;
    uint32_t flags;
    char     name[32];
    uint32_t count_values;
    uint32_t count_enum_blobs;
    uint32_t count_enum_blob_values;
};

struct drm_mode_set_property {
    uint32_t obj_id;
    uint32_t obj_type;        /* DRM_MODE_OBJECT_* */
    uint32_t prop_id;
    uint32_t pad;
    uint64_t value;
};

struct drm_mode_get_blob {
    uint32_t blob_id;
    uint32_t length;
    uint64_t data;
};

/* Atomic commit request (one object's property change) */
struct drm_mode_atomic {
    uint32_t flags;             /* DRM_MODE_ATOMIC_* */
    uint32_t count_objs;
    uint64_t objs_ptr;          /* uint32_t object IDs */
    uint64_t count_props_ptr;   /* uint32_t per-object prop counts */
    uint64_t props_ptr;         /* uint32_t prop ID arrays */
    uint64_t prop_values_ptr;   /* uint64_t value arrays */
    uint64_t reserved;
    uint64_t user_data;
};

/* Property blob (opaque binary data) */
struct drm_property_blob {
    uint32_t blob_id;
    size_t   length;
    void    *data;
    int      refcount;
    int      in_use;
};

/* ═══════════════════════════════════════════════════════════════════
 *  Internal property representation
 * ═══════════════════════════════════════════════════════════════════ */

/* Maximum property enumerations per property */
#define DRM_PROP_MAX_ENUMS 16

/* Maximum properties per object type */
#define DRM_MAX_PROPERTIES_PER_OBJ 32

/* Maximum blob objects */
#define DRM_MAX_BLOBS 64

/* A property defined by a driver (type + allowed values) */
struct drm_property {
    int      in_use;
    uint32_t prop_id;
    uint32_t flags;               /* DRM_MODE_PROP_* */
    char     name[32];
    uint32_t num_values;
    uint64_t values[DRM_PROP_MAX_ENUMS];
};

/* Per-object property value tracking */
struct drm_property_value {
    int      in_use;
    uint32_t prop_id;
    uint64_t value;
};

/* ═══════════════════════════════════════════════════════════════════
 *  Internal API (declared for drm_atomic.c and drm_core.c)
 * ═══════════════════════════════════════════════════════════════════ */

/* Property lifecycle */
int  drm_atomic_init(void);
void drm_atomic_exit(void);

/* Property creation / lookup */
int  drm_property_create(uint32_t flags, const char *name,
                          uint32_t num_values, const uint64_t *values);
struct drm_property *drm_property_lookup(uint32_t prop_id);

/* Per-object property value tracking */
int  drm_object_property_set_value(uint32_t obj_type, uint32_t obj_id,
                                    uint32_t prop_id, uint64_t value);
int  drm_object_property_get_value(uint32_t obj_type, uint32_t obj_id,
                                    uint32_t prop_id, uint64_t *value);
int  drm_object_property_attach(uint32_t obj_type, uint32_t obj_id,
                                 uint32_t prop_id);

/* Blob management */
int  drm_property_create_blob(const void *data, size_t length,
                               uint32_t *blob_id);
int  drm_property_destroy_blob(uint32_t blob_id);
int  drm_property_get_blob(uint32_t blob_id,
                            struct drm_property_blob **out_blob);

/* ioctl handlers for drm_core.c dispatch */
int  drm_ioctl_get_property(struct drm_device *dev, struct drm_file *fp,
                             struct drm_mode_get_property *arg);
int  drm_ioctl_set_property(struct drm_device *dev, struct drm_file *fp,
                             struct drm_mode_set_property *arg);
int  drm_ioctl_get_property_blob(struct drm_device *dev,
                                  struct drm_file *fp,
                                  struct drm_mode_get_blob *arg);

#endif /* DRM_ATOMIC_H */
