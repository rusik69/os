/*
 * acpi_power_button.c — Improved ACPI power button driver
 *
 * Enhanced ACPI fixed hardware power button driver with
 * callback registration and event tracking.
 * NOTE: Functions are prefixed with _ext to avoid symbol
 * conflicts with the basic power button support in acpi.c.
 */

#include "acpi_power_button.h"
#include "acpi.h"
#include "io.h"
#include "printf.h"
#include "string.h"

static int g_pbtn_ext_init_done = 0;
static int g_pbtn_ext_present = 0;
static int g_pbtn_ext_state = 0;
static acpi_pbtn_callback_t g_pbtn_ext_callback = NULL;

/* ACPI PM1a_EVT_BLK ports for power button status */
#define PM1a_EVT_BLK  0x1000  /* Will be set from FADT in real system */
#define ACPI_PWRBTN_STS (1U << 8)   /* Power button status bit in PM1_STS */
#define ACPI_PWRBTN_EN  (1U << 8)   /* Power button enable bit in PM1_EN */
#define ACPI_PM1_STS    0x00  /* Offset of PM1 status within EVT_BLK */
#define ACPI_PM1_EN     0x02  /* Offset of PM1 enable within EVT_BLK */

/* Local PM1a EVT BLK base — defaults to PM1a_EVT_BLK */
static uint32_t g_ext_pm1a_evt_blk = PM1a_EVT_BLK;

int __init acpi_power_button_ext_init(void) {
    if (g_pbtn_ext_init_done)
        return 0;

    /* Use FADT address if available — currently using default */
    /* (in a full ACPI implementation, this would read from FADT) */

    /* Enable power button event */
    uint16_t pm1_en = inw((uint16_t)g_ext_pm1a_evt_blk + ACPI_PM1_EN);
    outw((uint16_t)g_ext_pm1a_evt_blk + ACPI_PM1_EN, pm1_en | ACPI_PWRBTN_EN);

    g_pbtn_ext_present = 1;
    g_pbtn_ext_init_done = 1;
    kprintf("[ACPI_PBTN_EXT] Power button driver initialized\n");
    return 0;
}

int acpi_power_button_ext_read(void) {
    if (!g_pbtn_ext_present)
        return -1;

    /* Read PM1 status for power button flag */
    uint16_t pm1_sts = inw((uint16_t)g_ext_pm1a_evt_blk + ACPI_PM1_STS);
    if (pm1_sts & ACPI_PWRBTN_STS) {
        /* Clear by writing 1 to the status bit */
        outw((uint16_t)g_ext_pm1a_evt_blk + ACPI_PM1_STS, ACPI_PWRBTN_STS);
        g_pbtn_ext_state = ACPI_PBTN_PRESSED;

        if (g_pbtn_ext_callback)
            g_pbtn_ext_callback();

        return ACPI_PBTN_PRESSED;
    }

    return ACPI_PBTN_RELEASED;
}

int acpi_power_button_ext_register_callback(acpi_pbtn_callback_t cb) {
    if (!g_pbtn_ext_present)
        return -1;
    g_pbtn_ext_callback = cb;
    return 0;
}

void acpi_power_button_ext_unregister_callback(void) {
    g_pbtn_ext_callback = NULL;
}

int acpi_power_button_ext_is_initialized(void) {
    return g_pbtn_ext_init_done;
}

/* ── Fixed event: acpi_power_button_fixed_event_handler ──────────── */
/*
 * ACPI Fixed Event 0x00 handler — Power Button.
 *
 * Called from the ACPI SCI dispatch to check whether the fixed
 * hardware power button has been pressed.  Reads PM1_STS bit 8
 * (PWRBTN_STS), clears the status bit by writing 1, fires the
 * registered callback (if any), and returns 1 if the event was
 * handled.
 *
 * Reference: ACPI v6.3, Section 4.8.2.1.1 (Power Button).
 */
int acpi_power_button_fixed_event_handler(void *context)
{
    (void)context;

    if (!g_pbtn_ext_present || g_ext_pm1a_evt_blk == 0)
        return 0;

    /* Read PM1 status register for the power button fixed event bit */
    uint16_t pm1_sts = inw((uint16_t)g_ext_pm1a_evt_blk + ACPI_PM1_STS);
    if (!(pm1_sts & ACPI_PWRBTN_STS))
        return 0;  /* No power button event pending */

    /* Clear the status bit by writing 1 to it (ACPI spec: write-1-clear) */
    outw((uint16_t)g_ext_pm1a_evt_blk + ACPI_PM1_STS, ACPI_PWRBTN_STS);

    /* Update internal state */
    g_pbtn_ext_state = ACPI_PBTN_PRESSED;

    kprintf("[ACPI_PBTN] Fixed event: power button pressed (PM1_STS bit 8)\n");

    /* Fire registered callback if any */
    if (g_pbtn_ext_callback)
        g_pbtn_ext_callback();

    return 1;  /* Event handled */
}

/* ── Fixed event: acpi_power_button_fixed_event_init ────────────── */
/*
 * Initialize the ACPI power button fixed event handler.
 *
 * Reads the FADT to determine the real PM1a_EVT_BLK base address,
 * stores it in g_ext_pm1a_evt_blk, and enables the power button
 * event by setting bit 8 in the PM1 enable register.
 *
 * This is intended to be called from the main ACPI init sequence
 * after the FADT has been parsed and RSDP/XSDT are cached.
 *
 * Reference: ACPI v6.3, Sections 5.2.9 (FADT), 4.8.2.1.1 (Power Button).
 */
int acpi_power_button_fixed_event_init(void)
{
    if (g_pbtn_ext_init_done)
        return 0;

    /*
     * Try to read PM1a_EVT_BLK from the FADT.
     * In the ACPI FADT (FACP), the PM1a_EVT_BLK field is a 32-bit
     * value at byte offset 40 from the start of the table (after the
     * 36-byte ACPI table header, at offset 4 within the FADT-specific
     * fields: byte 40 = sizeof(acpi_header) + 4).
     */
    void *fadt = acpi_get_table("FACP");
    if (fadt) {
        struct acpi_header *hdr = (struct acpi_header *)fadt;
        if (hdr->length >= 44) {
            uint32_t pm1a_blk;
            memcpy(&pm1a_blk, (uint8_t *)fadt + 40, sizeof(pm1a_blk));
            if (pm1a_blk > 0 && pm1a_blk < 0x10000) {
                g_ext_pm1a_evt_blk = pm1a_blk;
                kprintf("[ACPI_PBTN] Fixed event: PM1a_EVT_BLK = 0x%x "
                        "(from FADT)\n", pm1a_blk);
            }
        }
    }

    if (g_ext_pm1a_evt_blk == 0) {
        kprintf("[ACPI_PBTN] Fixed event: no PM1a_EVT_BLK available, "
                "disabled\n");
        return -1;
    }

    /* Enable power button event in PM1 enable register */
    uint16_t pm1_en = inw((uint16_t)g_ext_pm1a_evt_blk + ACPI_PM1_EN);
    outw((uint16_t)g_ext_pm1a_evt_blk + ACPI_PM1_EN,
         pm1_en | ACPI_PWRBTN_EN);

    g_pbtn_ext_present = 1;
    g_pbtn_ext_init_done = 1;

    kprintf("[ACPI_PBTN] Fixed event: power button initialized "
            "(PM1a_EVT_BLK=0x%x)\n", g_ext_pm1a_evt_blk);
    return 0;
}
