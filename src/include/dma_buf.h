#ifndef DMA_BUF_H
#define DMA_BUF_H

#include "types.h"
#include "list.h"
#include "spinlock.h"

/* ── DMA-buf constants ──────────────────────────────────────────────── */

#define DMA_BUF_MAX_BUFS      16   /* maximum number of shared buffers */
#define DMA_BUF_MAX_ATTACH    8    /* maximum attachments per buffer */

/* Flags for dma_buf_alloc */
#define DMA_BUF_F_READ        0x01 /* buffer readable by attached devices */
#define DMA_BUF_F_WRITE       0x02 /* buffer writable by attached devices */
#define DMA_BUF_F_RDWR        (DMA_BUF_F_READ | DMA_BUF_F_WRITE)

/* DMA data direction (mirrors dma.h) */
enum dma_buf_direction {
    DMA_BUF_BIDIRECTIONAL = 0,
    DMA_BUF_TO_DEVICE     = 1,
    DMA_BUF_FROM_DEVICE   = 2,
    DMA_BUF_NONE          = 3,
};

/* Forward declarations */
struct dma_buf;
struct dma_buf_attachment;

/* ── DMA-buf structure ────────────────────────────────────────────────
 *
 * A dma_buf represents a shared memory buffer allocated from contiguous
 * physical pages.  It maintains a reference count and a list of
 * device attachments.
 */

struct dma_buf {
    uint64_t phys_addr;       /* physical address of the buffer */
    void    *virt_addr;       /* kernel virtual address */
    size_t   size;            /* buffer size in bytes */
    uint32_t flags;           /* DMA_BUF_F_* flags */
    int      refcount;        /* reference count (free when 0) */
    int      in_use;          /* slot allocated */

    /* Number of contiguous frames allocated */
    int      num_frames;

    /* Attachments list */
    struct dma_buf_attachment *attachments[DMA_BUF_MAX_ATTACH];
    int      attach_count;

    /* Lock */
    spinlock_t lock;
} __cacheline_aligned;

/* ── Attachment structure ─────────────────────────────────────────────
 *
 * Represents a device attached to a dma_buf.  Each attachment can be
 * independently mapped for DMA access.
 */

struct dma_buf_attachment {
    struct dma_buf *dmabuf;
    void           *device;         /* opaque device pointer */
    uint64_t        dma_addr;       /* mapped DMA address */
    enum dma_buf_direction direction;
    int             mapped;
    int             in_use;
};

/* ── Public API ─────────────────────────────────────────────────────── */

/* Initialise the DMA-buf subsystem.  Called once during boot. */
void dma_buf_subsys_init(void);

/* Allocate a shared DMA buffer.
 * @size:  Size in bytes (will be rounded up to page boundary)
 * @flags: DMA_BUF_F_* flags
 * Returns a pointer to the dma_buf, or NULL on failure.
 */
struct dma_buf *dma_buf_alloc(size_t size, uint32_t flags);

/* Attach a device to a dma_buf.
 * @dmabuf:  The shared buffer
 * @device:  Opaque device pointer (e.g., struct pci_device *)
 * Returns a pointer to the attachment, or NULL on failure.
 */
struct dma_buf_attachment *dma_buf_attach(struct dma_buf *dmabuf,
                                           void *device);

/* Map an attachment for DMA access.
 * @attach:    The attachment
 * @direction: DMA data direction
 * Returns the DMA address usable by the device, or 0 on failure.
 */
uint64_t dma_buf_map_attachment(struct dma_buf_attachment *attach,
                                 enum dma_buf_direction direction);

/* Unmap an attachment.
 * @attach: The attachment to unmap
 */
void dma_buf_unmap_attachment(struct dma_buf_attachment *attach);

/* Detach a device from a dma_buf.
 * @dmabuf: The shared buffer
 * @attach: The attachment to remove
 */
void dma_buf_detach(struct dma_buf *dmabuf,
                     struct dma_buf_attachment *attach);

/* Free a dma_buf and all its attachments.
 * The buffer is only freed when its reference count reaches 0.
 * @dmabuf: The shared buffer to release (may be deferred if referenced)
 */
void dma_buf_free(struct dma_buf *dmabuf);

/* Get the kernel virtual address of a dma_buf. */
void *dma_buf_virt(struct dma_buf *dmabuf);

/* Get the physical address of a dma_buf. */
uint64_t dma_buf_phys(struct dma_buf *dmabuf);

#endif /* DMA_BUF_H */
