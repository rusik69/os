/*
 * xhci.c — xHCI (USB 3.0) host controller driver
 *
 * PCI-based USB 3.0 eXtensible Host Controller Interface driver.
 * Probes via class code 0x0C, 0x03, 0x30.
 */

#include "xhci.h"
#include "pci.h"
#include "io.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"
#include "errno.h"

static struct xhci_controller g_xhci;
static int g_xhci_init_done = 0;

/* Probe for xHCI controller via PCI */
static int xhci_probe_pci(void) {
    struct pci_device pci;
    int ret = pci_find_class(XHCI_PCI_CLASS, XHCI_PCI_SUBCLASS, &pci);
    if (ret < 0)
        return -1;

    /* Must be prog_if 0x30 for xHCI */
    if ((pci.class_code != XHCI_PCI_CLASS) || (pci.subclass != XHCI_PCI_SUBCLASS))
        return -1;

    pci_enable_bus_master(&pci);

    /* BAR0 contains MMIO registers */
    uint32_t bar0 = pci.bar[0];
    if (bar0 & 1) {
        kprintf("[xHCI] BAR0 is I/O space, not supported\n");
        return -1;
    }

    uint64_t mmio_base = (bar0 & 0xFFFFFFF0);
    if (mmio_base == 0)
        return -1;

    uint64_t virt_base = (uint64_t)PHYS_TO_VIRT((void*)(uintptr_t)mmio_base);

    g_xhci.cap_regs = virt_base;
    g_xhci.irq = pci.irq;

    /* Read capability registers */
    uint8_t caplength = xhci_read8(&g_xhci, g_xhci.cap_regs, 0x00);
    uint32_t hcsparams1 = xhci_read32(&g_xhci, g_xhci.cap_regs, XHCI_CAP_HCSPARAMS1);
    uint32_t hcsparams2 = xhci_read32(&g_xhci, g_xhci.cap_regs, XHCI_CAP_HCSPARAMS2);
    uint32_t hccparams1 = xhci_read32(&g_xhci, g_xhci.cap_regs, XHCI_CAP_HCCPARAMS1);

    g_xhci.op_regs = virt_base + caplength;
    g_xhci.max_ports = (uint8_t)((hcsparams1 >> 24) & 0xFF);
    g_xhci.max_slots = (uint8_t)(hcsparams2 & 0x1F);
    g_xhci.db_off = (hccparams1 & 0xFFFF);
    g_xhci.rt_off = xhci_read32(&g_xhci, g_xhci.cap_regs, XHCI_CAP_RTSOFF);

    if (g_xhci.max_ports > XHCI_MAX_PORTS)
        g_xhci.max_ports = XHCI_MAX_PORTS;

    kprintf("[xHCI] Found: VID=0x%04X DID=0x%04X, Ports=%d, Slots=%d\n",
            pci.vendor_id, pci.device_id, g_xhci.max_ports, g_xhci.max_slots);

    g_xhci.present = 1;
    return 0;
}

/* Reset and start xHCI controller */
static int xhci_start_controller(void) {
    /* Read USBSTS */
    uint32_t sts = xhci_read32(&g_xhci, g_xhci.op_regs, XHCI_OP_USBSTS);

    /* Reset controller */
    xhci_write32(&g_xhci, g_xhci.op_regs, XHCI_OP_USBCMD, XHCI_CMD_HCRST);

    /* Wait for reset to complete */
    int timeout = 100000;
    while (timeout--) {
        sts = xhci_read32(&g_xhci, g_xhci.op_regs, XHCI_OP_USBSTS);
        if (xhci_read32(&g_xhci, g_xhci.op_regs, XHCI_OP_USBCMD) & XHCI_CMD_HCRST) {
            __asm__ volatile("pause");
        } else {
            break;
        }
    }

    if (timeout <= 0) {
        kprintf("[xHCI] Controller reset timeout\n");
        return -1;
    }

    /* Read page size */
    g_xhci.page_size = xhci_read32(&g_xhci, g_xhci.op_regs, XHCI_OP_PAGESIZE);

    /* Set max slots in CONFIG register */
    xhci_write32(&g_xhci, g_xhci.op_regs, XHCI_OP_CONFIG, g_xhci.max_slots);

    /* Start the controller */
    xhci_write32(&g_xhci, g_xhci.op_regs, XHCI_OP_USBCMD, XHCI_CMD_RUN | XHCI_CMD_INTE);

    /* Wait for HCHalted bit to clear */
    timeout = 100000;
    while (timeout--) {
        sts = xhci_read32(&g_xhci, g_xhci.op_regs, XHCI_OP_USBSTS);
        if (!(sts & XHCI_STS_HCH))
            break;
        __asm__ volatile("pause");
    }

    if (timeout <= 0) {
        kprintf("[xHCI] Controller start timeout\n");
        return -1;
    }

    kprintf("[xHCI] Controller started\n");
    return 0;
}

/* ── Ring Management ──────────────────────────────────────────────────── */

/**
 * xhci_ring_create — Allocate and initialise a single-segment TRB ring.
 * @ring: Ring structure to initialise (caller-allocated).
 *
 * Allocates one page (4096 bytes) for 256 TRBs (16 bytes each).
 * TRB 255 is set up as a Link TRB pointing back to TRB 0 (circular
 * single-segment ring), with TC (Toggle Cycle) set.
 *
 * Returns 0 on success, negative errno on failure.
 */
