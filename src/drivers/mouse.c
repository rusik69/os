#include "mouse.h"
#include "io.h"
#include "idt.h"
#include "pic.h"
#include "printf.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

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

/* 3-byte packet accumulator */
static uint8_t mouse_cycle = 0;
static int8_t  mouse_bytes[3];

static void ps2_wait_write(void) {
    uint32_t timeout = 100000;
    while (timeout-- && (inb(PS2_STATUS) & 0x02));
}

static void ps2_wait_read(void) {
    uint32_t timeout = 100000;
    while (timeout-- && !(inb(PS2_STATUS) & 0x01));
}

static void ps2_write_cmd(uint8_t cmd) {
    ps2_wait_write();
    outb(PS2_CMD, cmd);
}

static void ps2_write_data(uint8_t data) {
    ps2_wait_write();
    outb(PS2_DATA, data);
}

static uint8_t ps2_read_data(void) {
    ps2_wait_read();
    return inb(PS2_DATA);
}

static void mouse_write(uint8_t cmd) {
    ps2_write_cmd(0xD4);  /* next byte goes to mouse */
    ps2_write_data(cmd);
}

static uint8_t mouse_read(void) {
    return ps2_read_data();
}

static void mouse_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    uint8_t byte = inb(PS2_DATA);

    switch (mouse_cycle) {
        case 0:
            /* First byte: buttons + overflow bits */
            /* Verify bit 3 is always set (synchronisation bit) */
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
            mouse_cycle = 0;

            /* decode */
            mouse_buttons = (uint8_t)(mouse_bytes[0] & 0x07);

            int dx = mouse_bytes[1];
            int dy = mouse_bytes[2];

            /* Apply sign extension from packet flags */
            if (mouse_bytes[0] & 0x10) dx |= ~0xFF;
            if (mouse_bytes[0] & 0x20) dy |= ~0xFF;

            /* Overflow: ignore */
            if (mouse_bytes[0] & 0x40) dx = 0;
            if (mouse_bytes[0] & 0x80) dy = 0;

            mouse_x += dx;
            mouse_y -= dy;  /* screen Y is inverted vs PS/2 dy */

            if (mouse_x < 0) mouse_x = 0;
            if (mouse_x >= VGA_COLS) mouse_x = VGA_COLS - 1;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_y >= VGA_ROWS) mouse_y = VGA_ROWS - 1;

            /* Pixel-space tracking */
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
    ps2_write_cmd(0xA8);

    /* Enable mouse interrupts: read controller config, set bit 1 (IRQ12) */
    ps2_write_cmd(0x20);
    uint8_t config = ps2_read_data();
    config |= 0x02;   /* enable IRQ12 */
    config &= ~0x20;  /* clear mouse disable bit */
    ps2_write_cmd(0x60);
    ps2_write_data(config);

    /* Reset mouse */
    mouse_write(0xFF);
    mouse_read(); /* ACK */
    mouse_read(); /* 0xAA self-test passed */
    mouse_read(); /* 0x00 mouse ID */

    /* Set defaults */
    mouse_write(0xF6);
    mouse_read(); /* ACK */

    /* Enable data reporting */
    mouse_write(0xF4);
    mouse_read(); /* ACK */

    mouse_cycle = 0;

    /* Register handler for IRQ12 (vector 44) */
    idt_register_handler(44, mouse_irq_handler);
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
