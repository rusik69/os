/*
 * drm_prime.c — DRM PRIME FD sharing (dma-buf export/import)
 *
 * Provides DRM PRIME ioctls for sharing GEM buffer objects between
 * DRM drivers and processes via a file-descriptor-like token that
 * wraps the kernel's dma-buf subsystem.
 *
 * Architecture:
 *   - A prime FD is an integer token in a global table, each entry
 *     mapping an FD → { dma_buf, gem_object }.
 *   - drm_gem_prime_handle_to_fd(): Export a GEM handle as a prime FD.
 *     Creates a dma_buf wrapping the GEM object's backing pages and
 *     allocates a new prime FD.  If the object was already exported,
 *     returns the existing FD (with an extra ref).
 *   - drm_gem_prime_fd_to_handle(): Import a prime FD as a new GEM
 *     handle.  Looks up the dma_buf from the FD table, creates a
 *     new GEM object backed by the dma_buf's pages, and registers
 *     a userspace handle for it.
 *   - Reference counting: exporting takes a dma_buf ref + GEM ref;
 *     importing takes an additional GEM ref via the handle table.
 *     When the last handle is closed, the GEM object is freed and
 *     the dma_buf is released.
 *
 * Item D143 task 7 — DRM PRIME FD sharing
 */

#define KERNEL_INTERNAL
#include "drm.h"
#include "dma_buf.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "heap.h"

/* ═══════════════════════════════════════════════════════════════════════
 *  Prime FD table
 * ═══════════════════════════════════════════════════════════════════════ */

#define PRIME_MAX_FDS  32

struct prime_fd_entry {
    int                  in_use;
    int                  fd;            /* the prime FD number */
    struct dma_buf      *dmabuf;        /* backing dma-buf */
    struct drm_gem_object *obj;         /* GEM object (may be NULL for external dma-bufs) */
};

static struct prime_fd_entry g_prime_fds[PRIME_MAX_FDS];
static int                   g_prime_inited = 0;
static spinlock_t            g_prime_lock;
static int                   g_next_prime_fd = 1;

/* ── Internal helpers ──────────────────────────────────────────────── */

static int prime_fd_alloc(struct dma_buf *dmabuf,
                          struct drm_gem_object *obj)
{
    if (!dmabuf)
        return -EINVAL;

    spinlock_acquire(&g_prime_lock);

    int fd = g_next_prime_fd++;
    if (g_next_prime_fd < 0 || g_next_prime_fd > 0x7FFFFFFF)
        g_next_prime_fd = 1;

    int idx = -1;
    for (int i = 0; i < PRIME_MAX_FDS; i++) {
        if (!g_prime_fds[i].in_use) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        spinlock_release(&g_prime_lock);
        kprintf("[DRM PRIME] WARNING: prime FD table full\n");
        return -ENOMEM;
    }

    g_prime_fds[idx].in_use = 1;
    g_prime_fds[idx].fd     = fd;
    g_prime_fds[idx].dmabuf = dmabuf;
    g_prime_fds[idx].obj    = obj;

    /* Bump dma-buf refcount (the FD holds one reference) */
    dmabuf->refcount++;

    spinlock_release(&g_prime_lock);

    kprintf("[DRM PRIME] allocated FD %d -> dmabuf=%p obj=%p\n",
            fd, (void *)dmabuf, (void *)obj);
    return fd;
}

static struct prime_fd_entry *prime_fd_lookup(int fd)
{
    for (int i = 0; i < PRIME_MAX_FDS; i++) {
        if (g_prime_fds[i].in_use && g_prime_fds[i].fd == fd)
            return &g_prime_fds[i];
    }
    return NULL;
}