int xhci_ring_create(struct xhci_ring *ring)
{
    memset(ring, 0, sizeof(*ring));

    ring->paddr = pmm_alloc_frame();
    if (!ring->paddr)
        return -ENOMEM;

    ring->trbs = (struct xhci_trb *)PHYS_TO_VIRT((void *)(uintptr_t)ring->paddr);
    memset(ring->trbs, 0, PAGE_SIZE);

    ring->cycle   = 1;   /* Producer cycle starts at 1 (xHCI spec §4.9.3) */
    ring->enq_idx = 0;
    ring->deq_idx = 0;

    /* Set up Link TRB at the last position for circular wrapping.
     * TC=1 causes the xHC to toggle its Consumer Cycle State when it
     * follows this link, keeping producer and consumer in lockstep. */
    {
        struct xhci_trb *link = &ring->trbs[XHCI_RING_TRBS - 1];
        link->parameter = TRB_LINK_PTR((uint64_t)ring->paddr);
        link->status    = 0;
        link->control   = TRB_SET_TYPE(TRB_TYPE_LINK) | TRB_TC_BIT | TRB_CYCLE_BIT;
    }

    return 0;
}

/**
 * xhci_ring_destroy — Free a TRB ring segment.
 */
void xhci_ring_destroy(struct xhci_ring *ring)
{
    if (ring && ring->paddr) {
        pmm_free_frame(ring->paddr);
        memset(ring, 0, sizeof(*ring));
    }
}

/**
 * xhci_ring_enqueue — Place a TRB onto the ring.
 * @ring: Target ring.
 * @trb:  TRB to enqueue (the cycle bit is set automatically).
 *
 * The TRB is written at the current enqueue position with the appropriate
 * cycle bit.  When the last usable slot is filled, the Link TRB's cycle
 * bit is updated and the enqueue pointer wraps to index 0, toggling the
 * producer cycle state.
 *
 * Returns 0 on success, -ENOMEM if ring is full, -EINVAL on bad state.
 */
int xhci_ring_enqueue(struct xhci_ring *ring, const struct xhci_trb *trb)
{
    int idx;

    if (!ring || !ring->trbs)
        return -EINVAL;

    idx = ring->enq_idx;

    /* Usable positions are 0 .. XHCI_RING_USABLE-1 (TRB 255 is Link) */
    if (idx < 0 || idx >= XHCI_RING_USABLE)
        return -EINVAL;

    /* Quick fullness check: if the next usable position equals deq_idx
     * the ring is full (we always leave at least one slot open to
     * distinguish empty from full). */
    {
        int nxt = idx + 1;
        if (nxt >= XHCI_RING_USABLE)
            nxt = 0;
        if (nxt == ring->deq_idx)
            return -ENOMEM;
    }

    /* Write the TRB */
    ring->trbs[idx].parameter = trb->parameter;
    ring->trbs[idx].status    = trb->status;

    /* Set the cycle bit per current producer cycle */
    uint32_t ctrl = trb->control;
    if (ring->cycle)
        ctrl |=  TRB_CYCLE_BIT;
    else
        ctrl &= ~TRB_CYCLE_BIT;
    ring->trbs[idx].control = ctrl;

    /* Memory barrier: ensure TRB is fully written before xHC can see it */
    __asm__ volatile("mfence" ::: "memory");

    /* Advance the enqueue pointer */
    if (idx == XHCI_RING_USABLE - 1) {
        /* Last usable slot written — update the Link TRB's cycle so the
         * xHC can follow it, then wrap to the beginning. */
        uint32_t lctrl = TRB_SET_TYPE(TRB_TYPE_LINK) | TRB_TC_BIT;
        if (ring->cycle)
            lctrl |= TRB_CYCLE_BIT;
        ring->trbs[XHCI_RING_TRBS - 1].control = lctrl;
        __asm__ volatile("mfence" ::: "memory");

        ring->enq_idx = 0;
        ring->cycle = !ring->cycle;   /* Toggle producer cycle */
    } else {
        ring->enq_idx = idx + 1;
    }

    return 0;
}

/**
 * xhci_ring_has_pending — Check whether the xHC has work on this ring.
 * Returns non-zero if at least one TRB is available for the xHC to
 * consume (based on whether enq_idx != deq_idx).
 */
int xhci_ring_has_pending(struct xhci_ring *ring)
{
    if (!ring || !ring->trbs)
        return 0;

    int e = ring->enq_idx;
    int d = ring->deq_idx;

    /* If enq == deq the ring could be either full or empty, but in
     * practice a full ring only happens if the driver overcommits —
     * treat as "has pending" conservatively so the caller drains. */
    if (e != d)
        return 1;

    /* enq == deq: check the TRB's cycle bit to disambiguate.
     * If the TRB at deq_idx has the expected consumer cycle, it's pending. */
    uint32_t ctrl = ring->trbs[d].control;
    return (ctrl & TRB_CYCLE_BIT) != 0;
}

