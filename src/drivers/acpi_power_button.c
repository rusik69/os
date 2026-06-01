/*
 * acpi_power_button.c — Improved ACPI power button driver
 *
 * Enhanced ACPI fixed hardware power button driver with
 * callback registration and event tracking.
 */

#include "acpi_power_button.h"
#include "acpi.h"
#include "io.h"
#include "printf.h"
#include "string.h"

static int g_pbtn_init_done = 0;
static int g_pbtn_present = 0;
static int g_pbtn_state = 0;
static acpi_pbtn_callback_t g_pbtn_callback = NULL;

/* ACPI PM1a_EVT_BLK ports for power button status */
#define PM1a_EVT_BLK  0x1000  /* Will be set from FADT in real system */
#define ACPI_PWRBTN_STS (1 << 8)   /* Power button status bit in PM1_STS */
#define ACPI_PWRBTN_EN  (1 << 8)   /* Power button enable bit in PM1_EN */
#define ACPI_PM1_STS    0x00  /* Offset of PM1 status within EVT_BLK */
#define ACPI_PM1_EN     0x02  /* Offset of PM1 enable within EVT_BLK */

/* External FADT info from ACPI driver (simplified) */
extern uint32_t g_pm1a_evt_blk;

int acpi_power_button_init(void) {
    if (g_pbtn_init_done)
        return 0;

    /* Use FADT address if available */
    if (g_pm1a_evt_blk == 0)
        g_pm1a_evt_blk = PM1a_EVT_BLK;

    /* Enable power button event */
    uint16_t pm1_en = inw((uint16_t)g_pm1a_evt_blk + ACPI_PM1_EN);
    outw((uint16_t)g_pm1a_evt_blk + ACPI_PM1_EN, pm1_en | ACPI_PWRBTN_EN);

    g_pbtn_present = 1;
    g_pbtn_init_done = 1;
    kprintf("[ACPI_PBTN] Power button driver initialized\n");
    return 0;
}

int acpi_power_button_read(void) {
    if (!g_pbtn_present)
        return -1;

    /* Read PM1 status for power button flag */
    uint16_t pm1_sts = inw((uint16_t)g_pm1a_evt_blk + ACPI_PM1_STS);
    if (pm1_sts & ACPI_PWRBTN_STS) {
        /* Clear by writing 1 to the status bit */
        outw((uint16_t)g_pm1a_evt_blk + ACPI_PM1_STS, ACPI_PWRBTN_STS);
        g_pbtn_state = ACPI_PBTN_PRESSED;

        if (g_pbtn_callback)
            g_pbtn_callback();

        return ACPI_PBTN_PRESSED;
    }

    return ACPI_PBTN_RELEASED;
}

int acpi_power_button_register_callback(acpi_pbtn_callback_t cb) {
    if (!g_pbtn_present)
        return -1;
    g_pbtn_callback = cb;
    return 0;
}

void acpi_power_button_unregister_callback(void) {
    g_pbtn_callback = NULL;
}

int acpi_power_button_is_initialized(void) {
    return g_pbtn_init_done;
}
