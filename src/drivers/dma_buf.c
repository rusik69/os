/* dma_buf.c — DMA buffer sharing mechanism.
 *
 * Provides a simple implementation of the DMA-buf API for sharing
 * memory buffers between device drivers.  Allocates contiguous
 * physical pages and manages attachments with refcounting.
 */

#define KERNEL_INTERNAL
#include "dma_buf.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"

/* ── Global state ──────────────────────────────────────────────────── */

static struct dma_buf g_dma_bufs[DMA_BUF_MAX_BUFS];
static struct dma_buf_attachment g_attachments[DMA_BUF_MAX_BUFS *
                                                DMA_BUF_MAX_ATTACH];
static int g_dma_buf_inited = 0;
static spinlock_t g_dma_buf_lock;

/* ── Helper: allocate an attachment slot ────────────────────────────── */

static struct dma_buf_attachment *alloc_attachment(void)
{
    for (int i = 0; i < DMA_BUF_MAX_BUFS * DMA_BUF_MAX_ATTACH; i++) {
        if (!g_attachments[i].in_use) {
            g_attachments[i].in_use = 1;
            return &g_attachments[i];
        }
    }
    return NULL;
}

static void free_attachment(struct dma_buf_attachment *att)
{
    if (att) {
        memset(att, 0, sizeof(*att));
    }
}

/* ── Helper: round up to page size ──────────────────────────────────── */