/**
 * xhci_ring_dequeue — Read a consumed TRB from the ring (driver side).
 * @ring:            Target ring.
 * @trb:             Output buffer for the consumed TRB.
 * @consumer_cycle:  Expected cycle bit from the consumer's perspective.
 *
 * If the TRB at the dequeue position has the expected cycle bit (meaning the
 * consumer — xHC — has already processed it), it is copied into @trb and the
 * dequeue pointer is advanced.  Link TRBs are skipped transparently.
 *
 * Returns: 1 if a TRB was consumed, 0 if nothing available,
 *          negative errno on error.
 */
int xhci_ring_dequeue(struct xhci_ring *ring, struct xhci_trb *trb, int consumer_cycle)
{
    int idx;

    if (!ring || !ring->trbs || !trb)
        return -EINVAL;

    idx = ring->deq_idx;

    /* Skip Link TRBs (the last slot) */
    if (idx < 0 || idx >= XHCI_RING_USABLE)
        return 0;

    /* Check cycle bit to see if xHC has consumed this TRB */
    {
        uint32_t ctrl = ring->trbs[idx].control;
        int trb_cycle = (ctrl & TRB_CYCLE_BIT) ? 1 : 0;
        if (trb_cycle != consumer_cycle)
            return 0;
    }

    /* Copy the consumed TRB */
    *trb = ring->trbs[idx];

    /* Advance dequeue pointer, wrapping past the Link TRB */
    ring->deq_idx++;
    if (ring->deq_idx >= XHCI_RING_USABLE)
        ring->deq_idx = 0;

    return 1;
}

/**
 * xhci_event_ring_init — Initialise an event ring with one segment.
 * @ev:        Event ring structure (caller-allocated).
 * @num_trbs:  Number of event TRBs (must be a power of 2, ≥ 16).
 *
 * Allocates the event ring segment and one ERST entry.  The ERST is
 * programmed later when the controller's runtime registers are known.
 *
 * Returns 0 on success, negative errno on failure.
 */
int xhci_event_ring_init(struct xhci_event_ring *ev, int num_trbs)
{
    size_t seg_size;

    memset(ev, 0, sizeof(*ev));

    /* Event ring must have a power-of-2 number of TRBs */
    if (num_trbs < 16 || (num_trbs & (num_trbs - 1)) != 0)
        return -EINVAL;

    ev->num_trbs = num_trbs;

    /* Allocate event ring segment (16-byte TRBs) */
    seg_size = (size_t)num_trbs * sizeof(struct xhci_trb);
    if (seg_size > PAGE_SIZE) {
        /* For rings larger than 4K we'd need multiple pages; for now
         * cap at one page (256 TRBs max, same as command/transfer rings). */
        ev->num_trbs = PAGE_SIZE / (int)sizeof(struct xhci_trb);
        seg_size = PAGE_SIZE;
    }

    ev->paddr = pmm_alloc_frame();
    if (!ev->paddr)
        return -ENOMEM;

    ev->trbs = (struct xhci_trb *)PHYS_TO_VIRT((void *)(uintptr_t)ev->paddr);
    memset(ev->trbs, 0, seg_size);

    ev->num_trbs = (int)(seg_size / sizeof(struct xhci_trb));
    ev->deq_idx  = 0;
    ev->cycle    = 1;   /* Consumer cycle starts at 1 (per spec) */

    /* Allocate ERST (one page for simplicity) */
    ev->erst_paddr = pmm_alloc_frame();
    if (!ev->erst_paddr) {
        pmm_free_frame(ev->paddr);
        ev->paddr = 0;
        return -ENOMEM;
    }

    ev->erst = (struct xhci_erst_entry *)PHYS_TO_VIRT((void *)(uintptr_t)ev->erst_paddr);
    memset(ev->erst, 0, PAGE_SIZE);

    /* Fill in the ERST entry */
    ev->erst[0].seg_addr = (uint64_t)ev->paddr;  /* bits 63:4 of segment address */
    ev->erst[0].seg_size = (uint32_t)ev->num_trbs; /* number of TRBs in segment */
    ev->erst_entries = 1;

    return 0;
}

/**
 * xhci_event_ring_fini — Free event ring resources.
 */
void xhci_event_ring_fini(struct xhci_event_ring *ev)
{
    if (!ev) return;

    if (ev->paddr) {
        pmm_free_frame(ev->paddr);
        ev->paddr = 0;
    }
    if (ev->erst_paddr) {
        pmm_free_frame(ev->erst_paddr);
        ev->erst_paddr = 0;
    }
    memset(ev, 0, sizeof(*ev));
}

/**
 * xhci_event_ring_process — Drain pending events from the event ring.
 * @ev:      Event ring to process.
 * @handler: Callback invoked for each event TRB.
 * @ctx:     Opaque context pointer for the handler.
 *
 * Reads event TRBs whose cycle bit matches the current consumer cycle,
 * invokes the handler for each, and advances the dequeue pointer.
 *
 * Returns the number of events processed.
 */
int xhci_event_ring_process(struct xhci_event_ring *ev,
                             void (*handler)(struct xhci_trb *ev_trb, void *ctx),
                             void *ctx)
{
    int count = 0;
    int idx;

    if (!ev || !ev->trbs)
        return 0;

    while (1) {
        idx = ev->deq_idx;

        /* Check cycle bit */
        uint32_t ctrl = ev->trbs[idx].control;
        int trb_cycle = (ctrl & TRB_CYCLE_BIT) ? 1 : 0;

        if (trb_cycle != ev->cycle)
            break;   /* No more valid events */

        /* Call the handler */
        if (handler)
            handler(&ev->trbs[idx], ctx);

        /* Advance dequeue */
        ev->deq_idx++;
        if (ev->deq_idx >= ev->num_trbs) {
            ev->deq_idx = 0;
            ev->cycle = !ev->cycle;   /* Toggle consumer cycle */
        }

        count++;
    }

    return count;
}

