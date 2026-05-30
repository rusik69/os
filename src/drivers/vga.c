#include "vga.h"
#include "io.h"
#include "string.h"
#include "vmm.h"
#include "printf.h"
#include "heap.h"
#include "pci.h"

#define FB_CELL_W 8
#define FB_CELL_H 16

/* Bochs/QEMU stdvga DISPI (portable VBE) */
#define VBE_DISPI_INDEX         0x01CE
#define VBE_DISPI_DATA          0x01CF
#define VBE_DISPI_ID            0xB0C5
#define VBE_DISPI_INDEX_ID      0x0
#define VBE_DISPI_INDEX_XRES    0x1
#define VBE_DISPI_INDEX_YRES    0x2
#define VBE_DISPI_INDEX_BPP     0x3
#define VBE_DISPI_INDEX_ENABLE  0x4
#define VBE_DISPI_INDEX_VIRT_W  0x6
#define VBE_DISPI_ENABLED       0x01
#define VBE_DISPI_LFB_ENABLED   0x40
#define VBE_DISPI_LFB_PHYS      0xE0000000ULL

/* VBE 32bpp X8R8G8B8 (stored little-endian as B,G,R,X in memory) */
static inline uint32_t vga_pack_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

struct vbe_mode_info {
    uint16_t mode_attributes;
    uint8_t  win_a_attributes;
    uint8_t  win_b_attributes;
    uint16_t win_granularity;
    uint16_t win_size;
    uint16_t win_a_segment;
    uint16_t win_b_segment;
    uint32_t win_func_ptr;
    uint16_t pitch;
    uint16_t width;
    uint16_t height;
    uint8_t  char_width;
    uint8_t  char_height;
    uint8_t  planes;
    uint8_t  bits_per_pixel;
    uint8_t  banks;
    uint8_t  memory_model;
    uint8_t  bank_size;
    uint8_t  image_pages;
    uint8_t  reserved1;
    uint8_t  red_mask_size;
    uint8_t  red_field_position;
    uint8_t  green_mask_size;
    uint8_t  green_field_position;
    uint8_t  blue_mask_size;
    uint8_t  blue_field_position;
    uint8_t  reserved_mask_size;
    uint8_t  reserved_field_position;
    uint8_t  direct_color_mode_info;
    uint32_t phys_base;
    uint32_t offscreen_memory_offset;
    uint16_t offscreen_memory_size;
    uint8_t  reserved2[206];
} __attribute__((packed));

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

static void vbe_write(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_INDEX, index);
    outw(VBE_DISPI_DATA, value);
}

static uint16_t vbe_read(uint16_t index) {
    outw(VBE_DISPI_INDEX, index);
    return inw(VBE_DISPI_DATA);
}

static uint64_t pci_bar_mem_addr(uint32_t bar_lo, uint32_t bar_hi) {
    if (bar_lo & 1)
        return 0;
    if (bar_lo & 0x4)
        return ((uint64_t)bar_hi << 32) | (bar_lo & 0xFFFFFFF0ULL);
    return bar_lo & 0xFFFFFFF0ULL;
}

/* QEMU -vga std (PCI): framebuffer is BAR0, not 0xE0000000 (ISA only). */
static uint64_t vga_lookup_pci_fb(void) {
    struct pci_device vga;

    if (pci_find_device(0x1234, 0x1111, &vga) != 0 &&
        pci_find_class(0x03, 0x00, &vga) != 0)
        return 0;

    uint32_t cmd = pci_read(vga.bus, vga.slot, vga.func, 0x04);
    pci_write(vga.bus, vga.slot, vga.func, 0x04, cmd | (1u << 1)); /* mem space */

    uint32_t bar0 = pci_read(vga.bus, vga.slot, vga.func, 0x10);
    uint32_t bar1 = pci_read(vga.bus, vga.slot, vga.func, 0x14);
    uint64_t addr = pci_bar_mem_addr(bar0, bar1);

    /* QEMU stdvga BAR2 MMIO: set little-endian scanout (spec offset 0x604) */
    uint32_t bar2 = pci_read(vga.bus, vga.slot, vga.func, 0x18);
    uint64_t mmio = pci_bar_mem_addr(bar2, 0);
    if (mmio) {
        volatile uint32_t *end_reg = (volatile uint32_t *)(uintptr_t)(mmio + 0x604);
        *end_reg = 0x1e1e1e1e;
        __asm__ volatile("mfence" ::: "memory");
    }

    return addr;
}

