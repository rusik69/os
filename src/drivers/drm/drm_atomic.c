/*
 * drm_atomic.c — DRM atomic modesetting property framework
 *
 * Implements property enumeration, blob tracking, and the property
 * value infrastructure needed for atomic modesetting.  Properties
 * are key-value metadata attached to DRM objects (CRTCs, connectors,
 * planes) that describe their configuration.
 *
 * Architecture:
 *   - struct drm_property: a named typed property (range, enum, blob)
 *   - struct drm_property_blob: blob storage for large property values
 *   - struct prop_value_entry: per-object per-property value tracking
 *   - Property query via DRM_IOCTL_MODE_GETPROPERTY / GETPROPBLOB
 *   - Property set via DRM_IOCTL_MODE_SETPROPERTY (legacy path)
 *
 * Item D143 task 1 — DRM atomic modesetting property tracking
 */

#define KERNEL_INTERNAL
#include "drm.h"
#include "drm_atomic.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "pmm.h"
#include "errno.h"
#include "heap.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Internal property value entry (obj + prop → value mapping)
 * ═══════════════════════════════════════════════════════════════════ */

struct prop_value_entry {
	int      in_use;
	uint32_t obj_type;
	uint32_t obj_id;
	uint32_t prop_id;
	uint64_t value;
};

/* ═══════════════════════════════════════════════════════════════════
 *  Global state
 * ═══════════════════════════════════════════════════════════════════ */

#define DRM_MAX_PROPERTIES      64
#define DRM_MAX_PROP_VALUES    256

static struct drm_property       g_drm_properties[DRM_MAX_PROPERTIES];
static struct drm_property_blob  g_drm_blobs[DRM_MAX_BLOBS];
static struct prop_value_entry   g_prop_value_entries[DRM_MAX_PROP_VALUES];

static int        g_drm_atomic_inited = 0;
static spinlock_t g_drm_atomic_lock;
static uint32_t   g_next_prop_id = 1;
static uint32_t   g_next_blob_id = 1;

/* ═══════════════════════════════════════════════════════════════════
 *  Initialisation
 * ═══════════════════════════════════════════════════════════════════ */

int drm_atomic_init(void)
{
	if (g_drm_atomic_inited)
		return 0;

	spinlock_init(&g_drm_atomic_lock);

	memset(g_drm_properties, 0, sizeof(g_drm_properties));
	memset(g_drm_blobs, 0, sizeof(g_drm_blobs));
	memset(g_prop_value_entries, 0, sizeof(g_prop_value_entries));

	g_next_prop_id = 1;
	g_next_blob_id = 1;
	g_drm_atomic_inited = 1;

	kprintf("[DRM atomic] property framework initialised\n");
	return 0;
}

void drm_atomic_exit(void)
{
	spinlock_acquire(&g_drm_atomic_lock);

	/* Free all blob data */
	for (int i = 0; i < DRM_MAX_BLOBS; i++) {
		if (g_drm_blobs[i].in_use && g_drm_blobs[i].data) {
			kfree(g_drm_blobs[i].data);
		}
	}

	memset(g_drm_blobs, 0, sizeof(g_drm_blobs));
	memset(g_drm_properties, 0, sizeof(g_drm_properties));
	memset(g_prop_value_entries, 0, sizeof(g_prop_value_entries));

	g_drm_atomic_inited = 0;

	spinlock_release(&g_drm_atomic_lock);
	kprintf("[DRM atomic] property framework shut down\n");
}

/* ═══════════════════════════════════════════════════════════════════
 *  Property creation / lookup
 * ═══════════════════════════════════════════════════════════════════ */