/* ── Doorbell ─────────────────────────────────────────────────────────── */

/**
 * xhci_ring_doorbell — Ring the doorbell for a slot/endpoint.
 * @xhci:            Controller.
 * @slot_id:         Device slot ID (0 for command ring).
 * @doorbell_target: Endpoint ID (1-31) or 0 for command.
 *
 * Writing to the doorbell register tells the xHC there is new work
 * on the associated ring.
 */
void xhci_ring_doorbell(struct xhci_controller *xhci, int slot_id, int doorbell_target)
{
    uint64_t db_reg;

    if (!xhci)
        return;

    db_reg = XHCI_DOORBELL_REG(xhci->db_off, slot_id);
    xhci_write32(xhci, xhci->cap_regs, db_reg, (uint32_t)(doorbell_target & 0xFF));
    __asm__ volatile("mfence" ::: "memory");
}

/* ── Endpoint / Device Context Management ──────────────────────────── */

/* ── DCBAA (Device Context Base Address Array) ────────────────────── */

/**
 * xhci_dcbaa_init — Allocate and program the Device Context Base Address Array.
 *
 * DCBAA holds one 64-bit pointer per device slot.  The register at
 * XHCI_OP_DCBAAP points to it.  Called once during controller init.
 *
 * Returns 0 on success, negative errno on failure.
 */
int xhci_dcbaa_init(struct xhci_controller *xhci)
{
    if (!xhci || !xhci->present)
        return -ENODEV;

    if (xhci->dcbaa)
        return 0;  /* Already initialised */

    /* DCBAA: one 64-bit pointer per device slot, max 255 slots.
     * One page (4096 bytes) holds 512 pointers — more than enough. */
    xhci->dcbaa_paddr = pmm_alloc_frame();
    if (!xhci->dcbaa_paddr)
        return -ENOMEM;

    xhci->dcbaa = (uint32_t *)PHYS_TO_VIRT((void *)(uintptr_t)xhci->dcbaa_paddr);
    memset(xhci->dcbaa, 0, PAGE_SIZE);

    /* Program DCBAAP register (two 32-bit writes for 64-bit addr) */
    xhci_write32(xhci, xhci->op_regs, XHCI_OP_DCBAAP,
                 (uint32_t)(xhci->dcbaa_paddr & 0xFFFFFFFF));
    xhci_write32(xhci, xhci->op_regs, XHCI_OP_DCBAAP + 4,
                 (uint32_t)((xhci->dcbaa_paddr >> 32) & 0xFFFFFFFF));
    __asm__ volatile("mfence" ::: "memory");

    kprintf("[xHCI] DCBAA programmed @ 0x%lx\n",
            (unsigned long)xhci->dcbaa_paddr);
    return 0;
}

/**
 * xhci_dcbaa_fini — Tear down DCBAA.
 */
void xhci_dcbaa_fini(struct xhci_controller *xhci)
{
    if (!xhci || !xhci->dcbaa_paddr)
        return;

    /* Clear DCBAAP register */
    xhci_write32(xhci, xhci->op_regs, XHCI_OP_DCBAAP, 0);
    xhci_write32(xhci, xhci->op_regs, XHCI_OP_DCBAAP + 4, 0);

    pmm_free_frame(xhci->dcbaa_paddr);
    xhci->dcbaa = NULL;
    xhci->dcbaa_paddr = 0;
}

/* ── Device Slot Alloc / Free ─────────────────────────────────────── */

/**
 * xhci_dev_slot_alloc — Allocate a device slot with a device-context page.
 * @xhci:       Controller.
 * @port_num:   Root hub port the device is connected to.
 * @speed:      USB speed (XHCI_SPEED_*).
 * @out_slot_id: On success, filled with the 1-based slot ID.
 *
 * Allocates one page (4096 bytes) for the device context (slot + 31 endpoint
 * contexts, 1024 bytes used) and registers it in the DCBAA.
 *
 * Returns 0 on success, negative errno on failure.
 */
