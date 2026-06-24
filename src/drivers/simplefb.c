/*
 * simplefb.c — Simple framebuffer driver for EFI/bootloader-provided fb
 *
 * This driver handles the "simple-framebuffer" device, which is a
 * pre-configured linear framebuffer set up by UEFI, coreboot, or
 * a bootloader (GRUB, Limine, etc.).  It provides minimal operations:
 * - Probe: validate the framebuffer parameters from boot info
 * - Setup: register the existing framebuffer as the fbcon device
 * - Info: expose resolution, format, and address to userspace
 *
 * The framebuffer configuration is typically retrieved from ACPI DSDT
 * ("BOOT0000" or "LNXVIDEO") or from Multiboot2 framebuffer tags.
 *
 * Item S160: Simple framebuffer
 */

#include "types.h"
#include "printf.h"
#include "fbcon.h"
#include "vga.h"       /* for vga_try_init_framebuffer compat */
#include "string.h"
#include "pmm.h"
#include "uio.h"

/* SimpleFB pixel format enum */
enum simplefb_format {
    SIMPLEFB_FORMAT_RGBX_8888 = 0,
    SIMPLEFB_FORMAT_BGRX_8888 = 1,
    SIMPLEFB_FORMAT_XRGB_8888 = 2,
    SIMPLEFB_FORMAT_XBGR_8888 = 3,
    SIMPLEFB_FORMAT_RGB_565   = 4,
    SIMPLEFB_FORMAT_UNKNOWN   = 0xFF,
};

/* SimpleFB device state */
struct simplefb_device {
    uint32_t *fb_addr;          /* Virtual address of framebuffer */
    uint32_t  width;            /* Visible width in pixels */
    uint32_t  height;           /* Visible height in pixels */
    uint32_t  stride;           /* Bytes per scanline */
    uint32_t  fb_size;          /* Total framebuffer size in bytes */
    enum simplefb_format format;
    int       registered;       /* Whether fbcon is using this */
};

static struct simplefb_device g_simplefb;

/* Format name lookup */
static const char *simplefb_format_name(enum simplefb_format fmt)
{
    switch (fmt) {
        case SIMPLEFB_FORMAT_RGBX_8888: return "RGBX-8888";
        case SIMPLEFB_FORMAT_BGRX_8888: return "BGRX-8888";
        case SIMPLEFB_FORMAT_XRGB_8888: return "XRGB-8888";
        case SIMPLEFB_FORMAT_XBGR_8888: return "XBGR-8888";
        case SIMPLEFB_FORMAT_RGB_565:   return "RGB-565";
        default:                        return "unknown";
    }
}

/*
 * simplefb_init — Register framebuffer from boot-provided parameters.
 *
 * @fb_addr:  physical address of the linear framebuffer
 * @width:    horizontal resolution in pixels
 * @height:   vertical resolution in pixels
 * @stride:   bytes per scanline (can be > width * bytes_per_pixel)
 * @format:   pixel format enum
 *
 * Returns 0 on success, -1 on invalid parameters.
 */
int simplefb_init(uint64_t fb_addr, uint32_t width, uint32_t height,
                  uint32_t stride, int format_enum)
{
    /* Validate parameters */
    if (!fb_addr || width == 0 || height == 0 || stride == 0) {
        kprintf("[simplefb] Invalid parameters: addr=%p w=%u h=%u stride=%u\n",
                (void*)(uintptr_t)fb_addr, width, height, stride);
        return -1;
    }

    /* Compute bpp from format */
    uint32_t bpp;
    switch (format_enum) {
        case SIMPLEFB_FORMAT_RGBX_8888:
        case SIMPLEFB_FORMAT_BGRX_8888:
        case SIMPLEFB_FORMAT_XRGB_8888:
        case SIMPLEFB_FORMAT_XBGR_8888:
            bpp = 32;
            break;
        case SIMPLEFB_FORMAT_RGB_565:
            bpp = 16;
            break;
        default:
            /* Assume 32bpp BGRX (most common UEFI GOP format) */
            bpp = 32;
            format_enum = SIMPLEFB_FORMAT_BGRX_8888;
            break;
    }

    /* Compute total framebuffer size */
    uint32_t fb_size = stride * height;

    /* Map the framebuffer as uncacheable (ioremap equivalent) */
    /* For now, assume the framebuffer is already identity-mapped or
       accessible via direct map.  A real kernel would call ioremap(). */
    g_simplefb.fb_addr    = (uint32_t*)(uintptr_t)fb_addr;
    g_simplefb.width      = width;
    g_simplefb.height     = height;
    g_simplefb.stride     = stride;
    g_simplefb.fb_size    = fb_size;
    g_simplefb.format     = (enum simplefb_format)format_enum;
    g_simplefb.registered = 0;

    kprintf("[simplefb] %s framebuffer: %dx%d, %dbpp, stride=%d, addr=%p, size=%u\n",
            simplefb_format_name(g_simplefb.format),
            width, height, bpp, stride, g_simplefb.fb_addr, fb_size);

    /* Register with fbcon */
    fbcon_init(g_simplefb.fb_addr, g_simplefb.width,
               g_simplefb.height, g_simplefb.stride);
    g_simplefb.registered = 1;

    /* Also update vga module so that other subsystems know about fb */
    /* vga_set_framebuffer(fb_addr, width, height, stride, bpp); */

    return 0;
}

