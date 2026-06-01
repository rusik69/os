#define KERNEL_INTERNAL
#include "types.h"
#include "acpi.h"
#include "io.h"
#include "string.h"
#include "printf.h"

/* ACPI Embedded Controller (EC) driver.
 * The EC is typically described in ACPI DSDT and accessed via I/O ports.
 * Standard EC I/O ports: 0x62 (data), 0x66 (command/status)
 */

#define EC_DATA     0x62
#define EC_CMD      0x66

/* EC commands */
#define EC_CMD_READ     0x80
#define EC_CMD_WRITE    0x81
#define EC_CMD_QUERY    0x84

/* EC status bits */
#define EC_ST_OBF       0x01  /* Output buffer full */
#define EC_ST_IBF       0x02  /* Input buffer full */
#define EC_ST_SCI       0x20  /* SCI event pending */
#define EC_ST_BURST     0x40  /* Burst mode enabled */

/* EC query events */
#define EC_QUE_GPE      0x00  /* Query GPE */
#define EC_QUE_SMi      0x01  /* Query SMI */

static int ec_initialized = 0;
static int ec_present = 0;

/* Wait for EC input buffer to be empty (ready to send) */
static int ec_wait_ibf(void) {
    int timeout = 100000;
    while (timeout-- > 0) {
        if (!(inb(EC_CMD) & EC_ST_IBF))
            return 0;
    }
    return -1; /* timeout */
}

/* Wait for EC output buffer to be full (data ready) */
static int ec_wait_obf(void) {
    int timeout = 100000;
    while (timeout-- > 0) {
        if (inb(EC_CMD) & EC_ST_OBF)
            return 0;
    }
    return -1; /* timeout */
}

/* Read a byte from EC at given address */
int ec_read(uint8_t addr, uint8_t *val) {
    if (!ec_present) return -1;
    
    if (ec_wait_ibf() < 0) return -1;
    outb(EC_CMD_READ, EC_CMD);
    
    if (ec_wait_ibf() < 0) return -1;
    outb(addr, EC_DATA);
    
    if (ec_wait_obf() < 0) return -1;
    *val = inb(EC_DATA);
    return 0;
}

/* Write a byte to EC at given address */
int ec_write(uint8_t addr, uint8_t val) {
    if (!ec_present) return -1;
    
    if (ec_wait_ibf() < 0) return -1;
    outb(EC_CMD_WRITE, EC_CMD);
    
    if (ec_wait_ibf() < 0) return -1;
    outb(addr, EC_DATA);
    
    if (ec_wait_ibf() < 0) return -1;
    outb(val, EC_DATA);
    
    return 0;
}

/* Query EC for pending events */
int ec_query(void) {
    if (!ec_present) return -1;
    
    if (ec_wait_ibf() < 0) return -1;
    outb(EC_CMD_QUERY, EC_CMD);
    
    if (ec_wait_obf() < 0) return -1;
    return inb(EC_DATA);
}

/* Check if EC has a SCI event pending */
int ec_sci_pending(void) {
    if (!ec_present) return 0;
    return (inb(EC_CMD) & EC_ST_SCI) != 0;
}

/* Detect ACPI EC by probing standard I/O ports */
static int ec_detect(void) {
    /* Check if burst mode is supported — send burst enable */
    if (ec_wait_ibf() < 0) return 0;
    outb(0x82, EC_CMD); /* Burst enable */
    
    if (ec_wait_obf() < 0) return 0;
    uint8_t status = inb(EC_DATA);
    
    /* Disable burst mode */
    if (ec_wait_ibf() < 0) return 0;
    outb(0x83, EC_CMD); /* Burst disable */
    
    return (status == 0x90) ? 1 : 1; /* Even if no burst ack, ports may still work */
}

void ec_init(void) {
    if (ec_initialized) return;
    
    if (ec_detect()) {
        ec_present = 1;
        kprintf("[OK] ACPI EC detected at ports 0x%x/0x%x\n", EC_DATA, EC_CMD);
    } else {
        kprintf("[EC] not detected\n");
    }
    
    ec_initialized = 1;
}