int xhci_dev_slot_alloc(struct xhci_controller *xhci, int port_num,
                        int speed, int *out_slot_id)
{
    int slot_id;
    uint64_t ctx_paddr;
    struct xhci_dev_ctx *dev_ctx;

    if (!xhci || !out_slot_id)
        return -EINVAL;
    if (port_num < 0 || port_num >= (int)xhci->max_ports)
        return -EINVAL;
    if (speed < XHCI_SPEED_FULL || speed > XHCI_SPEED_SUPER_PLUS)
        return -EINVAL;
    if (xhci->num_slots_used >= (int)xhci->max_slots)
        return -ENOSPC;

    /* Find a free slot ID (1-based, slot 0 is unused) */
    for (slot_id = 1; slot_id <= (int)xhci->max_slots; slot_id++) {
        if (!xhci->slots[slot_id].enabled)
            break;
    }
    if (slot_id > (int)xhci->max_slots)
        return -ENOSPC;

    /* Allocate a page for the device context */
    ctx_paddr = pmm_alloc_frame();
    if (!ctx_paddr)
        return -ENOMEM;

    dev_ctx = (struct xhci_dev_ctx *)PHYS_TO_VIRT((void *)(uintptr_t)ctx_paddr);
    memset(dev_ctx, 0, PAGE_SIZE);

    /* Initialise the slot structure */
    {
        struct xhci_dev_slot *slot = &xhci->slots[slot_id];
        memset(slot, 0, sizeof(*slot));
        slot->slot_id    = slot_id;
        slot->enabled    = 1;
        slot->port_num   = port_num;
        slot->speed      = speed;
        slot->dev_ctx    = dev_ctx;
        slot->dev_ctx_paddr = ctx_paddr;
    }

    /* Update DCBAA entry for this slot (two 32-bit writes = 64-bit ptr) */
    xhci->dcbaa[slot_id * 2]     = (uint32_t)(ctx_paddr & 0xFFFFFFFF);
    xhci->dcbaa[slot_id * 2 + 1] = (uint32_t)((ctx_paddr >> 32) & 0xFFFFFFFF);
    __asm__ volatile("mfence" ::: "memory");

    xhci->num_slots_used++;
    *out_slot_id = slot_id;

    kprintf("[xHCI] Slot %d allocated (port %d, speed %d, ctx @ 0x%lx)\n",
            slot_id, port_num, speed, (unsigned long)ctx_paddr);
    return 0;
}

/**
 * xhci_dev_slot_free — Release a device slot.
 */
int xhci_dev_slot_free(struct xhci_controller *xhci, int slot_id)
{
    struct xhci_dev_slot *slot;
    int i;

    if (!xhci || slot_id < 1 || slot_id > (int)xhci->max_slots)
        return -EINVAL;

    slot = &xhci->slots[slot_id];
    if (!slot->enabled)
        return -ENOENT;

    /* Free per-endpoint transfer rings */
    for (i = 0; i < MAX_XHCI_ENDPOINTS; i++) {
        if (slot->ep_rings_initialized[i]) {
            xhci_ring_destroy(&slot->ep_rings[i]);
            slot->ep_rings_initialized[i] = 0;
        }
    }

    /* Free device context page */
    if (slot->dev_ctx_paddr) {
        pmm_free_frame(slot->dev_ctx_paddr);
    }

    /* Clear DCBAA entry */
    if (xhci->dcbaa) {
        xhci->dcbaa[slot_id * 2]     = 0;
        xhci->dcbaa[slot_id * 2 + 1] = 0;
        __asm__ volatile("mfence" ::: "memory");
    }

    memset(slot, 0, sizeof(*slot));
    xhci->num_slots_used--;

    kprintf("[xHCI] Slot %d freed\n", slot_id);
    return 0;
}

/* ── Per-Endpoint Transfer Rings ──────────────────────────────────── */

/**
 * xhci_ep_ring_create — Create a transfer ring for an endpoint.
 * @slot:   Device slot the endpoint belongs to.
 * @ep_idx: Endpoint index (0..30, maps to EP ID 1..31).
 *
 * Returns 0 on success, negative errno on failure.
 */
int xhci_ep_ring_create(struct xhci_dev_slot *slot, int ep_idx)
{
    int ret;

    if (!slot || !slot->enabled)
        return -EINVAL;
    if (ep_idx < 0 || ep_idx >= MAX_XHCI_ENDPOINTS)
        return -EINVAL;
    if (slot->ep_rings_initialized[ep_idx])
        return 0;  /* Already created */

    ret = xhci_ring_create(&slot->ep_rings[ep_idx]);
    if (ret < 0)
        return ret;

    slot->ep_rings_initialized[ep_idx] = 1;
    return 0;
}

/**
 * xhci_ep_ring_free — Destroy a transfer ring for an endpoint.
 */
void xhci_ep_ring_free(struct xhci_dev_slot *slot, int ep_idx)
{
    if (!slot || ep_idx < 0 || ep_idx >= MAX_XHCI_ENDPOINTS)
        return;

    if (slot->ep_rings_initialized[ep_idx]) {
        xhci_ring_destroy(&slot->ep_rings[ep_idx]);
        slot->ep_rings_initialized[ep_idx] = 0;
    }
}

/* ── Context Initialisation Helpers ───────────────────────────────── */

/**
 * xhci_slot_context_init — Fill in a slot context structure.
 *
 * Writes DW0 and DW1 (route string, speed, root port info, context entries).
 * Caller is responsible for placing the result in the right position within
 * an input context or device context.
 *
 * Returns 0 on success, negative errno on failure.
 */
