/*
 * acpi_button.c — ACPI sleep button fixed event driver
 *
 * Implements the ACPI fixed hardware sleep button (Fixed Event 0x01)
 * with callback registration and event tracking.  The sleep button
 * transitions the system to a configured sleep state (default S1)
 * when pressed.
 *
 * ACPI Fixed Events:
 *   0x00 = Power Button  (PM1_STS bit 8)
 *   0x01 = Sleep Button  (PM1_STS bit 9)
 *
 * Reference: ACPI v6.3, Sections 4.8.2.1.2 (Sleep Button), 5.2.9 (FADT).
 */

#include "acpi_sleep_button.h"
#include "acpi.h"
#include "io.h"
#include "printf.h"
#include "string.h"

static int g_slpbtn_init_done = 0;
static int g_slpbtn_present = 0;
static int g_slpbtn_state = 0;
static uint32_t g_slpbtn_target_state = ACPI_SLPBTN_DEFAULT_SLEEP_STATE;
static acpi_slpbtn_callback_t g_slpbtn_callback = NULL;

/* ACPI PM1a_EVT_BLK ports for sleep button status */
#define PM1a_EVT_BLK          0x1000  /* Will be set from FADT in real system */
#define ACPI_SLPBTN_STS       (1U << 9)   /* Sleep button status bit in PM1_STS */
#define ACPI_SLPBTN_EN        (1U << 9)   /* Sleep button enable bit in PM1_EN */
#define ACPI_PM1_STS          0x00  /* Offset of PM1 status within EVT_BLK */
#define ACPI_PM1_EN           0x02  /* Offset of PM1 enable within EVT_BLK */

/* Local PM1a EVT BLK base — defaults to PM1a_EVT_BLK */
static uint32_t g_slpbtn_pm1a_evt_blk = PM1a_EVT_BLK;

int __init acpi_sleep_button_init(void)
{
    if (g_slpbtn_init_done)
        return 0;

    /* Use FADT address if available — currently using default */

    /* Enable sleep button event */
    uint16_t pm1_en = inw((uint16_t)g_slpbtn_pm1a_evt_blk + ACPI_PM1_EN);
    outw((uint16_t)g_slpbtn_pm1a_evt_blk + ACPI_PM1_EN,
         pm1_en | ACPI_SLPBTN_EN);

    g_slpbtn_present = 1;
    g_slpbtn_init_done = 1;
    g_slpbtn_target_state = ACPI_SLPBTN_DEFAULT_SLEEP_STATE;

    kprintf("[ACPI_SLPBTN] Sleep button driver initialized "
            "(target state S%u)\n", (uint32_t)g_slpbtn_target_state);
    return 0;
}

int acpi_sleep_button_read(void)
{
    if (!g_slpbtn_present)
        return -1;

    /* Read PM1 status for sleep button flag */
    uint16_t pm1_sts = inw((uint16_t)g_slpbtn_pm1a_evt_blk + ACPI_PM1_STS);
    if (pm1_sts & ACPI_SLPBTN_STS) {
        /* Clear by writing 1 to the status bit */
        outw((uint16_t)g_slpbtn_pm1a_evt_blk + ACPI_PM1_STS,
             ACPI_SLPBTN_STS);
        g_slpbtn_state = ACPI_SLPBTN_PRESSED;

        if (g_slpbtn_callback)
            g_slpbtn_callback();

        return ACPI_SLPBTN_PRESSED;
    }

    return ACPI_SLPBTN_RELEASED;
}

int acpi_sleep_button_register_callback(acpi_slpbtn_callback_t cb)
{
    if (!g_slpbtn_present)
        return -1;
    g_slpbtn_callback = cb;
    return 0;
}

void acpi_sleep_button_unregister_callback(void)
{
    g_slpbtn_callback = NULL;
}

int acpi_sleep_button_is_initialized(void)
{
    return g_slpbtn_init_done;
}

void acpi_sleep_button_set_target_state(uint32_t state)
{
    if (state >= ACPI_S1 && state <= ACPI_S5) {
        g_slpbtn_target_state = state;
        kprintf("[ACPI_SLPBTN] Target sleep state set to S%u\n",
                (uint32_t)state);
    }
}

uint32_t acpi_sleep_button_get_target_state(void)
{
    return g_slpbtn_target_state;
}