static int vga_fb_selftest(void) {
    if (!fb_base || fb_bpp != 32) return -1;
    volatile uint32_t *p = (volatile uint32_t *)fb_base;
    uint32_t saved = p[0];
    uint32_t probe = 0x00AABBCC;
    p[0] = probe;
    __asm__ volatile("mfence" ::: "memory");
    uint32_t got = p[0];
    p[0] = saved;
    __asm__ volatile("mfence" ::: "memory");
    return got == probe ? 0 : -1;
}

static int vga_try_bochs_vbe(uint32_t width, uint32_t height, uint8_t bpp) {
    uint16_t id = vbe_read(VBE_DISPI_INDEX_ID);
    if (id != VBE_DISPI_ID && id != 0xB0C4)
        return -1;

    vbe_write(VBE_DISPI_INDEX_ENABLE, 0);
    vbe_write(VBE_DISPI_INDEX_XRES, (uint16_t)width);
    vbe_write(VBE_DISPI_INDEX_YRES, (uint16_t)height);
    vbe_write(VBE_DISPI_INDEX_BPP, bpp);
    vbe_write(VBE_DISPI_INDEX_VIRT_W, (uint16_t)width);
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    uint64_t fb_addr = vga_lookup_pci_fb();
    if (!fb_addr)
        fb_addr = VBE_DISPI_LFB_PHYS;

    fb_base = (volatile uint8_t *)(uintptr_t)fb_addr;
    fb_pitch = width * (bpp / 8);
    fb_width = width;
    fb_height = height;
    fb_bpp = bpp;
    fb_active = 1;

    vmm_set_range_uncacheable(fb_addr, (uint64_t)fb_pitch * height);
    return 0;
}

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
    if (!fb_active || !fb_base) return;
    if (fb_bpp == 32) {
        for (uint32_t y = 0; y < fb_height; y++) {
            volatile uint32_t *row = (volatile uint32_t *)(fb_base + y * fb_pitch);
            for (uint32_t x = 0; x < fb_width; x++)
                row[x] = rgb;
        }
    } else {
        for (uint32_t y = 0; y < fb_height; y++) {
            for (uint32_t x = 0; x < fb_width; x++)
                fb_put_pixel(x, y, rgb);
        }
    }
    __asm__ volatile("mfence" ::: "memory");
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
    
    uint64_t fb_addr = mbi->framebuffer_addr;
    uint32_t fb_w = mbi->framebuffer_width;
    uint32_t fb_h = mbi->framebuffer_height;
    uint32_t fb_pitch_val = mbi->framebuffer_pitch;
    uint8_t fb_bpp_val = mbi->framebuffer_bpp;
    uint8_t fb_type = mbi->framebuffer_type;
    
    /* If framebuffer_addr is 0, try to read from VBE mode info */
    if (!fb_addr && mbi->vbe_mode_info) {
        struct vbe_mode_info *vmi = (struct vbe_mode_info *)PHYS_TO_VIRT((uint32_t)mbi->vbe_mode_info);
        fb_addr = (uint64_t)vmi->phys_base;
        fb_w = vmi->width;
        fb_h = vmi->height;
        fb_pitch_val = vmi->pitch;
        fb_bpp_val = vmi->bits_per_pixel;
        fb_type = 1;  /* Assume RGB */
        kprintf("[..] Using VBE mode info: addr=0x%x %ux%u bpp=%d pitch=%u\n",
                (unsigned)fb_addr, fb_w, fb_h, fb_bpp_val, fb_pitch_val);
    }
    
    /* If still no framebuffer, return failure - will try to allocate later */
    if (!fb_addr) {
        return -1;
    }
    
    kprintf("[..] Framebuffer: addr=0x%x type=%d bpp=%d %ux%u pitch=%u\n",
            (unsigned)fb_addr, fb_type,
            fb_bpp_val, fb_w, fb_h,
            fb_pitch_val);
    
    if (fb_type != 1) {
        kprintf("[--] Framebuffer type %d (need 1 = RGB)\n", fb_type);
        return -1;
    }
    if (fb_bpp_val != 24 && fb_bpp_val != 32) {
        kprintf("[--] Framebuffer BPP %d (need 24 or 32)\n", fb_bpp_val);
        return -1;
    }
    if (fb_w < VGA_WIDTH * FB_CELL_W ||
        fb_h < VGA_HEIGHT * FB_CELL_H) {
        kprintf("[--] Framebuffer too small: %ux%u (need at least %ux%u)\n",
                fb_w, fb_h,
                VGA_WIDTH * FB_CELL_W, VGA_HEIGHT * FB_CELL_H);
        return -1;
    }

    uint64_t fb_size = (uint64_t)fb_pitch_val * fb_h;
    
    /* Map framebuffer in high-half VMA space */
    fb_base = (volatile uint8_t *)vmm_map_phys(fb_addr, fb_size,
                VMM_FLAG_PRESENT | VMM_FLAG_WRITE);

    if (!fb_base) {
        kprintf("[--] Failed to map framebuffer at 0x%x\n", (unsigned)fb_addr);
        return -1;
    }

    fb_pitch = fb_pitch_val;
    fb_width = fb_w;
    fb_height = fb_h;
    fb_bpp = fb_bpp_val;
    fb_active = 1;

    if (fb_addr >= 0xC0000000ULL)
        vmm_set_range_uncacheable((uint64_t)(uintptr_t)fb_base, fb_size);

    clear_framebuffer(vga_palette[(vga_color >> 4) & 0x0F]);
    render_all();
    return 0;
}