int xhci_slot_context_init(struct xhci_slot_ctx *sctx, int route_string,
                           int speed, int root_port, int num_ports,
                           int ctx_entries)
{
    if (!sctx)
        return -EINVAL;

    memset(sctx, 0, sizeof(*sctx));

    /* DW0: Route String | Speed | Context Entries */
    sctx->dw0 = XHCI_BF(route_string, XHCI_SLOT_CTX_ROUTE_STRING_S,
                        XHCI_SLOT_CTX_ROUTE_STRING_M)
              | XHCI_BF(speed, XHCI_SLOT_CTX_SPEED_S, XHCI_SLOT_CTX_SPEED_M)
              | XHCI_BF(ctx_entries, XHCI_SLOT_CTX_CTX_ENTRIES_S,
                        XHCI_SLOT_CTX_CTX_ENTRIES_M);

    /* DW1: Root Hub Port Number | Number of Ports */
    sctx->dw1 = XHCI_BF(root_port, XHCI_SLOT_CTX_RH_PORT_NUM_S,
                        XHCI_SLOT_CTX_RH_PORT_NUM_M)
              | XHCI_BF(num_ports, XHCI_SLOT_CTX_NUM_PORTS_S,
                        XHCI_SLOT_CTX_NUM_PORTS_M);

    return 0;
}

/**
 * xhci_ep_context_init — Fill in an endpoint context structure.
 *
 * Sets EP type, CErr=3 (as recommended by the xHCI spec), the TR dequeue
 * pointer (pointing to the endpoint's transfer ring), max packet size,
 * and max burst size.
 *
 * Returns 0 on success, negative errno on failure.
 */
int xhci_ep_context_init(struct xhci_endpoint_ctx *ep_ctx, int ep_type,
                         int max_packet_size, int max_burst_size,
                         uint64_t tr_dequeue_paddr, int dcs)
{
    if (!ep_ctx)
        return -EINVAL;
    if (ep_type < 0 || ep_type > 7)
        return -EINVAL;

    memset(ep_ctx, 0, sizeof(*ep_ctx));

    /* DW0: EP State = 0 (Disabled — will transition when the xHC processes
     * the Configure Endpoint command) */
    ep_ctx->dw0 = 0;

    /* DW1: EP Type, CErr = 3 (max error count per xHCI recommendation) */
    ep_ctx->dw1 = XHCI_BF(3, XHCI_EP_CTX_CERR_S, XHCI_EP_CTX_CERR_M)
                | XHCI_BF(ep_type, XHCI_EP_CTX_EP_TYPE_S, XHCI_EP_CTX_EP_TYPE_M);

    /* DW2: TR Dequeue Pointer Lo (bits 3:31) + DCS (bit 0)
     * DW3: TR Dequeue Pointer Hi */
    ep_ctx->dw2 = (dcs ? 1U : 0U)
                | ((uint32_t)(tr_dequeue_paddr >> 4)
                   << XHCI_EP_CTX_TR_DEQUEUE_PTR_LO_S);
    ep_ctx->dw3 = (uint32_t)((tr_dequeue_paddr >> 32) & 0xFFFFFFFF);

    /* DW4: Max Packet Size | Max Burst / ESIT Payload */
    ep_ctx->dw4 = XHCI_BF(max_packet_size, XHCI_EP_CTX_MAX_PACKET_SIZE_S,
                          XHCI_EP_CTX_MAX_PACKET_SIZE_M)
                | XHCI_BF(max_burst_size, XHCI_EP_CTX_MAX_ESIT_PAYLOAD_S,
                          XHCI_EP_CTX_MAX_ESIT_PAYLOAD_M);

    return 0;
}

/**
 * xhci_input_ctx_init — Zero and set the drop/add flags of an input context.
 *
 * @drop_mask: Bitmask of contexts to drop (bit N = endpoint context N,
 *             bit 0 = slot context).
 * @add_mask:  Bitmask of contexts to add/update.
 *
 * Returns 0 on success, negative errno on failure.
 */
int xhci_input_ctx_init(struct xhci_input_ctx *in_ctx, int drop_mask,
                        int add_mask)
{
    if (!in_ctx)
        return -EINVAL;

    memset(in_ctx, 0, sizeof(*in_ctx));
    in_ctx->icc.drop_flags = (uint32_t)drop_mask;
    in_ctx->icc.add_flags  = (uint32_t)add_mask;
    return 0;
}

/* ── Endpoint / Slot State Queries ────────────────────────────────── */

int xhci_ep_ctx_get_state(const struct xhci_endpoint_ctx *ep_ctx)
{
    if (!ep_ctx)
        return -EINVAL;
    return XHCI_BF_GET(ep_ctx->dw0, XHCI_EP_CTX_STATE_S, XHCI_EP_CTX_STATE_M);
}

int xhci_slot_ctx_get_state(const struct xhci_slot_ctx *sctx)
{
    if (!sctx)
        return -EINVAL;
    return XHCI_BF_GET(sctx->dw3, XHCI_SLOT_CTX_SLOT_STATE_S,
                       XHCI_SLOT_CTX_SLOT_STATE_M);
}

/* ── Configure Endpoint Command ───────────────────────────────────── */

/**
 * xhci_configure_endpoint — Build and submit a Configure Endpoint command.
 * @xhci:       Controller.
 * @slot_id:    Target device slot (1-based).
 * @in_ctx_paddr: Physical address of the input context (must be 64-byte
 *               aligned, caller-managed).
 *
 * The caller must have filled the input context with the desired slot and
 * endpoint contexts and the drop/add flags before calling this function.
 * The input context must reside in physically contiguous, 64-byte aligned
 * memory (e.g., allocated via pmm_alloc_frame()).
 *
 * Returns 0 on success, negative errno on failure.
 */
