#include "vga.h"
#include "io.h"
#include "string.h"
#include "vmm.h"

#define FB_CELL_W 8
#define FB_CELL_H 16

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint8_t color_info[6];
} __attribute__((packed));

static uint16_t vga_row;
static uint16_t vga_col;
static uint8_t  vga_color;
static uint16_t vga_cells[VGA_WIDTH * VGA_HEIGHT];

static volatile uint8_t *fb_base;
static uint32_t fb_pitch;
static uint32_t fb_width;
static uint32_t fb_height;
static uint8_t fb_bpp;
static int fb_active;

static const uint32_t vga_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)(uint8_t)c | ((uint16_t)color << 8);
}

static const uint8_t *font5x7(char ch) {
    static const uint8_t blank[7] = {0,0,0,0,0,0,0};
    static const uint8_t unknown[7] = {0x0E,0x11,0x01,0x02,0x04,0x00,0x04};
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    switch (ch) {
    case 'A': { static const uint8_t g[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}; return g; }
    case 'B': { static const uint8_t g[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}; return g; }
    case 'C': { static const uint8_t g[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}; return g; }
    case 'D': { static const uint8_t g[7] = {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}; return g; }
    case 'E': { static const uint8_t g[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}; return g; }
    case 'F': { static const uint8_t g[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}; return g; }
    case 'G': { static const uint8_t g[7] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}; return g; }
    case 'H': { static const uint8_t g[7] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}; return g; }
    case 'I': { static const uint8_t g[7] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}; return g; }
    case 'J': { static const uint8_t g[7] = {0x07,0x02,0x02,0x02,0x12,0x12,0x0C}; return g; }
    case 'K': { static const uint8_t g[7] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11}; return g; }
    case 'L': { static const uint8_t g[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}; return g; }
    case 'M': { static const uint8_t g[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}; return g; }
    case 'N': { static const uint8_t g[7] = {0x11,0x11,0x19,0x15,0x13,0x11,0x11}; return g; }
    case 'O': { static const uint8_t g[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}; return g; }
    case 'P': { static const uint8_t g[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}; return g; }
    case 'Q': { static const uint8_t g[7] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}; return g; }
    case 'R': { static const uint8_t g[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}; return g; }
    case 'S': { static const uint8_t g[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}; return g; }
    case 'T': { static const uint8_t g[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}; return g; }
    case 'U': { static const uint8_t g[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}; return g; }
    case 'V': { static const uint8_t g[7] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}; return g; }
    case 'W': { static const uint8_t g[7] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}; return g; }
    case 'X': { static const uint8_t g[7] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}; return g; }
    case 'Y': { static const uint8_t g[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}; return g; }
    case 'Z': { static const uint8_t g[7] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}; return g; }
    case '0': { static const uint8_t g[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; return g; }
    case '1': { static const uint8_t g[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}; return g; }
    case '2': { static const uint8_t g[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}; return g; }
    case '3': { static const uint8_t g[7] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}; return g; }
    case '4': { static const uint8_t g[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; return g; }
    case '5': { static const uint8_t g[7] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}; return g; }
    case '6': { static const uint8_t g[7] = {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}; return g; }
    case '7': { static const uint8_t g[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; return g; }
    case '8': { static const uint8_t g[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; return g; }
    case '9': { static const uint8_t g[7] = {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}; return g; }
    case ' ': return blank;
    case '-': { static const uint8_t g[7] = {0,0,0,0x1F,0,0,0}; return g; }
    case '_': { static const uint8_t g[7] = {0,0,0,0,0,0,0x1F}; return g; }
    case '.': { static const uint8_t g[7] = {0,0,0,0,0,0x0C,0x0C}; return g; }
    case ',': { static const uint8_t g[7] = {0,0,0,0,0,0x0C,0x08}; return g; }
    case ':': { static const uint8_t g[7] = {0,0x0C,0x0C,0,0x0C,0x0C,0}; return g; }
    case ';': { static const uint8_t g[7] = {0,0x0C,0x0C,0,0x0C,0x0C,0x08}; return g; }
    case '!': { static const uint8_t g[7] = {0x04,0x04,0x04,0x04,0x04,0,0x04}; return g; }
    case '?': return unknown;
    case '/': { static const uint8_t g[7] = {0x01,0x02,0x02,0x04,0x08,0x08,0x10}; return g; }
    case '\\': { static const uint8_t g[7] = {0x10,0x08,0x08,0x04,0x02,0x02,0x01}; return g; }
    case '(': { static const uint8_t g[7] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02}; return g; }
    case ')': { static const uint8_t g[7] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08}; return g; }
    case '[': { static const uint8_t g[7] = {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}; return g; }
    case ']': { static const uint8_t g[7] = {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}; return g; }
    case '<': { static const uint8_t g[7] = {0x02,0x04,0x08,0x10,0x08,0x04,0x02}; return g; }
    case '>': { static const uint8_t g[7] = {0x08,0x04,0x02,0x01,0x02,0x04,0x08}; return g; }
    case '=': { static const uint8_t g[7] = {0,0x1F,0,0,0x1F,0,0}; return g; }
    case '+': { static const uint8_t g[7] = {0,0x04,0x04,0x1F,0x04,0x04,0}; return g; }
    case '*': { static const uint8_t g[7] = {0,0x11,0x0A,0x04,0x0A,0x11,0}; return g; }
    case '|': { static const uint8_t g[7] = {0x04,0x04,0x04,0x04,0x04,0x04,0x04}; return g; }
    case '#': { static const uint8_t g[7] = {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}; return g; }
    case '"': { static const uint8_t g[7] = {0x0A,0x0A,0x04,0,0,0,0}; return g; }
    case '\'': { static const uint8_t g[7] = {0x04,0x04,0x02,0,0,0,0}; return g; }
    case '%': { static const uint8_t g[7] = {0x19,0x19,0x02,0x04,0x08,0x13,0x13}; return g; }
    default:
        return unknown;
    }
}

