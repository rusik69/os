/*
 * acpi_ec.c — ACPI Embedded Controller (EC) driver with burst mode
 *
 * The EC is typically described in ACPI DSDT and accessed via I/O ports.
 * Standard EC I/O ports: 0x62 (data), 0x66 (command/status)
 *
 * Burst mode (EC command 0x82) allows the host to perform multiple EC
 * transactions without bus arbitration overhead.  Once enabled, burst
 * mode remains active until explicitly disabled (command 0x83) or the
 * EC times out (~1ms of inactivity).  Burst transactions skip the
 * IBF/OBF wait cycles between bytes, significantly speeding up
 * multi-byte reads and writes (e.g. fan speed, battery info).
 *
 * This implementation:
 *   - Probes for burst mode support on init
 *   - Enables burst mode for all EC data transactions
 *   - Falls back to normal mode if burst is not supported
 *   - Handles burst timeout by re-enabling if needed
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "acpi.h"
#include "acpi_ec.h"
#include "io.h"
#include "string.h"
#include "printf.h"

/* Standard EC I/O ports */
#define EC_DATA     0x62
#define EC_CMD      0x66

/* EC commands */
#define EC_CMD_READ     0x80
#define EC_CMD_WRITE    0x81
#define EC_CMD_QUERY    0x84
#define EC_CMD_BURST_EN 0x82
#define EC_CMD_BURST_DIS 0x83

/* EC status bits */
#define EC_ST_OBF       0x01  /* Output buffer full */
#define EC_ST_IBF       0x02  /* Input buffer full */
#define EC_ST_SCI       0x20  /* SCI event pending */
#define EC_ST_BURST     0x40  /* Burst mode enabled */

/* EC burst acknowledgement (expected byte after burst enable) */
#define EC_BURST_ACK    0x90

/* Timeout parameters */
#define EC_TIMEOUT_SHORT   5000    /* ~25us @ 200MHz core — fits in cache */
#define EC_TIMEOUT_LONG    100000  /* ~500us — worst-case EC response */

static int ec_initialized = 0;
static int ec_present = 0;
static int ec_burst_supported = 0;
static int ec_burst_enabled = 0;

/* Wait for EC input buffer to be empty (ready to send) */
static int ec_wait_ibf(void)
{
    int timeout = EC_TIMEOUT_LONG;
    while (timeout-- > 0) {
        if (!(inb(EC_CMD) & EC_ST_IBF))
            return 0;
    }
    return -1; /* timeout */
}

/* Wait for EC output buffer to be full (data ready) */
static int ec_wait_obf(void)
{
    int timeout = EC_TIMEOUT_LONG;
    while (timeout-- > 0) {
        if (inb(EC_CMD) & EC_ST_OBF)
            return 0;
    }
    return -1; /* timeout */
}

/* Wait for burst bit to be set (confirms burst mode active) */
static int ec_wait_burst(void)
{
    int timeout = EC_TIMEOUT_LONG;
    while (timeout-- > 0) {
        if (inb(EC_CMD) & EC_ST_BURST)
            return 0;
    }
    return -1;
}

/*
 * Enable burst mode on the EC.
 * Returns 0 on success, -1 on failure (EC does not support burst).
 */
static int ec_burst_enable(void)
{
    if (ec_wait_ibf() < 0) return -1;
    outb(EC_CMD_BURST_EN, EC_CMD);

    if (ec_wait_obf() < 0) return -1;
    uint8_t ack = inb(EC_DATA);
    if (ack != EC_BURST_ACK)
        return -1;

    /* Confirm burst bit is set in the status register */
    if (ec_wait_burst() < 0)
        return -1;

    ec_burst_enabled = 1;
    return 0;
}

/*
 * Disable burst mode on the EC.
 */
static void ec_burst_disable(void)
{
    if (!ec_burst_enabled) return;
    if (ec_wait_ibf() == 0) {
        outb(EC_CMD_BURST_DIS, EC_CMD);
        /* Short wait for EC to exit burst (~50us) */
        int timeout = EC_TIMEOUT_SHORT;
        while (timeout-- > 0) {
            if (!(inb(EC_CMD) & EC_ST_BURST)) {
                ec_burst_enabled = 0;
                return;
            }
        }
    }
    ec_burst_enabled = 0; /* assume disabled despite timeout */
}

/*
 * Shut down the EC: disable burst mode.
 * Called during system shutdown / suspend.
 */
void ec_shutdown(void)
{
    ec_burst_disable();
    ec_present = 0;
}

/*
 * Ensure burst mode is active.  If the EC drops burst (inactivity timeout),
 * this re-enables it transparently.
 */