int xhci_configure_endpoint(struct xhci_controller *xhci, int slot_id,
                            uint64_t in_ctx_paddr)
{
    struct xhci_trb cmd_trb;
    int ret;

    if (!xhci || !xhci->present || !xhci->cmd_ring.trbs)
        return -ENODEV;
    if (slot_id < 1 || slot_id > (int)xhci->max_slots)
        return -EINVAL;
    if (!in_ctx_paddr || (in_ctx_paddr & 0x3FULL))
        return -EINVAL;

    /* Build Configure Endpoint command TRB (type 12, xHCI 1.2 §6.4.3.5) */
    memset(&cmd_trb, 0, sizeof(cmd_trb));
    cmd_trb.parameter = in_ctx_paddr & ~0x3FULL;  /* 64-byte aligned */
    cmd_trb.status    = (uint32_t)((slot_id & 0xFF) << 24);  /* Slot ID */
    cmd_trb.control   = TRB_SET_TYPE(TRB_TYPE_CONFIG_ENDPOINT)
                      | TRB_IOC_BIT
                      | TRB_CYCLE_BIT;

    /* Enqueue the command on the command ring */
    ret = xhci_ring_enqueue(&xhci->cmd_ring, &cmd_trb);
    if (ret < 0) {
        kprintf("[xHCI] configure_endpoint: enqueue failed: %d\n", ret);
        return ret;
    }

    /* Ring the command doorbell (slot 0, target 0) */
    xhci_ring_doorbell(xhci, 0, 0);

    kprintf("[xHCI] Configure Endpoint submitted (slot %d, in_ctx @ 0x%lx)\n",
            slot_id, (unsigned long)in_ctx_paddr);
    return 0;
}

/* ── Ring init in controller startup ──────────────────────────────────── */

/**
 * xhci_rings_init — Initialise the command ring and event ring.
 * Called after the controller is started and before devices are enumerated.
 */
static int xhci_rings_init(struct xhci_controller *xhci)
{
    int ret;

    if (!xhci || !xhci->present)
        return -ENODEV;

    if (xhci->rings_initialized)
        return 0;

    /* Create command ring */
    ret = xhci_ring_create(&xhci->cmd_ring);
    if (ret < 0) {
        kprintf("[xHCI] Failed to create command ring: %d\n", ret);
        return ret;
    }

    /* Create event ring (256 TRBs) */
    ret = xhci_event_ring_init(&xhci->ev_ring, 256);
    if (ret < 0) {
        kprintf("[xHCI] Failed to create event ring: %d\n", ret);
        xhci_ring_destroy(&xhci->cmd_ring);
        return ret;
    }

    /* Program the Command Ring Control Register (CRCR) */
    {
        uint64_t crcr = (uint64_t)xhci->cmd_ring.paddr & ~0x3FULL;
        crcr |= 0;  /* CRR=0 (not running), RCS matches first producer cycle = 1 */
        /* Write CRCR low 32 bits */
        xhci_write32(xhci, xhci->op_regs, XHCI_OP_CRCR, (uint32_t)(crcr & 0xFFFFFFFF));
        /* Write CRCR high 32 bits (must be 0 below 4GB, which we assume) */
        xhci_write32(xhci, xhci->op_regs, XHCI_OP_CRCR + 4, (uint32_t)((crcr >> 32) & 0xFFFFFFFF));
    }

    /* Program the Event Ring registers in the Runtime Register Space */
    {
        uint64_t rt_base = xhci->rt_off;

        /* ERSTSZ — number of segments (1) */
        xhci_write32(xhci, rt_base, XHCI_ERSTSZ(0, 0), 1);

        /* ERSTBA — physical address of ERST array */
        uint64_t erst_ba = xhci->ev_ring.erst_paddr;
        xhci_write32(xhci, rt_base, XHCI_ERSTBA(0, 0), (uint32_t)(erst_ba & 0xFFFFFFFF));
        xhci_write32(xhci, rt_base, XHCI_ERSTBA(0, 0) + 4, (uint32_t)((erst_ba >> 32) & 0xFFFFFFFF));

        /* ERDP — Event Ring Dequeue Pointer (point to start, DCS=1) */
        uint64_t erdp = ((uint64_t)xhci->ev_ring.paddr & ~0x0FULL) | 1ULL;  /* DCS=1 */
        xhci_write32(xhci, rt_base, XHCI_ERDP(0, 0), (uint32_t)(erdp & 0xFFFFFFFF));
        xhci_write32(xhci, rt_base, XHCI_ERDP(0, 0) + 4, (uint32_t)((erdp >> 32) & 0xFFFFFFFF));
    }

    /* Allocate and program the Device Context Base Address Array */
    ret = xhci_dcbaa_init(xhci);
    if (ret < 0) {
        kprintf("[xHCI] Failed to initialise DCBAA: %d\n", ret);
        xhci_event_ring_fini(&xhci->ev_ring);
        xhci_ring_destroy(&xhci->cmd_ring);
        return ret;
    }

    /* Initialise device slot array */
    memset(xhci->slots, 0, sizeof(xhci->slots));
    xhci->num_slots_used = 0;

    xhci->rings_initialized = 1;
    kprintf("[xHCI] Rings initialized (cmd @ 0x%lx, ev @ 0x%lx)\n",
            (unsigned long)xhci->cmd_ring.paddr,
            (unsigned long)xhci->ev_ring.paddr);
    return 0;
}

/**
 * xhci_rings_fini — Tear down rings.
 */