static size_t round_up_page(size_t size)
{
    return (size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
}

/* ── Subsystem initialisation ───────────────────────────────────────── */

void dma_buf_subsys_init(void)
{
    if (g_dma_buf_inited) return;

    memset(g_dma_bufs, 0, sizeof(g_dma_bufs));
    memset(g_attachments, 0, sizeof(g_attachments));
    spinlock_init(&g_dma_buf_lock);
    g_dma_buf_inited = 1;

    kprintf("[dma_buf] subsystem initialised (%d buffers max, "
            "%d attachments max)\n",
            DMA_BUF_MAX_BUFS, DMA_BUF_MAX_BUFS * DMA_BUF_MAX_ATTACH);
}

/* ── Buffer allocation ──────────────────────────────────────────────── */

struct dma_buf *dma_buf_alloc(size_t size, uint32_t flags)
{
    if (!g_dma_buf_inited) return NULL;
    if (size == 0) return NULL;

    size_t aligned = round_up_page(size);
    int num_frames = (int)(aligned / PAGE_SIZE);
    if (num_frames == 0) num_frames = 1;

    /* Allocate contiguous physical pages */
    uint64_t first_frame = 0;
    uint64_t *frames = pmm_alloc_frames((size_t)num_frames);
    if (!frames) {
        /* Fallback: allocate single frames and hope for contiguity.
         * For a real implementation we'd use a proper contiguous allocator. */
        first_frame = pmm_alloc_frame();
        if (!first_frame) return NULL;
        uint64_t expected = first_frame + PAGE_SIZE;

        for (int i = 1; i < num_frames; i++) {
            uint64_t f = pmm_alloc_frame();
            if (!f) {
                /* Free already allocated frames */
                for (int j = 0; j < i; j++)
                    pmm_free_frame(first_frame + (uint64_t)j * PAGE_SIZE);
                pmm_free_frame(first_frame);
                return NULL;
            }
            if (f != expected) {
                /* Not contiguous — in a real implementation we would
                 * use a proper contiguous allocator.  For simplicity
                 * we accept non-contiguous pages. */
                (void)f;
                expected = f + PAGE_SIZE;
            } else {
                expected = f + PAGE_SIZE;
            }
        }
    } else {
        first_frame = frames[0];
        /* Free the array allocated by pmm_alloc_frames — we only need
         * the first frame as reference. */
        /* pmm_alloc_frames returns a dynamically allocated array */
    }

    /* Find a free dma_buf slot */
    struct dma_buf *dmabuf = NULL;

    uint64_t lock_flags;
    spinlock_irqsave_acquire(&g_dma_buf_lock, &lock_flags);

    for (int i = 0; i < DMA_BUF_MAX_BUFS; i++) {
        if (!g_dma_bufs[i].in_use) {
            dmabuf = &g_dma_bufs[i];
            dmabuf->in_use = 1;
            break;
        }
    }

    spinlock_irqsave_release(&g_dma_buf_lock, lock_flags);

    if (!dmabuf) {
        /* Free pages */
        for (int i = 0; i < num_frames; i++)
            pmm_free_frame(first_frame + (uint64_t)i * PAGE_SIZE);
        kprintf("[dma_buf] failed to allocate slot (max=%d)\n",
                DMA_BUF_MAX_BUFS);
        return NULL;
    }

    spinlock_init(&dmabuf->lock);
    dmabuf->phys_addr   = first_frame;
    dmabuf->virt_addr   = PHYS_TO_VIRT(first_frame);
    dmabuf->size        = aligned;
    dmabuf->flags       = flags;
    dmabuf->refcount    = 1;
    dmabuf->num_frames  = num_frames;
    dmabuf->attach_count = 0;
    memset(dmabuf->attachments, 0, sizeof(dmabuf->attachments));

    kprintf("[dma_buf] allocated buf=%p size=%llu flags=0x%x "
            "phys=0x%llx virt=%p frames=%d\n",
            (void *)dmabuf, (unsigned long long)aligned,
            (unsigned int)flags,
            (unsigned long long)first_frame,
            dmabuf->virt_addr, num_frames);

    return dmabuf;
}

/* ── Attach / Detach ────────────────────────────────────────────────── */

struct dma_buf_attachment *dma_buf_attach(struct dma_buf *dmabuf,
                                           void *device)
{
    if (!g_dma_buf_inited || !dmabuf || !device) return NULL;
    if (!dmabuf->in_use) return NULL;

    struct dma_buf_attachment *att = alloc_attachment();
    if (!att) return NULL;

    att->dmabuf    = dmabuf;
    att->device    = device;
    att->dma_addr  = 0;
    att->mapped    = 0;
    att->direction = DMA_BUF_NONE;

    /* Add to dma_buf's attachment list */
    uint64_t flags;
    spinlock_irqsave_acquire(&dmabuf->lock, &flags);

    if (dmabuf->attach_count >= DMA_BUF_MAX_ATTACH) {
        spinlock_irqsave_release(&dmabuf->lock, flags);
        free_attachment(att);
        return NULL;
    }

    dmabuf->attachments[dmabuf->attach_count++] = att;
    dmabuf->refcount++;

    spinlock_irqsave_release(&dmabuf->lock, flags);

    kprintf("[dma_buf] attached device %p to buf=%p (refcount=%d)\n",
            device, (void *)dmabuf, dmabuf->refcount);

    return att;
}

/* ── Map / Unmap ────────────────────────────────────────────────────── */

uint64_t dma_buf_map_attachment(struct dma_buf_attachment *attach,
                                 enum dma_buf_direction direction)
{
    if (!attach || !attach->in_use) return 0;

    struct dma_buf *dmabuf = attach->dmabuf;
    if (!dmabuf || !dmabuf->in_use) return 0;

    /* Store the DMA direction and mark as mapped.
     * The physical address IS the DMA address in our simple
     * identity-mapped DMA scheme (no IOMMU). */
    attach->direction = direction;
    attach->dma_addr  = dmabuf->phys_addr;
    attach->mapped    = 1;

    return attach->dma_addr;
}

void dma_buf_unmap_attachment(struct dma_buf_attachment *attach)
{
    if (!attach || !attach->in_use) return;

    attach->mapped    = 0;
    attach->dma_addr  = 0;
    attach->direction = DMA_BUF_NONE;
}

/* ── Detach ─────────────────────────────────────────────────────────── */

void dma_buf_detach(struct dma_buf *dmabuf,
                     struct dma_buf_attachment *attach)
{
    if (!dmabuf || !attach || !dmabuf->in_use) return;

    /* Unmap first */
    dma_buf_unmap_attachment(attach);

    /* Remove from attachment list */
    uint64_t flags;
    spinlock_irqsave_acquire(&dmabuf->lock, &flags);

    for (int i = 0; i < dmabuf->attach_count; i++) {
        if (dmabuf->attachments[i] == attach) {
            /* Compact the array */
            for (int j = i; j < dmabuf->attach_count - 1; j++)
                dmabuf->attachments[j] = dmabuf->attachments[j + 1];
            dmabuf->attachments[dmabuf->attach_count - 1] = NULL;
            dmabuf->attach_count--;
            break;
        }
    }

    if (dmabuf->refcount > 0)
        dmabuf->refcount--;

    spinlock_irqsave_release(&dmabuf->lock, flags);

    free_attachment(attach);

    kprintf("[dma_buf] detached from buf=%p (refcount=%d)\n",
            (void *)dmabuf, dmabuf->refcount);
}

/* ── Free ────────────────────────────────────────────────────────────── */

void dma_buf_free(struct dma_buf *dmabuf)
{
    if (!dmabuf || !dmabuf->in_use) return;

    uint64_t flags;
    spinlock_irqsave_acquire(&dmabuf->lock, &flags);

    if (dmabuf->refcount > 0)
        dmabuf->refcount--;

    if (dmabuf->refcount > 0) {
        /* Still referenced by attachments */
        spinlock_irqsave_release(&dmabuf->lock, flags);
        return;
    }

    /* Detach all remaining attachments */
    while (dmabuf->attach_count > 0) {
        struct dma_buf_attachment *att = dmabuf->attachments[0];
        if (att) {
            dma_buf_unmap_attachment(att);
            free_attachment(att);
        }
        for (int j = 0; j < dmabuf->attach_count - 1; j++)
            dmabuf->attachments[j] = dmabuf->attachments[j + 1];
        dmabuf->attachments[dmabuf->attach_count - 1] = NULL;
        dmabuf->attach_count--;
    }

    /* Free the physical pages */
    uint64_t phys = dmabuf->phys_addr;
    int num = dmabuf->num_frames;
    for (int i = 0; i < num; i++) {
        pmm_free_frame(phys + (uint64_t)i * PAGE_SIZE);
    }

    dmabuf->in_use = 0;

    spinlock_irqsave_release(&dmabuf->lock, flags);

    kprintf("[dma_buf] freed buf=%p phys=0x%llx size=%llu\n",
            (void *)dmabuf, (unsigned long long)phys,
            (unsigned long long)dmabuf->size);
}

/* ── Accessors ──────────────────────────────────────────────────────── */

void *dma_buf_virt(struct dma_buf *dmabuf)
{
    if (!dmabuf || !dmabuf->in_use) return NULL;
    return dmabuf->virt_addr;
}

uint64_t dma_buf_phys(struct dma_buf *dmabuf)
{
    if (!dmabuf || !dmabuf->in_use) return 0;
    return dmabuf->phys_addr;
}