/*
 * simplefb_init_from_multiboot2 — Initialize from multiboot2 framebuffer tag.
 */
int simplefb_init_from_multiboot2(uint64_t mboot_info_phys)
{
    if (!mboot_info_phys)
        return -1;

    /* Scan multiboot2 tags for framebuffer tag (type=8) */
    uint32_t total_size = *(uint32_t*)(uintptr_t)mboot_info_phys;
    uint32_t offset = 8; /* skip total_size + reserved */

    while (offset + 8 < total_size) {
        uint16_t tag_type = *(uint16_t*)(uintptr_t)(mboot_info_phys + offset);
        uint16_t tag_size = *(uint16_t*)(uintptr_t)(mboot_info_phys + offset + 2);

        if (tag_type == 0)
            break;

        if (tag_type == 8) { /* framebuffer info tag */
            if (tag_size < 25)
                goto skip;

            uint8_t *data = (uint8_t*)(uintptr_t)(mboot_info_phys + offset + 8);
            uint64_t fb_addr   = *(uint32_t*)(data);
            fb_addr           |= ((uint64_t)*(uint32_t*)(data + 4)) << 32;
            uint32_t pitch    = *(uint32_t*)(data + 8);
            uint32_t width    = *(uint32_t*)(data + 12);
            uint32_t height   = *(uint32_t*)(data + 16);
            uint8_t  bpp      = *(uint8_t*)(data + 24);
            uint8_t  fb_type  = *(uint8_t*)(data + 25);
            (void)bpp;
            (void)fb_type;

            int fmt = SIMPLEFB_FORMAT_BGRX_8888;

            kprintf("[simplefb] Found in multiboot2 tag: %dx%d pitch=%d addr=%p\n",
                    width, height, pitch, (void*)(uintptr_t)fb_addr);

            return simplefb_init(fb_addr, width, height, pitch, fmt);
        }

    skip:
        offset += (tag_size + 7) & ~7;
    }

    return -1;
}

/*
 * simplefb_init_from_multiboot1 — Initialize from multiboot v1 info.
 */
int simplefb_init_from_multiboot1(uint64_t mboot_info_phys)
{
    if (!mboot_info_phys)
        return -1;

    /* Multiboot v1: framebuffer fields at offset 60-88 if flags[2] is set */
    uint32_t flags = *(uint32_t*)(uintptr_t)mboot_info_phys;
    if (!(flags & (1U << 2))) {
        kprintf("[simplefb] No framebuffer in multiboot v1 info\n");
        return -1;
    }

    uint32_t fb_addr_low = *(uint32_t*)(uintptr_t)(mboot_info_phys + 60);
    uint32_t fb_addr_high = *(uint32_t*)(uintptr_t)(mboot_info_phys + 64);
    uint64_t fb_addr = ((uint64_t)fb_addr_high << 32) | fb_addr_low;
    uint32_t pitch = *(uint32_t*)(uintptr_t)(mboot_info_phys + 68);
    uint32_t width = *(uint32_t*)(uintptr_t)(mboot_info_phys + 72);
    uint32_t height = *(uint32_t*)(uintptr_t)(mboot_info_phys + 76);
    uint8_t  bpp = *(uint8_t*)(uintptr_t)(mboot_info_phys + 80);
    /* fb_type at offset 81 */

    if (width == 0 || height == 0 || fb_addr == 0) {
        kprintf("[simplefb] Multiboot v1 framebuffer info incomplete\n");
        return -1;
    }

    kprintf("[simplefb] Multiboot v1 fb: %dx%d bpp=%d pitch=%d addr=%p\n",
            width, height, bpp, pitch, (void*)(uintptr_t)fb_addr);

    int fmt = SIMPLEFB_FORMAT_BGRX_8888;
    return simplefb_init(fb_addr, width, height, pitch, fmt);
}

/*
 * simplefb_get_info — Query the current simplefb configuration.
 */
int simplefb_get_info(uint64_t *fb_addr, uint32_t *width,
                      uint32_t *height, uint32_t *stride)
{
    if (!g_simplefb.fb_addr)
        return -1;

    if (fb_addr) *fb_addr = (uint64_t)(uintptr_t)g_simplefb.fb_addr;
    if (width)   *width   = g_simplefb.width;
    if (height)  *height  = g_simplefb.height;
    if (stride)  *stride  = g_simplefb.stride;
    return 0;
}

/*
 * simplefb_is_active — Return 1 if simplefb is driving the display.
 */
int simplefb_is_active(void)
{
    return (g_simplefb.fb_addr != NULL && g_simplefb.registered);
}

/* ── Set framebuffer mode (no-op for simplefb) ──────── */
int simplefb_set_mode(int width, int height, int bpp)
{
    /* simplefb is configured by bootloader/firmware.
     * Changing mode requires VESA BIOS call — not supported here. */
    (void)width;
    (void)height;
    (void)bpp;
    return 0;
}
