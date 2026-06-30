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
