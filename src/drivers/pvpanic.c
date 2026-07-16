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
#include "notifier.h"

#include "module.h"

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

/* ── Panic notifier ─────────────────────────────────────────────── */

/* Forward declaration */
int pvpanic_send(uint8_t event);

static struct notifier_block pvpanic_nb = {
    .notifier_call = NULL,
    .next          = NULL,
};

/* Called on kernel panic (via NOTIFIER_PANIC chain). Sends a panic
 * event to the QEMU hypervisor so it can record or react. */
static int pvpanic_panic_notifier(struct notifier_block *nb,
                                  unsigned long action, void *data)
{
    (void)nb;
    (void)data;

    if (action == 0) {
        pvpanic_send(PVPANIC_EVENT_PANIC);
    }

    return 0;
}

/* ── Public API: signal a panic event to the hypervisor ────────── */

int pvpanic_send(uint8_t event)
{
    if (!pvpanic_detected)
        return -1;

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
        return -1;
    }
    return 0;
}

/* ── Init ──────────────────────────────────────────────────────── */

static void pvpanic_init(void)
{
    pvpanic_detect_isa();
    if (!pvpanic_detected)
        pvpanic_detect_mmio();

    if (pvpanic_detected) {
        /* Register panic notifier so we signal QEMU on kernel panic */
        pvpanic_nb.notifier_call = pvpanic_panic_notifier;
        int ret = notifier_chain_register(NOTIFIER_PANIC, &pvpanic_nb);
        if (ret == 0) {
            kprintf("[PVPANIC] QEMU pvpanic device ready, mode=%d, notifier registered\n",
                    pvpanic_mode);
        } else {
            kprintf("[PVPANIC] QEMU pvpanic device ready, mode=%d, notifier FAILED (%d)\n",
                    pvpanic_mode, ret);
        }
    } else {
        kprintf("[PVPANIC] not detected\n");
    }
}

/* ── Built-in / modular init and exit ───────────────────────── */

static void pvpanic_exit(void) {}

module_init(pvpanic_init);
module_exit(pvpanic_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("QEMU pvpanic — MMIO detection, panic event, hypervisor notification");
MODULE_VERSION("1.0");
