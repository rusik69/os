#include "mouse.h"
#include "ps2.h"
#include "io.h"
#include "idt.h"
#include "pic.h"
#include "apic.h"
#include "printf.h"
#include "export.h"

#define VGA_COLS 80
#define VGA_ROWS 25

static volatile int mouse_x = 40;
static volatile int mouse_y = 12;
static volatile uint8_t mouse_buttons = 0;

/* Pixel-space position for framebuffer GUI (1024x768) */
static volatile int mouse_px = 512;
static volatile int mouse_py = 384;
#define FB_WIDTH  1024
#define FB_HEIGHT 768
#define MOUSE_SENSITIVITY 4

/* Packet accumulator (3-byte standard or 4-byte IntelliMouse) */
static uint8_t mouse_cycle = 0;
static int8_t  mouse_bytes[4];
static uint8_t mouse_packet_size = 4; /* bytes per packet (3 or 4) */
static uint8_t mouse_id = 0;          /* 0 = standard, 3 = IntelliMouse, 4 = 5-button */

/* Scroll wheel */
static volatile int mouse_wheel_delta = 0;

static void mouse_write(uint8_t cmd) {
    ps2_write_command(0xD4);  /* next byte goes to mouse */
    ps2_write_data(cmd);
}

static uint8_t mouse_read(void) {
    return ps2_read_data();
}

static void mouse_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    uint8_t byte = inb(PS2_DATA);
    irq_ack(12);

    /* Validate first byte: bit 3 must be set in a valid mouse packet */
    if (mouse_cycle == 0) {
        if (!(byte & 0x08)) {
            /* Not a valid packet start byte — reset and discard */
            mouse_cycle = 0;
            return;
        }
        mouse_bytes[0] = (int8_t)byte;
        mouse_cycle = 1;
        return;
    }

    /* Validate mid-packet bytes: 0xFA (ACK) or 0xAA (self-test passed)
     * arriving mid-sequence indicates the mouse state machine is out of sync. */
    if (byte == 0xFA || byte == 0xAA || byte == 0xFE) {
        /* Unexpected command response — reset packet accumulation */
        mouse_cycle = 0;
        return;
    }

    /* Accumulate packet bytes */
    if (mouse_cycle <= mouse_packet_size) {
        mouse_bytes[mouse_cycle] = (int8_t)byte;
        mouse_cycle++;
    }

    /* Process when we have a complete packet */
    if (mouse_cycle == mouse_packet_size) {
        mouse_cycle = 0;

        /* Decode packet (common layout: byte0=buttons, byte1=X, byte2=Y) */
        mouse_buttons = (uint8_t)(mouse_bytes[0] & 0x07);

        /* Decode 9-bit signed X/Y deltas.
         * Byte 0 bit 4 = X sign (9th bit), byte 0 bit 5 = Y sign.
         * Bytes 1/2 are the lower 8 bits (unsigned) of the 9-bit
         * two's complement value. */
        int dx = (uint8_t)mouse_bytes[1];
        int dy = (uint8_t)mouse_bytes[2];
        if (mouse_bytes[0] & 0x10) dx -= 256;
        if (mouse_bytes[0] & 0x20) dy -= 256;

        /* Overflow: ignore */
        if (mouse_bytes[0] & 0x40) dx = 0;
        if (mouse_bytes[0] & 0x80) dy = 0;

        /* Scroll wheel (4-byte IntelliMouse packets only) */
        if (mouse_packet_size == 4) {
            mouse_wheel_delta += (int8_t)mouse_bytes[3];
        }

        mouse_x += dx;
        mouse_y -= dy;

        if (mouse_x < 0) mouse_x = 0;
        if (mouse_x >= VGA_COLS) mouse_x = VGA_COLS - 1;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_y >= VGA_ROWS) mouse_y = VGA_ROWS - 1;

        mouse_px += dx * MOUSE_SENSITIVITY;
        mouse_py -= dy * MOUSE_SENSITIVITY;
        if (mouse_px < 0) mouse_px = 0;
        if (mouse_px >= FB_WIDTH)  mouse_px = FB_WIDTH  - 1;
        if (mouse_py < 0) mouse_py = 0;
        if (mouse_py >= FB_HEIGHT) mouse_py = FB_HEIGHT - 1;
    }
}