int drm_property_create(uint32_t flags, const char *name,
                         uint32_t num_values, const uint64_t *values)
{
	if (!name || !g_drm_atomic_inited)
		return -EINVAL;

	spinlock_acquire(&g_drm_atomic_lock);

	for (int i = 0; i < DRM_MAX_PROPERTIES; i++) {
		if (!g_drm_properties[i].in_use) {
			struct drm_property *prop = &g_drm_properties[i];
			memset(prop, 0, sizeof(*prop));

			prop->in_use = 1;
			prop->prop_id = g_next_prop_id++;
			prop->flags = flags;

			/* Copy name (max 31 chars + null terminator) */
			size_t name_len = strlen(name);
			if (name_len > 31)
				name_len = 31;
			memcpy(prop->name, name, name_len);
			prop->name[name_len] = '\0';

			/* Copy preset values */
			uint32_t nv = num_values;
			if (nv > DRM_PROP_MAX_ENUMS)
				nv = DRM_PROP_MAX_ENUMS;
			prop->num_values = nv;
			for (uint32_t j = 0; j < nv; j++)
				prop->values[j] = values ? values[j] : 0;

			uint32_t id = prop->prop_id;
			spinlock_release(&g_drm_atomic_lock);

			kprintf("[DRM atomic] created property '%s' "
			        "id=%u flags=0x%x\n",
			        prop->name, id, flags);
			return (int)id;
		}
	}

	spinlock_release(&g_drm_atomic_lock);
	return -ENOMEM;
}

struct drm_property *drm_property_lookup(uint32_t prop_id)
{
	if (!g_drm_atomic_inited)
		return NULL;

