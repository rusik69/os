/*
 * usb_transfer.c — USB control transfer lifecycle management
 *
 * Provides the core synchronous control transfer API that USB class
 * drivers use to communicate with devices.  Delegates the actual
 * transaction-level work to the registered host controller (EHCI, xHCI).
 *
 * Control transfer lifecycle (USB 2.0 spec §9.3):
 *   1. SETUP stage  — 8-byte setup packet sent to EP0
 *   2. DATA stage   — optional, data flows per bmRequestType direction
 *   3. STATUS stage — zero-length handshake, direction opposite of DATA
 *
 * Item S37 — USB control transfer (setup packet lifecycle)
 */

#include "usb.h"
#include "usb_core.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "pmm.h"
#include "errno.h"

/* ── Registered host controller ops ───────────────────────────────── */

static const struct usb_hc_ops *g_hc_ops = NULL;
static spinlock_t g_hc_lock;

/* ── HC ops registration ──────────────────────────────────────────── */

int usb_register_hc_ops(const struct usb_hc_ops *ops)
{
    if (!ops || !ops->control_transfer)
        return -EINVAL;

    spinlock_acquire(&g_hc_lock);
    if (g_hc_ops) {
        spinlock_release(&g_hc_lock);
        kprintf("[USB] HC ops already registered (overriding)\\n");
    }
    g_hc_ops = ops;
    spinlock_release(&g_hc_lock);

    kprintf("[USB] Host controller ops registered\\n");
    return 0;
}

void usb_deregister_hc_ops(void)
{
    spinlock_acquire(&g_hc_lock);
    g_hc_ops = NULL;
    spinlock_release(&g_hc_lock);
    kprintf("[USB] Host controller ops deregistered\\n");
}

/* ── DMA-safe buffer allocation helper ────────────────────────────── */

/*
 * Allocate a DMA-safe buffer (one physical page, zeroed).
 * Returns virtual address or NULL on failure.
 * Free with usb_free_dma_buf().
 */
void *usb_alloc_dma_buf(void)
{
    uint64_t phys = pmm_alloc_frame();
    if (!phys)
        return NULL;
    void *virt = PHYS_TO_VIRT(phys);
    memset(virt, 0, 4096);
    return virt;
}

/*
 * Free a DMA-safe buffer allocated with usb_alloc_dma_buf().
 */
void usb_free_dma_buf(void *virt)
{
    if (!virt)
        return;
    pmm_free_frame(VIRT_TO_PHYS(virt));
}

/* ── Core control transfer API ────────────────────────────────────── */

/*
 * Build an 8-byte setup packet from its constituent fields.
 */
static void build_setup_packet(struct usb_setup_packet *pkt,
                               uint8_t bmReqType, uint8_t bRequest,
                               uint16_t wValue, uint16_t wIndex,
                               uint16_t wLength)
{
    pkt->bmRequestType = bmReqType;
    pkt->bRequest      = bRequest;
    pkt->wValue        = wValue;
    pkt->wIndex        = wIndex;
    pkt->wLength       = wLength;
}

int usb_control_msg(uint8_t dev_addr, uint8_t bmReqType,
                    uint8_t bRequest, uint16_t wValue,
                    uint16_t wIndex, uint16_t wLength, void *data)
{
    struct usb_setup_packet setup;
    const struct usb_hc_ops *ops;
    int ret;

    spinlock_acquire(&g_hc_lock);
    ops = g_hc_ops;
    spinlock_release(&g_hc_lock);

    if (!ops) {
        kprintf("[USB] control_msg: no host controller registered\\n");
        return -ENODEV;
    }

    if (wLength > 0 && !data) {
        kprintf("[USB] control_msg: data buffer required for wLength=%u\\n",
                (unsigned)wLength);
        return -EINVAL;
    }

    build_setup_packet(&setup, bmReqType, bRequest,
                       wValue, wIndex, wLength);

    kprintf("[USB] control_msg: addr=%d req=0x%02x type=0x%02x "
            "val=%u idx=%u len=%u\\n",
            dev_addr, bRequest, bmReqType,
            (unsigned)wValue, (unsigned)wIndex, (unsigned)wLength);

    ret = ops->control_transfer(dev_addr, &setup, data, wLength);
    if (ret < 0) {
        kprintf("[USB] control_msg: failed (%d)\\n", ret);
        return ret;
    }

    /* Return number of bytes transferred */
    return (int)wLength;
}
