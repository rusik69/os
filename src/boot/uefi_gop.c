/*
 * uefi_gop.c — UEFI Graphics Output Protocol framebuffer setup
 *
 * Reads the UEFI GOP framebuffer info passed from the bootloader (via
 * multiboot2 tag or custom boot params) and registers it with the
 * fbcon subsystem as the kernel's linear framebuffer console.
 *
 * Item S156: UEFI GOP framebuffer
 */

#include "types.h"
#include "printf.h"
#include "fbcon.h"
#include "vga.h"
#include "string.h"

/* GOP framebuffer info structure passed from bootloader */
struct uefi_gop_info {
    uint64_t fb_addr;       /* Physical address of linear framebuffer */
    uint32_t fb_size;       /* Size of framebuffer in bytes */
    uint32_t width;         /* Horizontal resolution in pixels */
    uint32_t height;        /* Vertical resolution in pixels */
    uint32_t pitch;         /* Bytes per scanline */
    uint8_t  bpp;           /* Bits per pixel (typically 32) */
    uint8_t  pixel_format;  /* 0=RGB, 1=BGR, 2=Bitmap, 3=Grayscale */
} __attribute__((packed));

/* The GOP info passed from the boot stub (weak default = none) */
static struct uefi_gop_info g_gop_info;
static int g_gop_valid = 0;

/*
 * Parse GOP info from a multiboot2 framebuffer tag or a flat structure
 * at the given physical address.  Called early in boot.
 *
 * If @info_phys is non-zero, interpret it as a physical address pointing
 * to a struct uefi_gop_info.  Otherwise, try to locate GOP info via
 * the multiboot2 tag by scanning the multiboot info structure at
 * @mboot_info_phys.
 */
int uefi_gop_init(uint64_t mboot_info_phys, uint64_t info_phys)
{
    memset(&g_gop_info, 0, sizeof(g_gop_info));
    g_gop_valid = 0;

    if (info_phys) {
        /* Flat GOP info structure provided directly */
        memcpy(&g_gop_info, (void*)(uintptr_t)info_phys, sizeof(g_gop_info));
        if (g_gop_info.fb_addr && g_gop_info.width > 0 && g_gop_info.height > 0) {
            g_gop_valid = 1;
            goto register_fb;
        }
    }

    if (mboot_info_phys) {
        /* Scan multiboot2 tags for framebuffer tag (type=8) */
        uint64_t addr = mboot_info_phys;
        /* Multiboot2 info starts with total_size (4) + reserved (4) */
        uint32_t total_size = *(uint32_t*)(uintptr_t)addr;
        uint32_t offset = 8; /* skip header */

        while (offset + 8 < total_size) {
            uint16_t tag_type = *(uint16_t*)(uintptr_t)(addr + offset);
            uint16_t tag_size = *(uint16_t*)(uintptr_t)(addr + offset + 2);

            if (tag_type == 0) /* end tag */
                break;

            if (tag_type == 8) { /* framebuffer tag */
                if (tag_size < 24)
                    goto skip_tag;

                /* Multiboot2 framebuffer tag layout:
                   uint32_t fb_addr_low;
                   uint32_t fb_addr_high;
                   uint32_t fb_pitch;
                   uint32_t fb_width;
                   uint32_t fb_height;
                   uint8_t  fb_bpp;
                   uint8_t  fb_type; // 0=RGB, 1=text, 2=Grayscale
                */
                uint8_t *tag_data = (uint8_t*)(uintptr_t)(addr + offset + 8);
                uint64_t fb_addr = *(uint32_t*)(tag_data);
                fb_addr |= ((uint64_t)*(uint32_t*)(tag_data + 4)) << 32;

                g_gop_info.fb_addr      = fb_addr;
                g_gop_info.fb_size      = *(uint32_t*)(tag_data + 8);
                g_gop_info.width        = *(uint32_t*)(tag_data + 12);
                g_gop_info.height       = *(uint32_t*)(tag_data + 16);
                g_gop_info.pitch        = *(uint32_t*)(tag_data + 20);
                g_gop_info.bpp          = *(uint8_t*)(tag_data + 24);
                g_gop_info.pixel_format = *(uint8_t*)(tag_data + 25);

                if (g_gop_info.fb_addr && g_gop_info.width > 0 && g_gop_info.height > 0) {
                    g_gop_valid = 1;
                    break;
                }
            }

        skip_tag:
            /* Align to 8 bytes */
            offset += (tag_size + 7) & ~7;
        }
    }

register_fb:
    if (!g_gop_valid) {
        kprintf("[GOP] No framebuffer info found\n");
        return -1;
    }

    kprintf("[GOP] Framebuffer: %dx%d %dbpp pitch=%d addr=%p size=%u\n",
            g_gop_info.width, g_gop_info.height, g_gop_info.bpp,
            g_gop_info.pitch, (void*)(uintptr_t)g_gop_info.fb_addr,
            g_gop_info.fb_size);

    /* Register as fbcon device */
    fbcon_init((uint32_t*)(uintptr_t)g_gop_info.fb_addr,
               g_gop_info.width, g_gop_info.height,
               g_gop_info.pitch);

    return 0;
}

/* Return whether GOP framebuffer is active */
int uefi_gop_is_active(void)
{
    return g_gop_valid;
}

/* Return GOP framebuffer info */
void uefi_gop_get_info(uint64_t *fb_addr, uint32_t *width,
                       uint32_t *height, uint32_t *pitch)
{
    if (fb_addr) *fb_addr = g_gop_info.fb_addr;
    if (width)   *width   = g_gop_info.width;
    if (height)  *height  = g_gop_info.height;
    if (pitch)   *pitch   = g_gop_info.pitch;
}
