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
#include "printf.h"
#include "spinlock.h"
#include "errno.h"

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

/* ═══════════════════════════════════════════════════════════════════
 *  Initialisation
 * ═══════════════════════════════════════════════════════════════════ */

int drm_gem_init(void)
{
    spinlock_init(&g_gem_lock);
    memset(g_gem_handles, 0, sizeof(g_gem_handles));
    memset(g_gem_objects, 0, sizeof(g_gem_objects));
    g_gem_initialized = 1;
    g_next_handle = 1;
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
    obj->size = size;
    obj->is_contig = is_contig;
    obj->refcount = 1;

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
        if (!obj->vaddr) {
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
        if (!obj->vaddr) {
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