void __init mouse_init(void) {
    /* Enable auxiliary PS/2 device (mouse) */
    ps2_write_command(0xA8);

    /* Enable mouse interrupts: read controller config, set bit 1 (IRQ12) */
    ps2_write_command(0x20);
    uint8_t config = ps2_read_data();
    config |= 0x02;   /* enable IRQ12 */
    config &= ~0x20;  /* clear mouse disable bit */
    ps2_write_command(0x60);
    ps2_write_data(config);

    /* Reset mouse */
    mouse_write(0xFF);
    mouse_read(); /* ACK */
    mouse_read(); /* 0xAA self-test passed */
    mouse_id = mouse_read(); /* mouse ID: 0=standard, 3=IntelliMouse, 4=5-button */

    /* Set packet size based on detected mouse type */
    mouse_packet_size = (mouse_id == 0) ? 3 : 4;

    /* Set defaults */
    mouse_write(0xF6);
    mouse_read(); /* ACK */

    /* Enable IntelliMouse (scroll wheel) protocol:
       0xF3 200 = set sample rate 200
       0xF3 100 = set sample rate 100
       0xF3 80  = set sample rate 80
       After this sequence, the mouse uses 4-byte packets. */
    mouse_write(0xF3); mouse_read(); /* ACK */
    mouse_write(200);  mouse_read(); /* ACK */
    mouse_write(0xF3); mouse_read(); /* ACK */
    mouse_write(100);  mouse_read(); /* ACK */
    mouse_write(0xF3); mouse_read(); /* ACK */
    mouse_write(80);   mouse_read(); /* ACK */

    /* Re-read mouse ID after IntelliMouse activation sequence */
    mouse_write(0xF2); /* get mouse ID */
    mouse_read();      /* ACK */
    mouse_id = mouse_read(); /* 0x03 = IntelliMouse, 0x04 = 5-button */
    if (mouse_id >= 3) {
        mouse_packet_size = 4;
    }

    /* Enable data reporting */
    mouse_write(0xF4);
    mouse_read(); /* ACK */

    mouse_cycle = 0;

    /* Register handler for IRQ12 (vector 44) */
    idt_register_handler_named(44, mouse_irq_handler, "ps2_mouse");
    if (apic_is_init_complete()) {
        ioapic_unmask_irq(12);
    }
    pic_unmask(12);
}

void mouse_get_pos(int *x, int *y) {
    *x = mouse_x;
    *y = mouse_y;
}

void mouse_get_pixel_pos(int *x, int *y) {
    *x = mouse_px;
    *y = mouse_py;
}

uint8_t mouse_get_buttons(void) {
    return mouse_buttons;
}

int mouse_get_wheel(void) {
    int delta = mouse_wheel_delta;
    mouse_wheel_delta = 0;
    return delta;
}

/* ── Module exports (for doom.ko and other loadable modules) ────── */
#include "export.h"
EXPORT_SYMBOL(mouse_get_pixel_pos);
EXPORT_SYMBOL(mouse_get_buttons);
EXPORT_SYMBOL(mouse_get_wheel);
EXPORT_SYMBOL(mouse_init);

/* ── Open mouse device ──────────────────────────────── */
static int mouse_open(void *dev)
{
    (void)dev;
    return 0;
}

/* ── Close mouse device ─────────────────────────────── */
static int mouse_close(void *dev)
{
    (void)dev;
    return 0;
}

/* ── Mouse ioctl ────────────────────────────────────── */
static int mouse_ioctl(int cmd, void *arg)
{
    (void)cmd;
    (void)arg;
    return -ENOTTY;
}