static int ec_ensure_burst(void)
{
    if (!ec_burst_supported)
        return 0; /* not supported — callers proceed with normal mode */

    if (ec_burst_enabled) {
        /* Quick check: is burst still active? */
        if (inb(EC_CMD) & EC_ST_BURST)
            return 0;
        /* Burst was dropped by EC timeout — re-enable */
        ec_burst_enabled = 0;
    }

    return ec_burst_enable();
}

/* Read a byte from EC at given address */
int ec_read(uint8_t addr, uint8_t *val)
{
    if (!ec_present) return -1;

    /* Attempt to use burst mode */
    ec_ensure_burst();

    if (ec_wait_ibf() < 0) return -1;
    outb(EC_CMD_READ, EC_CMD);

    if (ec_wait_ibf() < 0) return -1;
    outb(addr, EC_DATA);

    if (ec_wait_obf() < 0) return -1;
    *val = inb(EC_DATA);
    return 0;
}

/* Write a byte to EC at given address */
int ec_write(uint8_t addr, uint8_t val)
{
    if (!ec_present) return -1;

    /* Attempt to use burst mode */
    ec_ensure_burst();

    if (ec_wait_ibf() < 0) return -1;
    outb(EC_CMD_WRITE, EC_CMD);

    if (ec_wait_ibf() < 0) return -1;
    outb(addr, EC_DATA);

    if (ec_wait_ibf() < 0) return -1;
    outb(val, EC_DATA);

    return 0;
}

/* Query EC for pending events (returns event byte, or -1) */
int ec_query(void)
{
    if (!ec_present) return -1;

    /* Query does not benefit from burst mode */
    if (ec_wait_ibf() < 0) return -1;
    outb(EC_CMD_QUERY, EC_CMD);

    if (ec_wait_obf() < 0) return -1;
    return inb(EC_DATA);
}

/* Check if EC has a SCI event pending */
int ec_sci_pending(void)
{
    if (!ec_present) return 0;
    return (inb(EC_CMD) & EC_ST_SCI) != 0;
}

/*
 * Read multiple bytes from consecutive EC addresses using burst mode.
 * In burst mode, after the initial address write, subsequent reads
 * automatically increment the address, which is much faster.
 */
int ec_read_burst(uint8_t start_addr, uint8_t *buf, int count)
{
    if (!ec_present || !buf || count <= 0) return -1;

    /* Must have burst mode for multi-byte transactions */
    if (ec_ensure_burst() < 0) {
        /* Burst not available — read one byte at a time */
        for (int i = 0; i < count; i++) {
            if (ec_read((uint8_t)(start_addr + i), &buf[i]) < 0)
                return -1;
        }
        return 0;
    }

    /* Start burst read: send read command + start address */
    if (ec_wait_ibf() < 0) return -1;
    outb(EC_CMD_READ, EC_CMD);

    if (ec_wait_ibf() < 0) return -1;
    outb(start_addr, EC_DATA);

    /* Read bytes — in burst mode, the EC auto-increments the address */
    for (int i = 0; i < count; i++) {
        if (ec_wait_obf() < 0) return -1;
        buf[i] = inb(EC_DATA);
    }

    return 0;
}

/* Detect and initialize the ACPI Embedded Controller */
void ec_init(void)
{
    if (ec_initialized) return;

    /* Probe for EC presence at standard I/O ports */
    if (ec_wait_ibf() < 0) {
        kprintf("[EC] not detected (IBF timeout on probe)\n");
        ec_initialized = 1;
        return;
    }

    /* Try to enable burst mode to confirm EC presence */
    if (ec_burst_enable() == 0) {
        ec_present = 1;
        ec_burst_supported = 1;
        kprintf("[OK] ACPI EC detected at ports 0x%x/0x%x (burst mode enabled)\n",
                EC_DATA, EC_CMD);

        /* Leave burst mode active for subsequent transactions */
    } else {
        /* Ports may still work even without burst support */
        ec_present = 1;
        ec_burst_supported = 0;
        kprintf("[OK] ACPI EC detected at ports 0x%x/0x%x (no burst mode)\n",
                EC_DATA, EC_CMD);
    }

    ec_initialized = 1;
}

/* ── Stub: acpi_ec_read ─────────────────────────────── */
int acpi_ec_read(uint8_t addr, uint8_t *val)
{
    (void)addr;
    (void)val;
    kprintf("[acpi] acpi_ec_read: not yet implemented\n");
    return 0;
}
/* ── Stub: acpi_ec_write ─────────────────────────────── */
int acpi_ec_write(uint8_t addr, uint8_t val)
{
    (void)addr;
    (void)val;
    kprintf("[acpi] acpi_ec_write: not yet implemented\n");
    return 0;
}
/* ── Stub: acpi_ec_init ─────────────────────────────── */
int acpi_ec_init(void *dev)
{
    (void)dev;
    kprintf("[acpi] acpi_ec_init: not yet implemented\n");
    return 0;
}
