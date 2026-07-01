/*
 * drm_gem.c — DRM GEM (Graphics Execution Manager) buffer management
 *
 * Provides buffer object allocation (physically contiguous or SG-backed),
 * handle creation, lookup, and reference counting for buffer lifetime.
 * Supports mmap() for userspace access to buffers.
 *
 * Architecture:
 *   - struct drm_gem_object: represents a buffer object
 *   - Handle table: maps userspace handles to GEM objects
 *   - Reference counting via drm_gem_ref / drm_gem_unref
 *   - mmap() via physical memory mapping
 *
 * Item S20 — DRM GEM
 */

#define KERNEL_INTERNAL
#include "drm.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "errno.h"
#include "err.h"
#include "printf.h"
#include "spinlock.h"
#include "heap.h"

/* ── Handle table ──────────────────────────────────────────────── */

#define GEM_MAX_HANDLES 256
#define GEM_MAX_OBJECTS 64

struct gem_handle_entry {
    int      in_use;
    uint32_t handle;
    struct drm_gem_object *obj;
};

static struct gem_handle_entry g_gem_handles[GEM_MAX_HANDLES];
static struct drm_gem_object   g_gem_objects[GEM_MAX_OBJECTS];
static int g_gem_initialized = 0;
static spinlock_t g_gem_lock;
static uint32_t g_next_handle = 1;

/* ── Global name (flink) table ───────────────────────────────── */

#define GEM_MAX_NAMES 64

struct gem_name_entry {
    int      in_use;
    uint32_t name;       /* global name (flink) */
    struct drm_gem_object *obj;
};

static struct gem_name_entry g_gem_names[GEM_MAX_NAMES];
static uint32_t g_next_name = 1;

/* ═══════════════════════════════════════════════════════════════════
 *  Initialisation
 * ═══════════════════════════════════════════════════════════════════ */