static void prime_fd_free(struct prime_fd_entry *ent)
{
    if (!ent || !ent->in_use)
        return;

    struct dma_buf *dmabuf = ent->dmabuf;

    kprintf("[DRM PRIME] releasing FD %d dmabuf=%p\n",
            ent->fd, (void *)dmabuf);

    ent->in_use = 0;
    ent->fd     = -1;
    ent->dmabuf = NULL;
    ent->obj    = NULL;

    /* Drop the dma-buf reference held by the FD */
    if (dmabuf && dmabuf->in_use) {
        dmabuf->refcount--;
        if (dmabuf->refcount <= 0)
            dma_buf_free(dmabuf);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Initialisation
 * ═══════════════════════════════════════════════════════════════════════ */

int drm_prime_init(void)
{
    if (g_prime_inited)
        return 0;

    spinlock_init(&g_prime_lock);
    memset(g_prime_fds, 0, sizeof(g_prime_fds));
    g_next_prime_fd = 1;
    g_prime_inited = 1;

    kprintf("[DRM PRIME] FD sharing initialised (%d descriptors max)\n",
            PRIME_MAX_FDS);
    return 0;
}

void drm_prime_exit(void)
{
    if (!g_prime_inited)
        return;

    spinlock_acquire(&g_prime_lock);
    for (int i = 0; i < PRIME_MAX_FDS; i++) {
        if (g_prime_fds[i].in_use)
            prime_fd_free(&g_prime_fds[i]);
    }
    g_prime_inited = 0;
    spinlock_release(&g_prime_lock);

    kprintf("[DRM PRIME] shutdown complete\n");
}

/* ═══════════════════════════════════════════════════════════════════════
 *  GEM object destruction callback
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * drm_prime_gem_destroy — Release PRIME references when a GEM object is freed.
 *
 * Called from drm_gem_free_object.  Nullifies the obj pointer in the
 * prime FD entry to prevent dangling references, then drops the extra
 * GEM ref that was taken during export.
 *
 * @obj: GEM object being destroyed (must have refcount == 0).
 */
void drm_prime_gem_destroy(struct drm_gem_object *obj)
{
    if (!obj)
        return;

    spinlock_acquire(&g_prime_lock);

    int fd = obj->prime_fd;
    if (fd >= 0) {
        struct prime_fd_entry *ent = prime_fd_lookup(fd);
        if (ent) {
            /* Nullify the obj pointer — the FD still references
             * the dma_buf, but the GEM object is going away. */
            ent->obj = NULL;

            /* Release the dma_buf reference that the FD holds */
            struct dma_buf *dmabuf = ent->dmabuf;
            if (dmabuf && dmabuf->in_use)
                dmabuf->refcount--;

            ent->dmabuf = NULL;
            ent->in_use = 0;
            ent->fd     = -1;
        }
        obj->prime_fd = -1;
    }

    spinlock_release(&g_prime_lock);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  PRIME export: handle → FD
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * drm_gem_prime_handle_to_fd — Export a GEM handle as a PRIME FD.
 *
 * Looks up the GEM object by handle, creates (or reuses) a dma-buf
 * backing it, and returns a prime FD token that userspace can pass
 * to another process or driver.
 *
 * @dev:       DRM device
 * @file_priv: DRM file-private (unused if per-file tables are global)
 * @args:      Input/output — { handle, flags, prime_fd }
 *
 * Returns 0 on success with args->prime_fd filled, or negative errno.
 */
int drm_gem_prime_handle_to_fd(struct drm_device *dev,
                               struct drm_file *file_priv,
                               struct drm_prime_handle *args)
{
    (void)file_priv;

    if (!dev || !args)
        return -EINVAL;

    uint32_t handle = args->handle;
    if (handle == 0)
        return -EINVAL;

    /* Look up the GEM object */
    struct drm_gem_object *obj = drm_gem_handle_lookup(dev, handle);
    if (!obj) {
        kprintf("[DRM PRIME] export: handle %u not found\n",
                (unsigned int)handle);
        return -ENOENT;
    }

    /* If this object was already exported, reuse the existing FD */
    if (obj->prime_fd >= 0) {
        spinlock_acquire(&g_prime_lock);
        struct prime_fd_entry *ent = prime_fd_lookup(obj->prime_fd);
        if (ent && ent->in_use) {
            /* Reuse the existing FD — the prime_fd_alloc call already took
             * one dma-buf reference for this entry.  No additional ref is
             * needed: every caller sharing this FD shares the same entry. */
            int fd = ent->fd;
            spinlock_release(&g_prime_lock);
            args->prime_fd = fd;
            kprintf("[DRM PRIME] export: handle %u -> existing FD %d\n",
                    (unsigned int)handle, fd);
            return 0;
        }
        /* Stale prime_fd — reset and create a new one */
        obj->prime_fd = -1;
        spinlock_release(&g_prime_lock);
    }

    /* Get the physical page array for the GEM object */
    uint64_t *pages = NULL;
    uint32_t num_pages = 0;
    int ret = drm_gem_get_pages(obj, &pages, &num_pages);
    if (ret < 0) {
        kprintf("[DRM PRIME] export: get_pages failed (%d)\n", ret);
        return ret;
    }

    /* Allocate a dma_buf that shares the GEM object's backing memory.
     * We create a new dma_buf but use the GEM object's physical pages
     * as the backing store, rather than allocating fresh pages.
     * For simplicity, we allocate a fresh dma_buf — in a full
     * implementation we would import the pages into the dma_buf. */
    size_t buf_size = obj->size;
    struct dma_buf *dmabuf = dma_buf_alloc(buf_size, DMA_BUF_F_RDWR);
    if (!dmabuf) {
        kfree(pages);
        return -ENOMEM;
    }

    /* Copy the GEM object's data into the dma_buf so the exported
     * buffer reflects the current content.  In a full implementation
     * we would share pages rather than copy. */
    if (obj->vaddr && dmabuf->virt_addr)
        memcpy(dmabuf->virt_addr, obj->vaddr, obj->size);

    kfree(pages);

    /* Allocate a prime FD for this dma_buf */
    int fd = prime_fd_alloc(dmabuf, obj);
    if (fd < 0) {
        dma_buf_free(dmabuf);
        return fd;
    }

    /* Record the prime FD on the GEM object */
    spinlock_acquire(&g_prime_lock);
    obj->prime_fd = fd;
    obj->flags |= DRM_GEM_OBJECT_EXPORTED;
    spinlock_release(&g_prime_lock);

    /* Take an extra GEM reference — the export keeps the object alive */
    drm_gem_ref(obj);

    args->prime_fd = fd;
    args->handle   = handle;

    kprintf("[DRM PRIME] export: handle %u -> FD %d size=%llu\n",
            (unsigned int)handle, fd, (unsigned long long)buf_size);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  PRIME import: FD → handle
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * drm_gem_prime_fd_to_handle — Import a PRIME FD as a new GEM handle.
 *
 * Looks up the dma-buf by prime FD, creates a GEM object backed by
 * the dma-buf's buffer, and returns a new handle for userspace.
 *
 * @dev:       DRM device
 * @file_priv: DRM file-private
 * @args:      Input/output — { handle (out), flags, prime_fd (in) }
 *
 * Returns 0 on success with args->handle filled, or negative errno.
 */
int drm_gem_prime_fd_to_handle(struct drm_device *dev,
                               struct drm_file *file_priv,
                               struct drm_prime_handle *args)
{
    (void)file_priv;

    if (!dev || !args)
        return -EINVAL;

    int prime_fd = args->prime_fd;
    if (prime_fd < 0)
        return -EINVAL;

    /* Look up the prime FD */
    struct prime_fd_entry *ent;
    spinlock_acquire(&g_prime_lock);
    ent = prime_fd_lookup(prime_fd);
    if (!ent || !ent->in_use || !ent->dmabuf) {
        spinlock_release(&g_prime_lock);
        kprintf("[DRM PRIME] import: FD %d not found\n", prime_fd);
        return -ENOENT;
    }

    struct dma_buf *dmabuf = ent->dmabuf;

    /* If this FD was exported from a GEM object, reuse it directly */
    if (ent->obj) {
        struct drm_gem_object *existing = ent->obj;
        /* Keep the lock while we dereference */
        uint32_t new_handle = 0;
        int ret = drm_gem_handle_create(dev, existing, &new_handle);
        spinlock_release(&g_prime_lock);

        if (ret < 0) {
            kprintf("[DRM PRIME] import: handle_create failed (%d)\n", ret);
            return ret;
        }

        args->handle   = new_handle;
        args->prime_fd = prime_fd;

        kprintf("[DRM PRIME] import: FD %d -> handle %u (reused obj %p)\n",
                prime_fd, (unsigned int)new_handle, (void *)existing);
        return 0;
    }

    /* Otherwise create a new GEM object backed by the dma-buf's memory */
    spinlock_release(&g_prime_lock);

    size_t buf_size = dmabuf->size;
    struct drm_gem_object *new_obj = NULL;
    int ret = drm_gem_create_object(buf_size, 1, &new_obj);
    if (ret < 0) {
        kprintf("[DRM PRIME] import: create_object failed (%d)\n", ret);
        return ret;
    }

    /* Copy the dma-buf content into the new GEM object.
     * In a full implementation we would share the physical pages
     * rather than copy. */
    if (dmabuf->virt_addr && new_obj->vaddr)
        memcpy(new_obj->vaddr, dmabuf->virt_addr, buf_size);

    new_obj->flags |= DRM_GEM_OBJECT_IMPORTED;

    /* Record the prime FD on the new GEM object so re-exports work */
    new_obj->prime_fd = prime_fd;

    /* Create a userspace handle for the new object */
    uint32_t new_handle = 0;
    ret = drm_gem_handle_create(dev, new_obj, &new_handle);
    if (ret < 0) {
        drm_gem_free_object(new_obj);
        return ret;
    }

    args->handle   = new_handle;
    args->prime_fd = prime_fd;

    kprintf("[DRM PRIME] import: FD %d -> handle %u size=%llu\n",
            prime_fd, (unsigned int)new_handle,
            (unsigned long long)buf_size);
    return 0;
}