static void xhci_rings_fini(struct xhci_controller *xhci)
{
    if (!xhci) return;
    if (!xhci->rings_initialized) return;

    xhci_dcbaa_fini(xhci);
    xhci_event_ring_fini(&xhci->ev_ring);
    xhci_ring_destroy(&xhci->cmd_ring);
    xhci->rings_initialized = 0;
}

int xhci_port_reset(int port) {
    if (!g_xhci.present || port < 0 || port >= g_xhci.max_ports)
        return -1;

    uint64_t portsc_base = g_xhci.op_regs + XHCI_PORTSC + (port * 0x10);
    uint32_t portsc = xhci_read32(&g_xhci, portsc_base, 0);

    /* Set port reset bit */
    xhci_write32(&g_xhci, portsc_base, 0, portsc | XHCI_PORTSC_PR);

    /* Wait for reset to complete */
    int timeout = 100000;
    while (timeout--) {
        portsc = xhci_read32(&g_xhci, portsc_base, 0);
        if (!(portsc & XHCI_PORTSC_PR))
            break;
        __asm__ volatile("pause");
    }

    if (timeout <= 0)
        return -1;

    /* Acknowledge port reset change */
    xhci_write32(&g_xhci, portsc_base, 0, portsc | XHCI_PORTSC_PRC);
    return 0;
}

int xhci_port_status(int port) {
    if (!g_xhci.present || port < 0 || port >= g_xhci.max_ports)
        return -1;

    uint64_t portsc_base = g_xhci.op_regs + XHCI_PORTSC + (port * 0x10);
    uint32_t portsc = xhci_read32(&g_xhci, portsc_base, 0);

    return (int)portsc;
}

int xhci_init(void) {
    if (g_xhci_init_done)
        return 0;

    memset(&g_xhci, 0, sizeof(g_xhci));

    if (xhci_probe_pci() < 0) {
        kprintf("[xHCI] No xHCI controller found\n");
        g_xhci_init_done = 1;
        return -1;
    }

    if (xhci_start_controller() < 0) {
        kprintf("[xHCI] Failed to start controller\n");
        return -1;
    }

    /* Initialise command ring + event ring */
    if (xhci_rings_init(&g_xhci) < 0) {
        kprintf("[xHCI] Failed to initialise rings\n");
        /* Continue without rings — basic port ops still work */
    }

    /* Reset all ports */
    for (int i = 0; i < g_xhci.max_ports; i++) {
        if (xhci_port_status(i) & XHCI_PORTSC_CCS) {
            kprintf("[xHCI] Port %d: device connected\n", i);
        }
    }

    g_xhci_init_done = 1;
    kprintf("[xHCI] Driver initialized\n");
    return 0;
}

int xhci_is_present(void) {
    return g_xhci.present;
}

void xhci_print_info(void) {
    if (!g_xhci.present) {
        kprintf("xHCI: Not present\n");
        return;
    }
    kprintf("xHCI: present, %d ports, %d slots\n",
            g_xhci.max_ports, g_xhci.max_slots);
}
#include "module.h"
module_init(xhci_init);

/* ── xhci_reset ─────────────────────────────────────────────── */
static int xhci_reset(void *dev)
{
    (void)dev;
    /* Full controller reset followed by ring re-init */
    if (g_xhci.present) {
        xhci_rings_fini(&g_xhci);
        if (xhci_start_controller() < 0)
            return -EIO;
        if (xhci_rings_init(&g_xhci) < 0)
            kprintf("[XHCI] xhci_reset: rings re-init failed\n");
    }
    return 0;
}

/* ── xhci_submit_urb ─────────────────────────────────────────────── */
static int xhci_submit_urb(void *urb)
{
    /* Build and enqueue a Normal TRB on the default endpoint's transfer ring.
     * For now, use the command ring as a simple placeholder; full per-endpoint
     * transfer ring support comes in task #9 (endpoint context management). */
    (void)urb;
    kprintf("[XHCI] xhci_submit_urb: ring-based transfer not yet wired\n");
    return -ENOSYS;
}

/* ── xhci_irq ───────────────────────────────────────────────── */
static void xhci_irq(struct interrupt_frame *frame)
{
    (void)frame;

    if (!g_xhci.present)
        return;

    /* Read USBSTS to determine interrupt cause */
    uint32_t sts = xhci_read32(&g_xhci, g_xhci.op_regs, XHCI_OP_USBSTS);

    if (sts & XHCI_STS_EINT) {
        /* Event Interrupt — process event ring */
        xhci_event_ring_process(&g_xhci.ev_ring, NULL, NULL);
    }

    if (sts & XHCI_STS_PCD) {
        /* Port Change Detect */
        kprintf("[XHCI] Port change detected\n");
    }

    if (sts & XHCI_STS_HSE) {
        /* Host System Error */
        kprintf("[XHCI] Host system error!\n");
    }

    /* Clear interrupt status bits by writing 1 to set bits */
    xhci_write32(&g_xhci, g_xhci.op_regs, XHCI_OP_USBSTS, sts);
}

/* ── Module metadata ─────────────────────────────────────────── */
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1.0");
MODULE_DESCRIPTION("xHCI (USB 3.0) host controller driver with TRB ring management");
MODULE_AUTHOR("1000 Changes Project");

