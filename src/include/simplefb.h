#ifndef SIMPLEFB_H
#define SIMPLEFB_H

#include "types.h"

/*
 * simplefb.h — Public interface for the Simple Framebuffer driver
 *
 * The simple framebuffer is a linear framebuffer pre-configured by the
 * boot firmware (UEFI GOP, Multiboot, coreboot, etc.).  This header
 * exposes the core initialisation and query functions so that other
 * subsystems (DRM, fbcon) can discover and use the framebuffer.
 */

/* SimpleFB pixel format enum */
enum simplefb_format {
    SIMPLEFB_FORMAT_RGBX_8888 = 0,
    SIMPLEFB_FORMAT_BGRX_8888 = 1,
    SIMPLEFB_FORMAT_XRGB_8888 = 2,
    SIMPLEFB_FORMAT_XBGR_8888 = 3,
    SIMPLEFB_FORMAT_RGB_565   = 4,
    SIMPLEFB_FORMAT_UNKNOWN   = 0xFF,
};

/*
 * simplefb_init — Register framebuffer from boot-provided parameters.
 *
 * @fb_addr:   physical address of the linear framebuffer
 * @width:     horizontal resolution in pixels
 * @height:    vertical resolution in pixels
 * @stride:    bytes per scanline (can be > width * bytes_per_pixel)
 * @format:    pixel format enum (simplefb_format values)
 *
 * Returns 0 on success, -1 on invalid parameters.
 */
int  simplefb_init(uint64_t fb_addr, uint32_t width, uint32_t height,
                   uint32_t stride, int format_enum);

/*
 * simplefb_init_from_multiboot2 — Initialize from multiboot2 framebuffer tag.
 *
 * @mboot_info_phys: physical address of the multiboot2 info structure.
 *
 * Returns 0 on success, -1 if no framebuffer tag is found.
 */
int  simplefb_init_from_multiboot2(uint64_t mboot_info_phys);

/*
 * simplefb_init_from_multiboot1 — Initialize from multiboot v1 info.
 *
 * @mboot_info_phys: physical address of the multiboot v1 info structure.
 *
 * Returns 0 on success, -1 if no framebuffer info is available.
 */
int  simplefb_init_from_multiboot1(uint64_t mboot_info_phys);

/*
 * simplefb_get_info — Query the current simplefb configuration.
 *
 * Returns framebuffer parameters through output pointers (all optional).
 * Returns 0 on success, -1 if no framebuffer is active.
 */
int  simplefb_get_info(uint64_t *fb_addr, uint32_t *width,
                       uint32_t *height, uint32_t *stride);

/*
 * simplefb_is_active — Check if simplefb is currently driving the display.
 *
 * Returns 1 if active, 0 otherwise.
 */
int  simplefb_is_active(void);

/*
 * simplefb_set_mode — Set framebuffer mode.
 *
 * For simplefb the mode is pre-configured by boot firmware; this is a
 * no-op that returns 0.  Real mode changes require VESA BIOS calls.
 */
int  simplefb_set_mode(int width, int height, int bpp);

#endif /* SIMPLEFB_H */