	spinlock_acquire(&g_drm_atomic_lock);
	for (int i = 0; i < DRM_MAX_PROPERTIES; i++) {
		if (g_drm_properties[i].in_use &&
		    g_drm_properties[i].prop_id == prop_id) {
			spinlock_release(&g_drm_atomic_lock);
			return &g_drm_properties[i];
		}
	}
	spinlock_release(&g_drm_atomic_lock);
	return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Per-object property value tracking
 * ═══════════════════════════════════════════════════════════════════ */

int drm_object_property_set_value(uint32_t obj_type, uint32_t obj_id,
                                   uint32_t prop_id, uint64_t value)
{
	if (!g_drm_atomic_inited)
		return -EINVAL;

	spinlock_acquire(&g_drm_atomic_lock);

	/* Check the property exists */
	int prop_valid = 0;
	for (int i = 0; i < DRM_MAX_PROPERTIES; i++) {
		if (g_drm_properties[i].in_use &&
		    g_drm_properties[i].prop_id == prop_id) {
			prop_valid = 1;
			break;
		}
	}
	if (!prop_valid) {
		spinlock_release(&g_drm_atomic_lock);
		return -EINVAL;
	}

	/* Find existing entry for this (obj_type, obj_id, prop_id) */
	for (int i = 0; i < DRM_MAX_PROP_VALUES; i++) {
		if (g_prop_value_entries[i].in_use &&
		    g_prop_value_entries[i].obj_type == obj_type &&
		    g_prop_value_entries[i].obj_id == obj_id &&
		    g_prop_value_entries[i].prop_id == prop_id) {
			g_prop_value_entries[i].value = value;
			spinlock_release(&g_drm_atomic_lock);
			return 0;
		}
	}

	/* No existing entry — find a free slot */
	for (int i = 0; i < DRM_MAX_PROP_VALUES; i++) {
		if (!g_prop_value_entries[i].in_use) {
			g_prop_value_entries[i].in_use = 1;
			g_prop_value_entries[i].obj_type = obj_type;
			g_prop_value_entries[i].obj_id = obj_id;
			g_prop_value_entries[i].prop_id = prop_id;
			g_prop_value_entries[i].value = value;
			spinlock_release(&g_drm_atomic_lock);
			return 0;
		}
	}

	spinlock_release(&g_drm_atomic_lock);
	return -ENOSPC;
}

int drm_object_property_get_value(uint32_t obj_type, uint32_t obj_id,
                                   uint32_t prop_id, uint64_t *value)
{
	if (!g_drm_atomic_inited || !value)
		return -EINVAL;

	spinlock_acquire(&g_drm_atomic_lock);

	for (int i = 0; i < DRM_MAX_PROP_VALUES; i++) {
		if (g_prop_value_entries[i].in_use &&
		    g_prop_value_entries[i].obj_type == obj_type &&
		    g_prop_value_entries[i].obj_id == obj_id &&
		    g_prop_value_entries[i].prop_id == prop_id) {
			*value = g_prop_value_entries[i].value;
			spinlock_release(&g_drm_atomic_lock);
			return 0;
		}
	}

	spinlock_release(&g_drm_atomic_lock);
	return -ENOENT;
}

int drm_object_property_attach(uint32_t obj_type, uint32_t obj_id,
                                uint32_t prop_id)
{
	/* "Attaching" a property to an object is really just
	 * initializing its value to a default.  If there's already
	 * a value set, this is a no-op. */
	uint64_t dummy = 0;
	int ret = drm_object_property_get_value(obj_type, obj_id,
	                                        prop_id, &dummy);
	if (ret == 0)
		return 0;  /* already attached */

	/* Create with a default value of 0 */
	return drm_object_property_set_value(obj_type, obj_id,
	                                     prop_id, 0);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Blob management
 * ═══════════════════════════════════════════════════════════════════ */

int drm_property_create_blob(const void *data, size_t length,
                              uint32_t *blob_id)
{
	if (!data || !blob_id || length == 0 || !g_drm_atomic_inited)
		return -EINVAL;

	spinlock_acquire(&g_drm_atomic_lock);

	for (int i = 0; i < DRM_MAX_BLOBS; i++) {
		if (!g_drm_blobs[i].in_use) {
			struct drm_property_blob *blob = &g_drm_blobs[i];

			/* Allocate and copy blob data */
			void *blob_data = kmalloc(length);
			if (!blob_data) {
				spinlock_release(&g_drm_atomic_lock);
				return -ENOMEM;
			}
			memcpy(blob_data, data, length);

			memset(blob, 0, sizeof(*blob));
			blob->in_use = 1;
			blob->blob_id = g_next_blob_id++;
			blob->length = length;
			blob->data = blob_data;
			blob->refcount = 1;

			*blob_id = blob->blob_id;
			spinlock_release(&g_drm_atomic_lock);
			return 0;
		}
	}

	spinlock_release(&g_drm_atomic_lock);
	return -ENOSPC;
}

int drm_property_destroy_blob(uint32_t blob_id)
{
	if (!g_drm_atomic_inited)
		return -EINVAL;

	spinlock_acquire(&g_drm_atomic_lock);

	for (int i = 0; i < DRM_MAX_BLOBS; i++) {
		if (g_drm_blobs[i].in_use &&
		    g_drm_blobs[i].blob_id == blob_id) {
			if (g_drm_blobs[i].data)
				kfree(g_drm_blobs[i].data);
			memset(&g_drm_blobs[i], 0,
			       sizeof(struct drm_property_blob));
			spinlock_release(&g_drm_atomic_lock);
			return 0;
		}
	}

	spinlock_release(&g_drm_atomic_lock);
	return -ENOENT;
}

int drm_property_get_blob(uint32_t blob_id,
                           struct drm_property_blob **out_blob)
{
	if (!out_blob || !g_drm_atomic_inited)
		return -EINVAL;

	spinlock_acquire(&g_drm_atomic_lock);

	for (int i = 0; i < DRM_MAX_BLOBS; i++) {
		if (g_drm_blobs[i].in_use &&
		    g_drm_blobs[i].blob_id == blob_id) {
			*out_blob = &g_drm_blobs[i];
			spinlock_release(&g_drm_atomic_lock);
			return 0;
		}
	}

	spinlock_release(&g_drm_atomic_lock);
	return -ENOENT;
}

/* ═══════════════════════════════════════════════════════════════════
 *  ioctl handlers
 * ═══════════════════════════════════════════════════════════════════ */

int drm_ioctl_get_property(struct drm_device *dev, struct drm_file *fp,
                            struct drm_mode_get_property *arg)
{
	(void)dev;
	(void)fp;

	if (!arg)
		return -EINVAL;

	struct drm_property *prop = drm_property_lookup(arg->prop_id);
	if (!prop)
		return -ENOENT;

	/* Copy name */
	size_t name_len = strlen(prop->name);
	if (name_len > 31)
		name_len = 31;
	memcpy(arg->name, prop->name, name_len);
	arg->name[name_len] = '\0';

	arg->flags = prop->flags;
	arg->count_values = prop->num_values;

	/* Also store the enum values for enum properties */
	if (prop->flags & DRM_MODE_PROP_ENUM) {
		arg->count_enum_blobs = prop->num_values;
		arg->count_enum_blob_values = 0;
	}

	/* Copy values to userspace if requested */
	if (arg->count_values > 0 && arg->values_ptr) {
		uint64_t *values_ptr = (uint64_t *)(uintptr_t)arg->values_ptr;
		for (uint32_t i = 0; i < prop->num_values; i++) {
			values_ptr[i] = prop->values[i];
		}
	}

	/* For enum properties, copy the enum descriptions */
	if ((prop->flags & DRM_MODE_PROP_ENUM) && arg->enum_blob_ptr) {
		struct drm_mode_property_enum *ep =
		    (struct drm_mode_property_enum *)
		    (uintptr_t)arg->enum_blob_ptr;
		/* Use a default enum name if values are just indices */
		for (uint32_t i = 0; i < prop->num_values; i++) {
			ep[i].value = prop->values[i];
			memset(ep[i].name, 0, sizeof(ep[i].name));
			/* Generate name from the value */
			ep[i].name_len = (uint32_t)
			    snprintf(ep[i].name, sizeof(ep[i].name),
			             "val%llu",
			             (unsigned long long)prop->values[i]);
		}
	}

	return 0;
}

int drm_ioctl_set_property(struct drm_device *dev, struct drm_file *fp,
                            struct drm_mode_set_property *arg)
{
	(void)dev;
	(void)fp;

	if (!arg)
		return -EINVAL;

	struct drm_property *prop = drm_property_lookup(arg->prop_id);
	if (!prop)
		return -ENOENT;

	if (prop->flags & DRM_MODE_PROP_IMMUTABLE)
		return -EACCES;

	int ret = drm_object_property_set_value(arg->obj_type,
	                                         arg->obj_id,
	                                         arg->prop_id,
	                                         arg->value);
	return ret;
}

int drm_ioctl_get_property_blob(struct drm_device *dev,
                                 struct drm_file *fp,
                                 struct drm_mode_get_blob *arg)
{
	(void)dev;
	(void)fp;

	if (!arg)
		return -EINVAL;

	struct drm_property_blob *blob = NULL;
	int ret = drm_property_get_blob(arg->blob_id, &blob);
	if (ret < 0)
		return ret;

	/* Return the length first */
	arg->length = (uint32_t)blob->length;

	/* If the user provides a buffer, copy data into it */
	if (arg->length > 0 && arg->data && blob->data) {
		uint32_t copy_len = arg->length;
		if ((size_t)copy_len > blob->length)
			copy_len = (uint32_t)blob->length;
		memcpy((void *)(uintptr_t)arg->data, blob->data,
		       copy_len);
	}

	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Atomic check + commit
 * ═══════════════════════════════════════════════════════════════════
 *
 * DRM_IOCTL_MODE_ATOMIC allows userspace to atomically set multiple
 * property values across several objects (CRTCs, connectors) in a
 * single operation.
 *
 *   Check phase:  validates the entire configuration without applying.
 *   Commit phase: applies all validated changes atomically.
 *
 * Item D143 task 2 — DRM atomic check + commit
 */

/* ── Per-object property value in an atomic request ─────────────── */

struct atomic_prop_val {
	uint32_t prop_id;
	uint64_t value;
};

/* ── Parsed atomic request for a single object ──────────────────── */

struct atomic_obj_req {
	uint32_t       obj_id;
	uint32_t       obj_type;   /* DRM_MODE_OBJECT_* */
	uint32_t       num_props;
	struct atomic_prop_val *props;
};

/* ── Parsed atomic request (all objects) ────────────────────────── */

struct atomic_req {
	uint32_t           flags;
	uint32_t           num_objs;
	struct atomic_obj_req *objs;
};

/* Forward declarations */
static int atomic_resolve_obj_type(struct drm_device *dev, uint32_t obj_id,
                                    uint32_t *obj_type);
static int atomic_check_obj(struct drm_device *dev,
                             const struct atomic_obj_req *obj_req);
static int atomic_commit_obj(struct drm_device *dev,
                              const struct atomic_obj_req *obj_req);

/* ═══════════════════════════════════════════════════════════════════
 *  Parser — copy the atomic request from userspace
 * ═══════════════════════════════════════════════════════════════════
 *
 * The userspace layout is:
 *   objs_ptr[]       — uint32_t object IDs (count_objs entries)
 *   count_props_ptr[] — uint32_t prop counts per object (count_objs)
 *   props_ptr[]      — concatenated uint32_t prop IDs
 *   prop_values_ptr[] — concatenated uint64_t prop values
 */

static int atomic_parse_request(struct drm_device *dev,
                                 const struct drm_mode_atomic *arg,
                                 struct atomic_req *req)
{
	(void)dev;

	memset(req, 0, sizeof(*req));
	req->flags = arg->flags;
	req->num_objs = arg->count_objs;

	if (req->num_objs == 0)
		return 0;

	/* Read array pointers from userspace */
	const uint32_t *obj_ids =
	    (const uint32_t *)(uintptr_t)arg->objs_ptr;
	const uint32_t *count_props =
	    (const uint32_t *)(uintptr_t)arg->count_props_ptr;
	const uint32_t *prop_ids =
	    (const uint32_t *)(uintptr_t)arg->props_ptr;
	const uint64_t *prop_values =
	    (const uint64_t *)(uintptr_t)arg->prop_values_ptr;

	if (!obj_ids || !count_props || !prop_ids || !prop_values)
		return -EFAULT;

	/* Allocate object request array */
	req->objs = (struct atomic_obj_req *)
	    kmalloc(sizeof(struct atomic_obj_req) * req->num_objs);
	if (!req->objs)
		return -ENOMEM;
	memset(req->objs, 0,
	       sizeof(struct atomic_obj_req) * req->num_objs);

	/* Walk concatenated arrays to build per-object property lists */
	uint32_t prop_idx = 0;
	for (uint32_t i = 0; i < req->num_objs; i++) {
		struct atomic_obj_req *o = &req->objs[i];
		o->obj_id = obj_ids[i];
		o->num_props = count_props[i];
		o->obj_type = 0;

		if (o->num_props == 0)
			continue;

		o->props = (struct atomic_prop_val *)
		    kmalloc(sizeof(struct atomic_prop_val) *
		            o->num_props);
		if (!o->props) {
			/* Clean up previous allocations */
			for (uint32_t j = 0; j < i; j++)
				if (req->objs[j].props)
					kfree(req->objs[j].props);
			kfree(req->objs);
			req->objs = NULL;
			return -ENOMEM;
		}

		for (uint32_t j = 0; j < o->num_props; j++) {
			o->props[j].prop_id = prop_ids[prop_idx];
			o->props[j].value = prop_values[prop_idx];
			prop_idx++;
		}
	}

	return 0;
}

static void atomic_free_request(struct atomic_req *req)
{
	if (!req || !req->objs)
		return;
	for (uint32_t i = 0; i < req->num_objs; i++) {
		if (req->objs[i].props)
			kfree(req->objs[i].props);
	}
	kfree(req->objs);
	req->objs = NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Object type resolution
 * ═══════════════════════════════════════════════════════════════════ */

static int atomic_resolve_obj_type(struct drm_device *dev,
                                    uint32_t obj_id,
                                    uint32_t *obj_type)
{
	if (!dev || !obj_type)
		return -EINVAL;

	/* Check CRTCs */
	for (int i = 0; i < DRM_MAX_CRTC; i++) {
		if (dev->crtcs[i].in_use &&
		    dev->crtcs[i].crtc_id == obj_id) {
			*obj_type = DRM_MODE_OBJECT_CRTC;
			return 0;
		}
	}

	/* Check connectors */
	for (int i = 0; i < DRM_MAX_CONNECTOR; i++) {
		if (dev->connectors[i].in_use &&
		    dev->connectors[i].connector_id == obj_id) {
			*obj_type = DRM_MODE_OBJECT_CONNECTOR;
			return 0;
		}
	}

	/* Check framebuffers */
	for (int i = 0; i < DRM_MAX_FB; i++) {
		if (dev->framebuffers[i].in_use &&
		    dev->framebuffers[i].fb_id == obj_id) {
			*obj_type = DRM_MODE_OBJECT_ANY;
			return 0;
		}
	}

	return -ENOENT;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Check phase — validate a single object's property changes
 * ═══════════════════════════════════════════════════════════════════ */

static int atomic_check_obj(struct drm_device *dev,
                             const struct atomic_obj_req *obj_req)
{
	uint32_t obj_type = 0;
	int ret = atomic_resolve_obj_type(dev, obj_req->obj_id,
	                                   &obj_type);
	if (ret < 0)
		return -ENOENT;

	/* Validate each property */
	for (uint32_t i = 0; i < obj_req->num_props; i++) {
		struct drm_property *prop =
		    drm_property_lookup(obj_req->props[i].prop_id);
		if (!prop)
			return -EINVAL;

		/* Immutable properties cannot be changed */
		if (prop->flags & DRM_MODE_PROP_IMMUTABLE)
			return -EACCES;

		/* For range properties, check value within bounds */
		if (prop->flags & DRM_MODE_PROP_RANGE) {
			if (prop->num_values >= 2) {
				uint64_t min_val = prop->values[0];
				uint64_t max_val = prop->values[1];
				if (obj_req->props[i].value < min_val ||
				    obj_req->props[i].value > max_val)
					return -ERANGE;
			}
		}

		/* For enum properties, check valid enum value */
		if (prop->flags & DRM_MODE_PROP_ENUM) {
			int valid = 0;
			for (uint32_t e = 0; e < prop->num_values; e++) {
				if (prop->values[e] ==
				    obj_req->props[i].value) {
					valid = 1;
					break;
				}
			}
			if (!valid)
				return -EINVAL;
		}

		/* For object properties, check target exists */
		if (prop->flags & DRM_MODE_PROP_OBJECT) {
			if (obj_req->props[i].value != 0) {
				uint32_t dummy_type = 0;
				ret = atomic_resolve_obj_type(
				    dev,
				    (uint32_t)obj_req->props[i].value,
				    &dummy_type);
				if (ret < 0)
					return -EINVAL;
			}
		}
	}

	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Commit phase — apply a single object's property changes
 * ═══════════════════════════════════════════════════════════════════ */

static int atomic_commit_obj(struct drm_device *dev,
                              const struct atomic_obj_req *obj_req)
{
	uint32_t obj_type = 0;
	int ret = atomic_resolve_obj_type(dev, obj_req->obj_id,
	                                   &obj_type);
	if (ret < 0)
		return ret;

	/* Apply each property value */
	for (uint32_t i = 0; i < obj_req->num_props; i++) {
		ret = drm_object_property_set_value(obj_type,
		             obj_req->obj_id,
		             obj_req->props[i].prop_id,
		             obj_req->props[i].value);
		if (ret < 0) {
			kprintf("[DRM atomic] commit: set_property "
			        "failed (obj=%u prop=%u ret=%d)\n",
			        obj_req->obj_id,
			        obj_req->props[i].prop_id, ret);
			return ret;
		}

		/* CRTC-specific side-effects */
		if (obj_type == DRM_MODE_OBJECT_CRTC) {
			struct drm_property *prop =
			    drm_property_lookup(
			        obj_req->props[i].prop_id);
			if (prop) {
				/* FB_ID -> update CRTC fb_id with refcount */
				if (!strcmp(prop->name, "FB_ID")) {
					for (int c = 0;
					     c < DRM_MAX_CRTC; c++) {
						if (dev->crtcs[c].in_use &&
						    dev->crtcs[c].crtc_id ==
						    obj_req->obj_id) {
							uint32_t old_fb_id =
							    dev->crtcs[c]
							        .fb_id;
							uint32_t new_fb_id =
							    (uint32_t)
							    obj_req->props[i]
							        .value;

							/* Take a reference on
							 * the new FB before
							 * releasing the old one,
							 * to avoid a window
							 * where refcount = 0 */
							if (new_fb_id != 0) {
								struct drm_framebuffer
								    *new_fb =
								    drm_fb_lookup(
								    dev,
								    new_fb_id);
								if (new_fb)
									drm_fb_ref(
									    new_fb);
							}

							dev->crtcs[c].fb_id =
							    new_fb_id;

							/* Release the old FB
							 * reference */
							if (old_fb_id != 0 &&
							    old_fb_id !=
							    new_fb_id) {
								struct drm_framebuffer
								    *old_fb =
								    drm_fb_lookup(
								    dev,
								    old_fb_id);
								if (old_fb)
									drm_fb_unref(
									    dev,
									    old_fb);
							}
							break;
						}
					}
				}
				/* ACTIVE -> toggle CRTC enable */
				if (!strcmp(prop->name, "ACTIVE")) {
					for (int c = 0;
					     c < DRM_MAX_CRTC; c++) {
						if (dev->crtcs[c].in_use &&
						    dev->crtcs[c].crtc_id ==
						    obj_req->obj_id) {
							dev->crtcs[c].enabled =
							    obj_req->props[i]
							        .value ? 1 : 0;
							break;
						}
					}
				}
			}
		}

		/* Connector-specific side-effects */
		if (obj_type == DRM_MODE_OBJECT_CONNECTOR) {
			struct drm_property *prop =
			    drm_property_lookup(
			        obj_req->props[i].prop_id);
			if (prop) {
				/* CRTC_ID -> connector routing */
				if (!strcmp(prop->name, "CRTC_ID")) {
					kprintf("[DRM atomic] connector %u "
					        "routed to CRTC %llu\n",
					        obj_req->obj_id,
					        (unsigned long long)
					        obj_req->props[i].value);
				}
			}
		}
	}

	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Top-level check — validate the entire atomic request
 * ═══════════════════════════════════════════════════════════════════ */

int drm_atomic_check(struct drm_device *dev,
                      const struct drm_mode_atomic *arg)
{
	struct atomic_req req;
	int ret = atomic_parse_request(dev, arg, &req);
	if (ret < 0)
		return ret;

	for (uint32_t i = 0; i < req.num_objs; i++) {
		ret = atomic_check_obj(dev, &req.objs[i]);
		if (ret < 0) {
			kprintf("[DRM atomic] check failed on "
			        "object %u (ret=%d)\n",
			        req.objs[i].obj_id, ret);
			goto out;
		}
	}

out:
	atomic_free_request(&req);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Top-level commit — apply the entire atomic request
 * ═══════════════════════════════════════════════════════════════════
 *
 * Must only be called after drm_atomic_check succeeds.
 */

int drm_atomic_commit(struct drm_device *dev,
                       const struct drm_mode_atomic *arg)
{
	struct atomic_req req;
	int ret = atomic_parse_request(dev, arg, &req);
	if (ret < 0)
		return ret;

	for (uint32_t i = 0; i < req.num_objs; i++) {
		ret = atomic_commit_obj(dev, &req.objs[i]);
		if (ret < 0) {
			kprintf("[DRM atomic] commit failed on "
			        "object %u (ret=%d)\n",
			        req.objs[i].obj_id, ret);
			goto out;
		}
	}

	kprintf("[DRM atomic] committed %u objects\n",
	        req.num_objs);

out:
	atomic_free_request(&req);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════════
 *  IOCTL handler — DRM_IOCTL_MODE_ATOMIC
 * ═══════════════════════════════════════════════════════════════════
 *
 * Top-level entry point for atomic modesetting from userspace.
 * Handles TEST_ONLY, NONBLOCK, and ALLOW_MODESET flags.
 */

int drm_ioctl_atomic(struct drm_device *dev, struct drm_file *fp,
                      struct drm_mode_atomic *arg)
{
	(void)fp;

	if (!dev || !arg)
		return -EINVAL;

	/* Validate flags */
	uint32_t valid_flags = DRM_MODE_ATOMIC_TEST_ONLY |
	                       DRM_MODE_ATOMIC_NONBLOCK |
	                       DRM_MODE_ATOMIC_ALLOW_MODESET;
	if (arg->flags & ~valid_flags) {
		kprintf("[DRM atomic] invalid flags 0x%x\n",
		        arg->flags);
		return -EINVAL;
	}

	/* Check phase — validate the entire request */
	int ret = drm_atomic_check(dev, arg);
	if (ret < 0) {
		kprintf("[DRM atomic] check failed (ret=%d)\n", ret);
		return ret;
	}

	/* If TEST_ONLY, stop after check */
	if (arg->flags & DRM_MODE_ATOMIC_TEST_ONLY) {
		kprintf("[DRM atomic] test-only check passed "
		        "(%u objects)\n", arg->count_objs);
		return 0;
	}

	/* Commit phase — apply all changes */
	ret = drm_atomic_commit(dev, arg);
	if (ret < 0) {
		kprintf("[DRM atomic] commit failed (ret=%d)\n", ret);
		return ret;
	}

	return 0;
}
