#include "vga.h"
#include "io.h"
#include "string.h"

static uint16_t vga_row;
static uint16_t vga_col;
static uint8_t  vga_color;

static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

static void vga_update_cursor(void) {
    uint16_t pos = vga_row * VGA_WIDTH + vga_col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void vga_scroll(void) {
    if (vga_row < VGA_HEIGHT) return;

    for (int i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++) {
        VGA_MEMORY[i] = VGA_MEMORY[i + VGA_WIDTH];
    }
    for (int i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++) {
        VGA_MEMORY[i] = vga_entry(' ', vga_color);
    }
    vga_row = VGA_HEIGHT - 1;
}

void vga_init(void) {
    vga_row = 0;
    vga_col = 0;
    vga_color = VGA_LIGHT_GREY | (VGA_BLACK << 4);
    vga_clear();
}

void vga_clear(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_MEMORY[i] = vga_entry(' ', vga_color);
    }
    vga_row = 0;
    vga_col = 0;
    vga_update_cursor();
}

void vga_set_color(uint8_t fg, uint8_t bg) {
    vga_color = fg | (bg << 4);
}

void vga_putchar(char c) {
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (c == '\r') {
        vga_col = 0;
    } else if (c == '\t') {
        vga_col = (vga_col + 8) & ~7;
    } else if (c == '\b') {
        if (vga_col > 0) {
            vga_col--;
            VGA_MEMORY[vga_row * VGA_WIDTH + vga_col] = vga_entry(' ', vga_color);
        }
    } else {
        VGA_MEMORY[vga_row * VGA_WIDTH + vga_col] = vga_entry(c, vga_color);
        vga_col++;
    }

    if (vga_col >= VGA_WIDTH) {
        vga_col = 0;
        vga_row++;
    }

    vga_scroll();
    vga_update_cursor();
}

void vga_write(const char *str) {
    while (*str) vga_putchar(*str++);
}

void vga_set_cursor(uint16_t row, uint16_t col) {
    vga_row = row;
    vga_col = col;
    vga_update_cursor();
}

void vga_get_cursor(uint16_t *row, uint16_t *col) {
    *row = vga_row;
    *col = vga_col;
}

void vga_put_entry_at(char c, uint8_t color, uint16_t row, uint16_t col) {
    VGA_MEMORY[row * VGA_WIDTH + col] = (uint16_t)c | ((uint16_t)color << 8);
}