/* ── Fixed event: acpi_sleep_button_fixed_event_handler ──────────── */
/*
 * ACPI Fixed Event 0x01 handler — Sleep Button.
 *
 * Called from the ACPI SCI dispatch to check whether the fixed
 * hardware sleep button has been pressed.  Reads PM1_STS bit 9
 * (SLPBTN_STS), clears the status bit by writing 1, fires the
 * registered callback (if any), and triggers the configured sleep
 * state transition.  Returns 1 if the event was handled.
 *
 * Reference: ACPI v6.3, Section 4.8.2.1.2 (Sleep Button).
 */
int acpi_sleep_button_fixed_event_handler(void *context)
{
    (void)context;

    if (!g_slpbtn_present || g_slpbtn_pm1a_evt_blk == 0)
        return 0;

    /* Read PM1 status register for the sleep button fixed event bit */
    uint16_t pm1_sts = inw((uint16_t)g_slpbtn_pm1a_evt_blk + ACPI_PM1_STS);
    if (!(pm1_sts & ACPI_SLPBTN_STS))
        return 0;  /* No sleep button event pending */

    /* Clear the status bit by writing 1 to it (ACPI spec: write-1-clear) */
    outw((uint16_t)g_slpbtn_pm1a_evt_blk + ACPI_PM1_STS, ACPI_SLPBTN_STS);

    /* Update internal state */
    g_slpbtn_state = ACPI_SLPBTN_PRESSED;

    kprintf("[ACPI_SLPBTN] Fixed event: sleep button pressed "
            "(PM1_STS bit 9, target S%u)\n",
            (uint32_t)g_slpbtn_target_state);

    /* Fire registered callback if any */
    if (g_slpbtn_callback)
        g_slpbtn_callback();

    /* Trigger the sleep state transition */
    acpi_sleep_button_trigger_sleep();

    return 1;  /* Event handled */
}

/* ── Fixed event: acpi_sleep_button_fixed_event_init ─────────────── */
/*
 * Initialize the ACPI sleep button fixed event handler.
 *
 * Reads the FADT to determine the real PM1a_EVT_BLK base address,
 * stores it in g_slpbtn_pm1a_evt_blk, and enables the sleep button
 * event by setting bit 9 in the PM1 enable register.
 *
 * This is intended to be called from the main ACPI init sequence
 * after the FADT has been parsed and RSDP/XSDT are cached.
 *
 * Reference: ACPI v6.3, Sections 5.2.9 (FADT), 4.8.2.1.2 (Sleep Button).
 */
int acpi_sleep_button_fixed_event_init(void)
{
    if (g_slpbtn_init_done)
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
                g_slpbtn_pm1a_evt_blk = pm1a_blk;
                kprintf("[ACPI_SLPBTN] Fixed event: PM1a_EVT_BLK = 0x%x "
                        "(from FADT)\n", pm1a_blk);
            }
        }
    }

    if (g_slpbtn_pm1a_evt_blk == 0) {
        kprintf("[ACPI_SLPBTN] Fixed event: no PM1a_EVT_BLK available, "
                "disabled\n");
        return -1;
    }

    /* Enable sleep button event in PM1 enable register */
    uint16_t pm1_en = inw((uint16_t)g_slpbtn_pm1a_evt_blk + ACPI_PM1_EN);
    outw((uint16_t)g_slpbtn_pm1a_evt_blk + ACPI_PM1_EN,
         pm1_en | ACPI_SLPBTN_EN);

    g_slpbtn_present = 1;
    g_slpbtn_init_done = 1;
    g_slpbtn_target_state = ACPI_SLPBTN_DEFAULT_SLEEP_STATE;

    kprintf("[ACPI_SLPBTN] Fixed event: sleep button initialized "
            "(PM1a_EVT_BLK=0x%x, target S%u)\n",
            g_slpbtn_pm1a_evt_blk, (uint32_t)g_slpbtn_target_state);
    return 0;
}

/* ── acpi_sleep_button_trigger_sleep ─────────────────────────────── */
/*
 * Trigger the sleep transition based on the configured target state.
 *
 * Called after the sleep button event has been handled to actually
 * put the system to sleep.  Delegates to acpi_sleep() with the
 * configured target state.
 *
 * Returns 0 on success (system entered sleep), -1 on failure.
 */
int acpi_sleep_button_trigger_sleep(void)
{
    kprintf("[ACPI_SLPBTN] Triggering sleep state S%u...\n",
            (uint32_t)g_slpbtn_target_state);

    int ret = acpi_sleep(g_slpbtn_target_state);
    if (ret != 0) {
        kprintf("[ACPI_SLPBTN] Sleep to S%u failed (%d)\n",
                (uint32_t)g_slpbtn_target_state, ret);
    }

    return ret;
}
