/*
 * src/drivers/pvpanic.c — QEMU pvpanic driver
 *
 * Implements MMIO-based detection and panic event reporting
 * for the QEMU pvpanic device (ISA/ACPI device, typically at
 * I/O port 0x505 or MMIO).  When the kernel panics, this driver
 * writes a value to the device to signal the hypervisor.
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "io.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── pvpanic constants ─────────────────────────────────────────── */

/* Standard ACPI/ISA pvpanic port */
#define PVPANIC_PORT            0x505
#define PVPANIC_MMIO_BASE       0xFEDC0000   /* typical QEMU MMIO address */

/* Event types */
#define PVPANIC_EVENT_PANIC     0x01         /* kernel panic occurred */

/* Register offsets */
#define PVPANIC_REG_EVENT       0x00         /* write event type here */

/* ── Driver mode ───────────────────────────────────────────────── */

#define PVPANIC_MODE_UNKNOWN    0
#define PVPANIC_MODE_ISA        1
#define PVPANIC_MODE_MMIO       2

/* ── Driver state ──────────────────────────────────────────────── */

static int  pvpanic_mode     = PVPANIC_MODE_UNKNOWN;
static int  pvpanic_detected = 0;

/* ── Probe / detect the pvpanic device ─────────────────────────── */

static void pvpanic_detect_isa(void)
{
    uint8_t val = inb(PVPANIC_PORT);
    /* Port responds if not 0xFF on non-existent hardware */
    if (val != 0xFF || val == 0x00) {
        pvpanic_mode = PVPANIC_MODE_ISA;
        pvpanic_detected = 1;
        kprintf("[PVPANIC] detected at ISA I/O port 0x%04x (val=0x%02x)\n",
                PVPANIC_PORT, val);
    }
}

static void pvpanic_detect_mmio(void)
{
    volatile uint8_t *reg = (volatile uint8_t *)(uint64_t)PVPANIC_MMIO_BASE;
    uint8_t val = *reg;
    if (val != 0xFF) {
        pvpanic_mode = PVPANIC_MODE_MMIO;
        pvpanic_detected = 1;
        kprintf("[PVPANIC] detected at MMIO 0x%08x (val=0x%02x)\n",
                PVPANIC_MMIO_BASE, val);
    }
}

/* ── Public API: signal a panic event to the hypervisor ────────── */

static void pvpanic_send_event(uint8_t event)
{
    if (!pvpanic_detected) return;

    switch (pvpanic_mode) {
    case PVPANIC_MODE_ISA:
        outb(PVPANIC_PORT + PVPANIC_REG_EVENT, event);
        break;
    case PVPANIC_MODE_MMIO: {
        volatile uint8_t *reg = (volatile uint8_t *)(uint64_t)
            (PVPANIC_MMIO_BASE + PVPANIC_REG_EVENT);
        *reg = event;
        break;
    }
    default:
        break;
    }
}

/* ── Init ──────────────────────────────────────────────────────── */

static void pvpanic_init(void)
{
    pvpanic_detect_isa();
    if (!pvpanic_detected)
        pvpanic_detect_mmio();

    if (pvpanic_detected)
        kprintf("[PVPANIC] QEMU pvpanic device ready, mode=%d\n", pvpanic_mode);
    else
        kprintf("[PVPANIC] not detected\n");
}

#ifdef MODULE
int __init init_module(void) { pvpanic_init(); return 0; }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("QEMU pvpanic — MMIO detection, panic event");
MODULE_VERSION("1.0");
#endif

/* ── Stub: pvpanic_send ─────────────────────────────── */
static int pvpanic_send(uint8_t event)
{
    (void)event;
    kprintf("[PVPANIC] pvpanic_send: not yet implemented\n");
    return 0;
}