int drm_gem_init(void)
{
    spinlock_init(&g_gem_lock);
    memset(g_gem_handles, 0, sizeof(g_gem_handles));
    memset(g_gem_objects, 0, sizeof(g_gem_objects));
    memset(g_gem_names, 0, sizeof(g_gem_names));
    g_gem_initialized = 1;
    g_next_handle = 1;
    g_next_name = 1;
    kprintf("[DRM GEM] buffer manager initialised\n");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Object allocation and freeing
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * drm_gem_create_object — Allocate a GEM buffer object.
 *
 * @size:      Desired buffer size (bytes), aligned to page size.
 * @is_contig: 1 for physically contiguous, 0 for SG (scatter-gather).
 * @out_obj:   Receives pointer to the allocated object.
 *
 * Returns 0 on success.
 */
int drm_gem_create_object(size_t size, int is_contig,
                           struct drm_gem_object **out_obj)
{
    if (!out_obj || size == 0)
        return -1;

    /* Round up to page size */
    size = (size + 0xFFF) & ~0xFFFULL;

    spinlock_acquire(&g_gem_lock);

    /* Find a free object slot */
    int obj_idx = -1;
    for (int i = 0; i < GEM_MAX_OBJECTS; i++) {
        if (!g_gem_objects[i].refcount) {
            obj_idx = i;
            break;
        }
    }
    if (obj_idx < 0) {
        spinlock_release(&g_gem_lock);
        return -ENOMEM;
    }

    struct drm_gem_object *obj = &g_gem_objects[obj_idx];
    memset(obj, 0, sizeof(*obj));
    obj->prime_fd = -1;  /* not exported */
    obj->size = size;
    obj->is_contig = is_contig;
    obj->refcount = 1;
    obj->num_pages = (uint32_t)(size / 0x1000);

    /* Allocate physical memory */
    if (is_contig) {
        /* Physically contiguous — allocate from PMM */
        uint32_t num_pages = (uint32_t)(size / 0x1000);
        uint64_t *pages = pmm_alloc_frames(num_pages);
        if (!pages) {
            spinlock_release(&g_gem_lock);
            return -ENOMEM;
        }
        obj->phys_addr = pages[0];  /* first page address */
        /* Map into kernel virtual space */
        obj->vaddr = vmm_map_phys(obj->phys_addr, size,
                                   VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
        if (IS_ERR(obj->vaddr)) {
            pmm_free_frames_contiguous(obj->phys_addr, num_pages);
            spinlock_release(&g_gem_lock);
            return -ENOMEM;
        }
        /* Zero the buffer */
        memset(obj->vaddr, 0, size);
    } else {
        /* SG-backed: allocate individual pages */
        uint32_t num_pages = (uint32_t)(size / 0x1000);
        /* For simplicity, allocate contiguous + map */
        uint64_t *pages = pmm_alloc_frames(num_pages);
        if (!pages) {
            spinlock_release(&g_gem_lock);
            return -ENOMEM;
        }
        obj->phys_addr = pages[0];
        obj->vaddr = vmm_map_phys(obj->phys_addr, size,
                                   VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
        if (IS_ERR(obj->vaddr)) {
            pmm_free_frames_contiguous(obj->phys_addr, num_pages);
            spinlock_release(&g_gem_lock);
            return -ENOMEM;
        }
        memset(obj->vaddr, 0, size);
    }

    spinlock_release(&g_gem_lock);
    *out_obj = obj;
    return 0;
}

/*
 * drm_gem_free_object — Free a GEM buffer object.
 *
 * This is called when refcount drops to zero.
 */
void drm_gem_free_object(struct drm_gem_object *obj)
{
    if (!obj) return;

    /* Release any PRIME FD reference */
    drm_prime_gem_destroy(obj);

    /* Release any flink name if one was assigned */
    if (obj->name != 0) {
        spinlock_acquire(&g_gem_lock);
        for (int i = 0; i < GEM_MAX_NAMES; i++) {
            if (g_gem_names[i].in_use && g_gem_names[i].obj == obj) {
                g_gem_names[i].in_use = 0;
                g_gem_names[i].name = 0;
                g_gem_names[i].obj = NULL;
                break;
            }
        }
        spinlock_release(&g_gem_lock);
    }

    /* Unmap from kernel space */
    if (obj->vaddr) {
        vmm_unmap_phys(obj->vaddr, obj->size);
    }

    /* Free physical pages */
    if (obj->phys_addr) {
        uint32_t num_pages = (uint32_t)(obj->size / 0x1000);
        pmm_free_frames_contiguous(obj->phys_addr, num_pages);
    }

    memset(obj, 0, sizeof(*obj));
}

/* ═══════════════════════════════════════════════════════════════════
 *  Reference counting
 * ═══════════════════════════════════════════════════════════════════ */

void drm_gem_ref(struct drm_gem_object *obj)
{
    if (!obj) return;
    spinlock_acquire(&g_gem_lock);
    obj->refcount++;
    spinlock_release(&g_gem_lock);
}

void drm_gem_unref(struct drm_gem_object *obj)
{
    if (!obj) return;
    spinlock_acquire(&g_gem_lock);
    obj->refcount--;
    int do_free = (obj->refcount <= 0);
    spinlock_release(&g_gem_lock);

    if (do_free)
        drm_gem_free_object(obj);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Handle management
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * drm_gem_handle_create — Create a userspace-visible handle for a GEM object.
 *
 * @dev:    DRM device.
 * @obj:    GEM object to create handle for.
 * @handle: Receives the new handle value.
 *
 * Returns 0 on success.
 */
int drm_gem_handle_create(struct drm_device *dev,
                           struct drm_gem_object *obj,
                           uint32_t *handle)
{
    (void)dev;
    if (!obj || !handle)
        return -1;

    spinlock_acquire(&g_gem_lock);

    uint32_t h = g_next_handle++;
    if (g_next_handle == 0) g_next_handle = 1;

    /* Find a free handle slot */
    int idx = -1;
    for (int i = 0; i < GEM_MAX_HANDLES; i++) {
        if (!g_gem_handles[i].in_use) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        spinlock_release(&g_gem_lock);
        return -ENOMEM;
    }

    g_gem_handles[idx].in_use = 1;
    g_gem_handles[idx].handle = h;
    g_gem_handles[idx].obj = obj;

    /* Take a reference for the handle */
    drm_gem_ref(obj);

    spinlock_release(&g_gem_lock);

    *handle = h;
    return 0;
}

/*
 * drm_gem_handle_lookup — Look up a GEM object by handle.
 *
 * Returns the object pointer, or NULL if not found.
 */
struct drm_gem_object *drm_gem_handle_lookup(struct drm_device *dev,
                                              uint32_t handle)
{
    (void)dev;
    spinlock_acquire(&g_gem_lock);

    for (int i = 0; i < GEM_MAX_HANDLES; i++) {
        if (g_gem_handles[i].in_use && g_gem_handles[i].handle == handle) {
            struct drm_gem_object *obj = g_gem_handles[i].obj;
            spinlock_release(&g_gem_lock);
            return obj;
        }
    }

    spinlock_release(&g_gem_lock);
    return NULL;
}

/*
 * drm_gem_handle_close — Close a GEM handle, dropping the associated ref.
 *
 * Returns 0 on success.
 */
int drm_gem_handle_close(struct drm_device *dev, uint32_t handle)
{
    (void)dev;
    spinlock_acquire(&g_gem_lock);

    for (int i = 0; i < GEM_MAX_HANDLES; i++) {
        if (g_gem_handles[i].in_use && g_gem_handles[i].handle == handle) {
            struct drm_gem_object *obj = g_gem_handles[i].obj;
            g_gem_handles[i].in_use = 0;
            g_gem_handles[i].handle = 0;
            g_gem_handles[i].obj = NULL;
            spinlock_release(&g_gem_lock);

            /* Drop the handle's reference */
            drm_gem_unref(obj);
            return 0;
        }
    }

    spinlock_release(&g_gem_lock);
    return -1;
}

/*
 * drm_gem_mmap — Prepare a GEM object for mmap().
 *
 * Returns an offset that userspace can use with mmap().
 * In this simplified kernel, we return the physical address offset
 * from the framebuffer base.
 */
int drm_gem_mmap(struct drm_gem_object *obj, uint64_t *offset)
{
    if (!obj || !offset)
        return -1;

    /* Return the physical address as the mmap offset.
     * The userspace mmap handler would map this physical address. */
    *offset = obj->phys_addr;
    return 0;
}

/* ── Implement: drm_gem_object_init ─────────────────────────────── */
int drm_gem_object_init(void *dev, void *obj, size_t size)
{
    (void)dev;
    if (!obj || size == 0) return -EINVAL;

    /* Initialize a GEM object in-place (as opposed to creating a new one).
     * Zero the object header and set the size. */
    struct drm_gem_object *gem_obj = (struct drm_gem_object *)obj;
    memset(gem_obj, 0, sizeof(*gem_obj));
    gem_obj->prime_fd = -1;
    gem_obj->size = size;
    gem_obj->refcount = 1;
    return 0;
}
/* ── Implement: drm_gem_object_free ─────────────────────────────── */
int drm_gem_object_free(void *obj)
{
    if (!obj) return -EINVAL;

    /* Free a GEM object — delegate to drm_gem_free_object */
    struct drm_gem_object *gem_obj = (struct drm_gem_object *)obj;
    drm_gem_free_object(gem_obj);
    return 0;
}
/* ── Implement: drm_gem_handle_delete ─────────────────────────────── */
int drm_gem_handle_delete(void *file, uint32_t handle)
{
    (void)file;
    /* Delete a GEM handle — our handle table is global, so we can
     * use drm_gem_handle_close with a NULL device (which is tolerated). */
    return drm_gem_handle_close(NULL, handle);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Enhanced GEM API
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * drm_gem_object_lookup — Look up a GEM object by handle (refcounted).
 *
 * Standard DRM helper: retrieve the GEM object associated with the
 * given handle and increment its reference count.  The caller must
 * call drm_gem_unref() when done with the object.
 *
 * Returns the object pointer (refcount bumped) or NULL if not found.
 */
struct drm_gem_object *drm_gem_object_lookup(struct drm_device *dev,
                                              struct drm_file *file_priv,
                                              uint32_t handle)
{
    (void)dev;
    (void)file_priv;

    struct drm_gem_object *obj = drm_gem_handle_lookup(dev, handle);
    if (obj)
        drm_gem_ref(obj);
    return obj;
}

/*
 * drm_gem_flink — Create a global (flink) name for a GEM object.
 *
 * Global names allow a GEM object to be shared across processes:
 * one process flinks the object to get a name, another process
 * opens the name to obtain its own handle.
 *
 * Returns 0 on success with @name filled.
 */
int drm_gem_flink(struct drm_device *dev,
                   struct drm_gem_object *obj,
                   uint32_t *name)
{
    (void)dev;
    int ret = 0;

    if (!obj || !name)
        return -EINVAL;

    spinlock_acquire(&g_gem_lock);

    /* If already has a name, return it */
    if (obj->name != 0) {
        *name = obj->name;
        goto out;
    }

    /* Find a free name slot */
    int idx = -1;
    for (int i = 0; i < GEM_MAX_NAMES; i++) {
        if (!g_gem_names[i].in_use) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        ret = -ENOMEM;
        goto out;
    }

    /* Allocate a global name */
    uint32_t n = g_next_name++;
    if (g_next_name == 0) g_next_name = 1;

    g_gem_names[idx].in_use = 1;
    g_gem_names[idx].name = n;
    g_gem_names[idx].obj = obj;
    obj->name = n;
    *name = n;

out:
    spinlock_release(&g_gem_lock);
    return ret;
}

/*
 * drm_gem_open — Open a GEM object by its global (flink) name.
 *
 * Creates a new userspace handle for the object identified by @name.
 * The caller receives the new @handle and object @size.
 *
 * Returns 0 on success.
 */
int drm_gem_open(struct drm_device *dev,
                  struct drm_file *file_priv,
                  uint32_t name,
                  uint32_t *handle,
                  uint64_t *size)
{
    (void)dev;
    (void)file_priv;
    int ret = 0;

    if (name == 0 || !handle || !size)
        return -EINVAL;

    spinlock_acquire(&g_gem_lock);

    /* Look up the name entry */
    struct drm_gem_object *obj = NULL;
    for (int i = 0; i < GEM_MAX_NAMES; i++) {
        if (g_gem_names[i].in_use && g_gem_names[i].name == name) {
            obj = g_gem_names[i].obj;
            break;
        }
    }
    if (!obj) {
        ret = -ENOENT;
        goto out;
    }

    /* Create a handle for it in the handle table */
    uint32_t h = g_next_handle++;
    if (g_next_handle == 0) g_next_handle = 1;

    int idx = -1;
    for (int i = 0; i < GEM_MAX_HANDLES; i++) {
        if (!g_gem_handles[i].in_use) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        ret = -ENOMEM;
        goto out;
    }

    g_gem_handles[idx].in_use = 1;
    g_gem_handles[idx].handle = h;
    g_gem_handles[idx].obj = obj;

    /* Take a reference for the new handle */
    drm_gem_ref(obj);

    *handle = h;
    *size = obj->size;

out:
    spinlock_release(&g_gem_lock);
    return ret;
}

/*
 * drm_gem_get_pages — Get the physical page array for a GEM object.
 *
 * Allocates and fills a page-pointer array for the physical frames
 * backing this GEM object.  The caller must call drm_gem_put_pages()
 * to release the array.
 *
 * Returns 0 on success, negative errno on failure.
 */
int drm_gem_get_pages(struct drm_gem_object *obj,
                       uint64_t **pages_out,
                       uint32_t *num_pages_out)
{
    if (!obj || !pages_out || !num_pages_out)
        return -EINVAL;

    uint32_t np = obj->num_pages;
    if (np == 0)
        return -EINVAL;

    uint64_t *pages = (uint64_t *)kmalloc(np * sizeof(uint64_t));
    if (!pages)
        return -ENOMEM;

    if (obj->is_contig && obj->phys_addr != 0) {
        /* Physically contiguous: pages are sequential */
        for (uint32_t i = 0; i < np; i++)
            pages[i] = obj->phys_addr + i * 0x1000ULL;
    } else {
        /* SG-backed: for now we still allocate contiguous,
         * but the pages are the same.  In a full scatter-gather
         * implementation, each page would be individually mapped. */
        if (obj->phys_addr != 0) {
            for (uint32_t i = 0; i < np; i++)
                pages[i] = obj->phys_addr + i * 0x1000ULL;
        } else {
            kfree(pages);
            return -EINVAL;
        }
    }

    *pages_out = pages;
    *num_pages_out = np;
    return 0;
}

/*
 * drm_gem_put_pages — Release a page array obtained from drm_gem_get_pages().
 *
 * Returns 0 on success.
 */
int drm_gem_put_pages(struct drm_gem_object *obj)
{
    (void)obj;
    /* Pages are heap-allocated in drm_gem_get_pages and freed by the
     * caller after use.  This hook exists for future bookkeeping
     * (e.g. cache coherency, dirty tracking).  Currently a no-op
     * because the caller frees the array directly. */
    return 0;
}

/*
 * drm_gem_vm_open — Record an mmap mapping on a GEM object.
 *
 * Called when a new userspace mmap is established for this object.
 * Tracks the mapping count so the object is not freed while mapped.
 * The object must have been looked up (refcounted) beforehand.
 */
void drm_gem_vm_open(struct drm_gem_object *obj)
{
    if (!obj) return;
    spinlock_acquire(&g_gem_lock);
    obj->vm_count++;
    obj->flags |= DRM_GEM_OBJECT_VMAPPED;
    drm_gem_ref(obj);
    spinlock_release(&g_gem_lock);
}

/*
 * drm_gem_vm_close — Release an mmap mapping on a GEM object.
 *
 * Called when a userspace mmap is unmapped.  Drops the reference
 * that was taken in drm_gem_vm_open().
 */
void drm_gem_vm_close(struct drm_gem_object *obj)
{
    if (!obj) return;
    spinlock_acquire(&g_gem_lock);
    if (obj->vm_count > 0)
        obj->vm_count--;
    if (obj->vm_count == 0)
        obj->flags &= ~DRM_GEM_OBJECT_VMAPPED;
    spinlock_release(&g_gem_lock);
    drm_gem_unref(obj);
}

/* ═══════════════════════════════════════════════════════════════════
 *  GEM ioctl handlers
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * drm_gem_close_ioctl — IOCTL handler for DRM_IOCTL_GEM_CLOSE.
 *
 * Closes a GEM handle, dropping the associated reference.
 * Equivalent to drm_gem_handle_close().
 */
int drm_gem_close_ioctl(struct drm_device *dev,
                         struct drm_file *file_priv,
                         uint32_t handle)
{
    (void)file_priv;
    return drm_gem_handle_close(dev, handle);
}

/*
 * drm_gem_flink_ioctl — IOCTL handler for DRM_IOCTL_GEM_FLINK.
 *
 * Creates a global (flink) name for the GEM object identified by
 * args->handle and returns the name in args->name.
 */
int drm_gem_flink_ioctl(struct drm_device *dev,
                         struct drm_file *file_priv,
                         struct drm_gem_flink *args)
{
    (void)file_priv;
    struct drm_gem_object *obj;

    if (!args)
        return -EINVAL;

    obj = drm_gem_handle_lookup(dev, args->handle);
    if (!obj)
        return -ENOENT;

    return drm_gem_flink(dev, obj, &args->name);
}

/*
 * drm_gem_open_ioctl — IOCTL handler for DRM_IOCTL_GEM_OPEN.
 *
 * Opens a GEM object by its global (flink) name and returns
 * a new handle and the object size.
 */
int drm_gem_open_ioctl(struct drm_device *dev,
                        struct drm_file *file_priv,
                        struct drm_gem_open *args)
{
    if (!args)
        return -EINVAL;

    return drm_gem_open(dev, file_priv,
                        args->name, &args->handle, &args->size);
}
