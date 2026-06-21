/*
 * xhci_streams.c — xHCI stream support for bulk endpoints
 *
 * B11: Implement stream context arrays for USB 3.0 bulk streams.
 * Allows multiple outstanding transfers per endpoint (streams).
 */

#include "xhci.h"
#include "heap.h"
#include "printf.h"
#include "string.h"
#include "errno.h"

/* Stream context types (spec 4.12.1) */
#define STREAM_CTX_TYPE_SLOT    0x01  /* Slot context */
#define STREAM_CTX_TYPE_ENDPOINT 0x02  /* Endpoint context */

/* Stream context structure (spec 6.16.1) */
struct xhci_stream_ctx {
    uint64_t ptr_low;
    uint32_t ptr_high;
    uint32_t dcs;
} __attribute__((packed));

/* Stream ID array entry */
struct xhci_stream_id_entry {
    uint16_t stream_id;
    uint16_t ep_index;
};

#define MAX_STREAMS_PER_EP      16
#define MAX_STREAM_IDS          256

/* Per-endpoint stream state */
struct xhci_ep_streams {
    int ep_index;
    int num_streams;
    struct xhci_stream_ctx *stream_ctx_array;
    uint64_t stream_ctx_paddr;
};

/* Controller-level stream tracking */
static struct xhci_ep_streams g_ep_streams[32];  /* Max 31 endpoints + 1 */
static int g_num_stream_eps = 0;

/*
 * xhci_streams_init — initialize stream subsystem
 */
int xhci_streams_init(void)
{
    memset(g_ep_streams, 0, sizeof(g_ep_streams));
    g_num_stream_eps = 0;
    return 0;
}

/*
 * xhci_streams_capable — check controller supports streams
 * Returns: bit 0 = primary stream support, bit 1 = secondary
 */
int xhci_streams_capable(struct xhci_controller *xhci)
{
    if (!xhci) return 0;
    uint32_t hccparams = xhci_read32(xhci, xhci->cap_regs, 0x10);  /* HCCPARAMS */
    int nss = (hccparams >> 12) & 0x1f;  /* Number of Stream Contexts */
    return (nss > 0) ? 1 : 0;
}

/*
 * xhci_ep_alloc_streams — allocate stream contexts for endpoint
 *
 * @ep_index: endpoint context index (1-31)
 * @num_streams: number of streams to allocate
 * Returns: stream ID base, or <0 on error
 */
int xhci_ep_alloc_streams(int ep_index, int num_streams)
{
    if (ep_index < 1 || ep_index > 31) return -EINVAL;
    if (num_streams < 2 || num_streams > MAX_STREAMS_PER_EP) return -EINVAL;
    if (g_num_stream_eps >= 32) return -ENOSPC;

    /* Allocate stream context array */
    size_t ctx_size = (size_t)num_streams * sizeof(struct xhci_stream_ctx);
    struct xhci_stream_ctx *ctx = (struct xhci_stream_ctx *)kmalloc(ctx_size);
    if (!ctx) return -ENOMEM;

    memset(ctx, 0, ctx_size);

    /* Initialize DCS (Data Stream Context) bit for each stream */
    for (int i = 0; i < num_streams; i++) {
        ctx[i].dcs = 1;  /* Initial DCS = 1 per spec */
    }

    /* Register */
    struct xhci_ep_streams *ep = &g_ep_streams[g_num_stream_eps];
    ep->ep_index = ep_index;
    ep->num_streams = num_streams;
    ep->stream_ctx_array = ctx;
    ep->stream_ctx_paddr = (uint64_t)ctx;  /* kmalloc returns physical in our kernel */

    g_num_stream_eps++;

    return 2;  /* Primary stream ID base (per USB 3.0 spec) */
}

/*
 * xhci_ep_free_streams — free streams for endpoint
 */
int xhci_ep_free_streams(int ep_index)
{
    for (int i = 0; i < g_num_stream_eps; i++) {
        if (g_ep_streams[i].ep_index == ep_index) {
            if (g_ep_streams[i].stream_ctx_array) {
                kfree(g_ep_streams[i].stream_ctx_array);
            }
            memset(&g_ep_streams[i], 0, sizeof(g_ep_streams[i]));
            /* Compact array */
            for (int j = i + 1; j < g_num_stream_eps; j++) {
                g_ep_streams[j - 1] = g_ep_streams[j];
            }
            g_num_stream_eps--;
            return 0;
        }
    }
    return -ENOENT;
}

/* ── Stub: xhci_streams_alloc ─────────────────────────────── */
int xhci_streams_alloc(void *dev, int count)
{
    (void)dev;
    (void)count;
    kprintf("[xhci] xhci_streams_alloc: not yet implemented\n");
    return 0;
}
/* ── Stub: xhci_streams_free ─────────────────────────────── */
int xhci_streams_free(void *dev)
{
    (void)dev;
    kprintf("[xhci] xhci_streams_free: not yet implemented\n");
    return 0;
}