int vga_is_framebuffer(void) {
    return fb_active;
}

int vga_try_alloc_software_framebuffer(void) {
    if (fb_active)
        return 0;

    /* Prefer Bochs VBE — QEMU -vga std scans out PCI BAR0 framebuffer */
    if (vga_try_bochs_vbe(1024, 768, 32) == 0) {
        if (vga_fb_selftest() != 0)
            kprintf("[--] Framebuffer write/read failed at 0x%x\n",
                    (unsigned)(uintptr_t)fb_base);
        kprintf("[OK] Bochs VBE framebuffer: 1024x768x32 at 0x%x\n",
                (unsigned)(uintptr_t)fb_base);
        clear_framebuffer(vga_palette[(vga_color >> 4) & 0x0F]);
        render_all();
        return 0;
    }

    /* Fallback: heap buffer (visible only with hardware multiboot FB) */
    uint32_t new_width = 1024;
    uint32_t new_height = 768;
    uint8_t new_bpp = 32;
    uint32_t new_pitch = new_width * (new_bpp / 8);
    uint64_t new_size = (uint64_t)new_pitch * new_height;

    uint8_t *new_fb = (uint8_t *)kmalloc(new_size);
    if (!new_fb) {
        kprintf("[--] Failed to allocate software framebuffer (%u KB)\n",
                (unsigned)(new_size / 1024));
        return -1;
    }

    kprintf("[--] Using off-screen framebuffer at 0x%x (no Bochs VBE)\n",
            (unsigned)(uint64_t)new_fb);

    fb_base = (volatile uint8_t *)new_fb;
    fb_pitch = new_pitch;
    fb_width = new_width;
    fb_height = new_height;
    fb_bpp = new_bpp;
    fb_active = 1;

    clear_framebuffer(vga_palette[(vga_color >> 4) & 0x0F]);
    render_all();
    return 0;
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

/* ===== Framebuffer Graphics API ===== */

void vga_put_pixel(int32_t x, int32_t y, uint32_t color) {
    if (!fb_base || x < 0 || y < 0 || x >= (int32_t)fb_width || y >= (int32_t)fb_height) {
        return;
    }

    uint64_t offset = (uint64_t)y * fb_pitch + x * (fb_bpp / 8);
    if (fb_bpp == 32) {
        volatile uint32_t *pixel = (volatile uint32_t *)(fb_base + offset);
        *pixel = color;
    } else if (fb_bpp == 24) {
        volatile uint8_t *pixel = (volatile uint8_t *)(fb_base + offset);
        pixel[0] = (color) & 0xFF;
        pixel[1] = (color >> 8) & 0xFF;
        pixel[2] = (color >> 16) & 0xFF;
    }
}

void vga_clear_framebuffer(uint32_t color) {
    if (!fb_base) return;

    if (fb_bpp == 32) {
        for (uint32_t y = 0; y < fb_height; y++) {
            volatile uint32_t *row = (volatile uint32_t *)(fb_base + y * fb_pitch);
            for (uint32_t x = 0; x < fb_width; x++)
                row[x] = color;
        }
    } else {
        for (uint32_t y = 0; y < fb_height; y++) {
            for (uint32_t x = 0; x < fb_width; x++)
                vga_put_pixel(x, y, color);
        }
    }
    __asm__ volatile("mfence" ::: "memory");
}

void vga_refresh_console(void) {
    if (!fb_active) return;
    render_all();
    vga_update_cursor();
}

void vga_get_framebuffer_ptr(uint8_t **ptr, uint32_t *width, uint32_t *height, uint32_t *pitch) {
    if (ptr) *ptr = (uint8_t *)fb_base;
    if (width) *width = fb_width;
    if (height) *height = fb_height;
    if (pitch) *pitch = fb_pitch;
}