static void vga_update_cursor(void) {
    if (fb_active) return;
    uint16_t pos = vga_row * VGA_WIDTH + vga_col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void fb_put_pixel(uint32_t x, uint32_t y, uint32_t rgb) {
    if (!fb_active || x >= fb_width || y >= fb_height) return;
    volatile uint8_t *p = fb_base + y * fb_pitch + x * (fb_bpp / 8);
    if (fb_bpp == 32) {
        *(volatile uint32_t *)p = rgb;
    } else if (fb_bpp == 24) {
        p[0] = rgb & 0xFF;
        p[1] = (rgb >> 8) & 0xFF;
        p[2] = (rgb >> 16) & 0xFF;
    }
}

static void render_cell(uint16_t row, uint16_t col) {
    uint16_t entry = vga_cells[row * VGA_WIDTH + col];
    char ch = (char)(entry & 0xFF);
    uint8_t color = (entry >> 8) & 0xFF;

    if (!fb_active) {
        VGA_MEMORY[row * VGA_WIDTH + col] = entry;
        return;
    }

    uint32_t fg = vga_palette[color & 0x0F];
    uint32_t bg = vga_palette[(color >> 4) & 0x0F];
    uint32_t x0 = col * FB_CELL_W;
    uint32_t y0 = row * FB_CELL_H;
    const uint8_t *glyph = font5x7(ch);

    for (uint32_t y = 0; y < FB_CELL_H; y++) {
        for (uint32_t x = 0; x < FB_CELL_W; x++) {
            fb_put_pixel(x0 + x, y0 + y, bg);
        }
    }

    for (uint32_t gy = 0; gy < 7; gy++) {
        uint8_t bits = glyph[gy];
        uint32_t py = y0 + 1 + gy * 2;
        for (uint32_t gx = 0; gx < 5; gx++) {
            if (!(bits & (1u << (4 - gx)))) continue;
            uint32_t px = x0 + 1 + gx;
            fb_put_pixel(px, py, fg);
            fb_put_pixel(px, py + 1, fg);
        }
    }
}

static void render_all(void) {
    for (uint16_t row = 0; row < VGA_HEIGHT; row++) {
        for (uint16_t col = 0; col < VGA_WIDTH; col++) {
            render_cell(row, col);
        }
    }
}

static void clear_framebuffer(uint32_t rgb) {
    if (!fb_active) return;
    for (uint32_t y = 0; y < fb_height; y++) {
        for (uint32_t x = 0; x < fb_width; x++) {
            fb_put_pixel(x, y, rgb);
        }
    }
}

static void vga_scroll(void) {
    if (vga_row < VGA_HEIGHT) return;

    memmove(vga_cells, vga_cells + VGA_WIDTH,
            (VGA_HEIGHT - 1) * VGA_WIDTH * sizeof(uint16_t));
    for (int i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++) {
        vga_cells[i] = vga_entry(' ', vga_color);
    }
    vga_row = VGA_HEIGHT - 1;
    render_all();
}

void vga_init(void) {
    vga_row = 0;
    vga_col = 0;
    vga_color = VGA_LIGHT_GREY | (VGA_BLACK << 4);
    fb_base = 0;
    fb_pitch = 0;
    fb_width = 0;
    fb_height = 0;
    fb_bpp = 0;
    fb_active = 0;
    vga_clear();
}

int vga_try_init_framebuffer(uint64_t multiboot_info_phys) {
    struct multiboot_info *mbi = (struct multiboot_info *)PHYS_TO_VIRT(multiboot_info_phys);
    if (!(mbi->flags & (1u << 12))) return -1;
    if (mbi->framebuffer_type != 1) return -1;
    if (mbi->framebuffer_bpp != 24 && mbi->framebuffer_bpp != 32) return -1;
    if (mbi->framebuffer_width < VGA_WIDTH * FB_CELL_W ||
        mbi->framebuffer_height < VGA_HEIGHT * FB_CELL_H) return -1;

    uint64_t fb_addr = mbi->framebuffer_addr;
    uint64_t fb_size = (uint64_t)mbi->framebuffer_pitch * mbi->framebuffer_height;
    uint64_t start = fb_addr & ~(PAGE_SIZE - 1ULL);
    uint64_t end = (fb_addr + fb_size + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE)
        vmm_map_page(addr, addr, VMM_FLAG_PRESENT | VMM_FLAG_WRITE);

    fb_base = (volatile uint8_t *)(uintptr_t)fb_addr;
    fb_pitch = mbi->framebuffer_pitch;
    fb_width = mbi->framebuffer_width;
    fb_height = mbi->framebuffer_height;
    fb_bpp = mbi->framebuffer_bpp;
    fb_active = 1;
    clear_framebuffer(vga_palette[(vga_color >> 4) & 0x0F]);
    render_all();
    return 0;
}

int vga_is_framebuffer(void) {
    return fb_active;
}

void vga_get_framebuffer_info(uint32_t *width, uint32_t *height,
                              uint32_t *pitch, uint8_t *bpp) {
    if (width)  *width = fb_width;
    if (height) *height = fb_height;
    if (pitch)  *pitch = fb_pitch;
    if (bpp)    *bpp = fb_bpp;
}

void vga_clear(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_cells[i] = vga_entry(' ', vga_color);
    }
    vga_row = 0;
    vga_col = 0;
    if (fb_active)
        clear_framebuffer(vga_palette[(vga_color >> 4) & 0x0F]);
    render_all();
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
            vga_cells[vga_row * VGA_WIDTH + vga_col] = vga_entry(' ', vga_color);
            render_cell(vga_row, vga_col);
        }
    } else {
        vga_cells[vga_row * VGA_WIDTH + vga_col] = vga_entry(c, vga_color);
        render_cell(vga_row, vga_col);
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
    vga_cells[row * VGA_WIDTH + col] = vga_entry(c, color);
    render_cell(row, col);
}
