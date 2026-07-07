#include "ps2.h"
#include "io.h"
#include "printf.h"

/* ── Wait helpers ─────────────────────────────────────────────────── */

void ps2_wait_write(void) {
    uint32_t timeout = 100000;
    while (timeout-- && (inb(PS2_STATUS) & 0x02))
        __asm__ volatile("pause");
}

void ps2_wait_read(void) {
    uint32_t timeout = 100000;
    while (timeout-- && !(inb(PS2_STATUS) & 0x01))
        __asm__ volatile("pause");
}

/* ── Low-level I/O ────────────────────────────────────────────────── */

void ps2_write_command(uint8_t cmd) {
    ps2_wait_write();
    outb(PS2_CMD, cmd);
}

void ps2_write_data(uint8_t data) {
    ps2_wait_write();
    outb(PS2_DATA, data);
}

uint8_t ps2_read_data(void) {
    ps2_wait_read();
    return inb(PS2_DATA);
}

/* ── Controller initialisation ────────────────────────────────────── */

int ps2_controller_init(void) {
    /* Step 1: Disable both ports */
    ps2_write_command(PS2_CMD_DISABLE_P2);  /* disable port 2 first */
    ps2_write_command(0xAD);                /* disable port 1 */

    /* Step 2: Flush output buffer */
    inb(PS2_DATA);
    io_wait();

    /* Step 3: Read controller config byte */
    ps2_write_command(PS2_CMD_READ_CONFIG);
    uint8_t config = ps2_read_data();

    /* Step 4: Do controller self-test */
    ps2_write_command(PS2_CMD_TEST_CTRL);
    uint8_t test_result = ps2_read_data();
    if (test_result != PS2_TEST_OK) {
        kprintf("[--] PS/2 controller self-test failed (0x%x)\n",
                (uint32_t)test_result);
        return -1;
    }
    kprintf("[OK] PS/2 controller self-test passed\n");

    /* Step 5: Write config back (keep current settings but ensure ports enabled) */
    config |= PS2_CFG_P1_IRQ;
    config = (uint8_t)(config & ~PS2_CFG_P1_CLOCK);  /* enable clock for port 1 */

    /* Check if dual-channel: read port 2 status */
    ps2_write_command(PS2_CMD_DISABLE_P2);
    uint8_t has_port2 = 0;
    ps2_write_command(PS2_CMD_READ_CONFIG);
    uint8_t cfg2 = ps2_read_data();
    if (cfg2 & PS2_CFG_P2_CLOCK) {
        /* Port 2 clock was held low — dual-channel controller present */
        has_port2 = 1;
        config |= PS2_CFG_P2_IRQ;
        config = (uint8_t)(config & ~PS2_CFG_P2_CLOCK);
    }

    /* Write config back */
    ps2_write_command(PS2_CMD_WRITE_CONFIG);
    ps2_write_data(config);

    /* Step 6: Test ports */
    ps2_write_command(PS2_CMD_TEST_P1);
    uint8_t p1_test = ps2_read_data();
    if (p1_test != 0x00) {
        kprintf("[--] PS/2 port 1 test failed (0x%x)\n", (uint32_t)p1_test);
    }

    if (has_port2) {
        ps2_write_command(PS2_CMD_TEST_P2);
        uint8_t p2_test = ps2_read_data();
        if (p2_test != 0x00) {
            kprintf("[--] PS/2 port 2 test failed (0x%x)\n", (uint32_t)p2_test);
            has_port2 = 0;
        }
    }

    /* Step 7: Enable ports */
    ps2_write_command(0xAE); /* enable port 1 */
    config = 0;
    if (has_port2) {
        ps2_write_command(PS2_CMD_ENABLE_P2);
    }

    kprintf("[OK] PS/2 controller initialised (port2: %s)\n",
            has_port2 ? "yes" : "no");
    return 0;
}

/* ── Port helpers ─────────────────────────────────────────────────── */

void ps2_enable_port1(void) {
    ps2_write_command(0xAE);
}

void ps2_enable_port2(void) {
    ps2_write_command(PS2_CMD_ENABLE_P2);
}

void ps2_disable_port2(void) {
    ps2_write_command(PS2_CMD_DISABLE_P2);
}

int ps2_test_port1(void) {
    ps2_write_command(PS2_CMD_TEST_P1);
    return ps2_read_data() == 0x00 ? 0 : -1;
}

int ps2_test_port2(void) {
    ps2_write_command(PS2_CMD_TEST_P2);
    return ps2_read_data() == 0x00 ? 0 : -1;
}

/* ── Flush the PS/2 output buffer ──────────────────── */
int ps2_flush(void)
{
    uint32_t timeout = 100000;
    while (timeout--) {
        if (inb(PS2_STATUS) & 0x01) {
            (void)inb(PS2_DATA); /* read and discard */
        } else {
            break;
        }
        io_wait();
    }
    return 0;
}

/* ── Initialise a PS/2 device (reset and set defaults) ── */
int ps2_init_device(int dev_type)
{
    (void)dev_type;

    /* Send reset command */
    ps2_write_data(0xFF);
    ps2_wait_read();
    uint8_t ack = ps2_read_data();
    if (ack != 0xFA) {
        kprintf("[PS2] Device init: expected ACK (0xFA) got 0x%02x\n", ack);
        return -1;
    }

    /* Self-test result should be 0xAA */
    ps2_wait_read();
    uint8_t self_test = ps2_read_data();
    if (self_test != 0xAA) {
        kprintf("[PS2] Device self-test failed: 0x%02x\n", self_test);
        return -1;
    }

    /* Device ID */
    ps2_wait_read();
    uint8_t dev_id = ps2_read_data();

    /* Set defaults */
    ps2_write_data(0xF6);
    ps2_wait_read();
    ack = ps2_read_data();

    kprintf("[PS2] Device initialised (type=0x%02x, id=0x%02x)\n",
            (uint32_t)dev_type, (uint32_t)dev_id);
    return 0;
}
