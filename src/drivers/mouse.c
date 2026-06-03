#include "mouse.h"
#include "ps2.h"
#include "io.h"
#include "idt.h"
#include "pic.h"
#include "apic.h"
#include "printf.h"

#define VGA_COLS 80
#define VGA_ROWS 25

static int mouse_x = 40;
static int mouse_y = 12;
static uint8_t mouse_buttons = 0;

/* Pixel-space position for framebuffer GUI (1024x768) */
static int mouse_px = 512;
static int mouse_py = 384;
#define FB_WIDTH  1024
#define FB_HEIGHT 768
#define MOUSE_SENSITIVITY 4

/* 4-byte packet accumulator (IntelliMouse: 4 bytes) */
static uint8_t mouse_cycle = 0;
static int8_t  mouse_bytes[4];

/* Scroll wheel */
static int mouse_wheel_delta = 0;

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

    switch (mouse_cycle) {
        case 0:
            /* First byte: buttons + overflow bits */
            if (!(byte & 0x08)) { mouse_cycle = 0; return; }
            mouse_bytes[0] = (int8_t)byte;
            mouse_cycle = 1;
            break;
        case 1:
            mouse_bytes[1] = (int8_t)byte;
            mouse_cycle = 2;
            break;
        case 2:
            mouse_bytes[2] = (int8_t)byte;
            mouse_cycle = 3;
            break;
        case 3:
            mouse_bytes[3] = (int8_t)byte;
            mouse_cycle = 0;

            /* Decode 4-byte IntelliMouse packet */
            mouse_buttons = (uint8_t)(mouse_bytes[0] & 0x07);

            int dx = mouse_bytes[1];
            int dy = mouse_bytes[2];

            /* Sign extension from packet flags */
            if (mouse_bytes[0] & 0x10) dx |= ~0xFF;
            if (mouse_bytes[0] & 0x20) dy |= ~0xFF;

            /* Overflow: ignore */
            if (mouse_bytes[0] & 0x40) dx = 0;
            if (mouse_bytes[0] & 0x80) dy = 0;

            /* Scroll wheel is in byte 3 (signed) */
            mouse_wheel_delta += (int8_t)mouse_bytes[3];

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
            break;
    }
}

void mouse_init(void) {
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
    mouse_read(); /* 0x00 mouse ID */

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
