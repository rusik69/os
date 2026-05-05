#ifndef VGA_H
#define VGA_H

#include "types.h"

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY ((volatile uint16_t *)PHYS_TO_VIRT(0xB8000))

enum vga_color {
    VGA_BLACK = 0,
    VGA_BLUE = 1,
    VGA_GREEN = 2,
    VGA_CYAN = 3,
    VGA_RED = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN = 6,
    VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8,
    VGA_LIGHT_BLUE = 9,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW = 14,
    VGA_WHITE = 15,
};

void vga_init(void);
int vga_try_init_framebuffer(uint64_t multiboot_info_phys);
int vga_is_framebuffer(void);
void vga_get_framebuffer_info(uint32_t *width, uint32_t *height,
                              uint32_t *pitch, uint8_t *bpp);
void vga_clear(void);
void vga_putchar(char c);
void vga_write(const char *str);
void vga_set_color(uint8_t fg, uint8_t bg);
void vga_set_cursor(uint16_t row, uint16_t col);
void vga_get_cursor(uint16_t *row, uint16_t *col);
void vga_put_entry_at(char c, uint8_t color, uint16_t row, uint16_t col);

#endif
