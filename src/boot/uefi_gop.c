/*
 * uefi_gop.c — UEFI Graphics Output Protocol framebuffer setup
 *
 * Architecture Overview
 * =====================
 * The UEFI GOP framebuffer driver is responsible for discovering,
 * parsing, and registering the linear framebuffer that UEFI firmware
 * sets up during the boot phase. During early boot, the bootloader
 * passes framebuffer metadata to the kernel either through:
 *
 *   1. A flat struct uefi_gop_info at a known physical address (the
 *      @info_phys path) — used when a custom boot stub conveys the
 *      framebuffer parameters directly.
 *   2. A Multiboot2 framebuffer tag (type = 8) embedded in the
 *      multiboot2 information structure at @mboot_info_phys — the
 *      standard path when booting via a multiboot2-compliant loader
 *      such as GRUB.
 *
 * Once valid framebuffer parameters are obtained, the driver registers
 * the linear framebuffer with the fbcon (framebuffer console) subsystem
 * via fbcon_init(), which sets up the software console that renders
 * kernel boot messages onto the screen.
 *
 * Lifecycle
 * ---------
 *  - uefi_gop_init() is called early in startup (typically from
 *    arch-specific boot code). It probes the two discovery sources,
 *    validates the parsed parameters, and registers the framebuffer.
 *  - uefi_gop_is_active() is a trivial predicate that returns whether
 *    a valid framebuffer was found.
 *  - uefi_gop_get_info() retrieves the discovered parameters for
 *    other kernel subsystems (e.g., a graphical splash screen).
 *  - The remaining functions (uefi_gop_set_mode, uefi_gop_blt,
 *    uefi_gop_query_mode) are currently stubs that log their calls
 *    and return success or an appropriate error. They represent the
 *    interface an eventual native UEFI GOP protocol driver would
 *    implement for mode switching and blitting at runtime.
 *
 * Data Structure
 * --------------
 * The kernel maintains a single static g_gop_info instance populated
 * during uefi_gop_init(), guarded by the g_gop_valid flag. This is
 * a global singleton pattern: at most one UEFI framebuffer is expected.
 *
 * Item S156: UEFI GOP framebuffer
 * Item S158: Full documentation
 */

#include "types.h"
#include "printf.h"
#include "fbcon.h"
#include "vga.h"
#include "string.h"

/*
 * struct uefi_gop_info — UEFI GOP framebuffer metadata
 *
 * This packed structure mirrors the subset of the UEFI GOP mode
 * information (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION) that the kernel
 * cares about. It is populated either from a flat memory copy (the
 * @info_phys path) or by extracting fields from the Multiboot2
 * framebuffer tag.
 *
 * @fb_addr:       Physical base address of the linear framebuffer.
 * @fb_size:       Total size of the framebuffer region in bytes
 *                 (derived from pitch × height generally).
 * @width:         Horizontal resolution in pixels.
 * @height:        Vertical resolution in pixels.
 * @pitch:         Number of bytes per scanline (may include padding).
 * @bpp:           Bits per pixel — typically 32 for true-colour modes.
 * @pixel_format:  Encoding of the pixel format:
 *                   0 = RGB (pixel[31:24]=reserved, [23:16]=R, [15:8]=G, [7:0]=B)
 *                   1 = BGR (pixel[31:24]=reserved, [23:16]=B, [15:8]=G, [7:0]=R)
 *                   2 = Bitmap (indexed colour)
 *                   3 = Grayscale
 */
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
 * uefi_gop_init — Discover and register the UEFI GOP framebuffer
 *
 * This function is called once during early kernel boot (typically
 * from arch/early_init or similar) to locate the UEFI GOP framebuffer
 * and register it with the framebuffer console subsystem.
 *
 * Discovery follows a two-step strategy:
 *
 *   1. If @info_phys is non-zero, it is treated as a physical address
 *      pointing to a flat struct uefi_gop_info (the "direct" path used
 *      by custom boot stubs). The structure is memcpy'd into g_gop_info.
 *
 *   2. If step 1 did not yield valid parameters and @mboot_info_phys
 *      is non-zero, the Multiboot2 information structure starting at
 *      that address is scanned for a framebuffer tag (type = 8).
 *      Fields are extracted per the Multiboot2 specification and
 *      converted into the canonical g_gop_info representation.
 *
 * Validation checks: fb_addr must be non-zero, width and height must
 * be positive. If neither source produces valid data, the function
 * returns -1 (failure) and the framebuffer is not registered.
 *
 * Side effect: prints detected framebuffer parameters to the kernel log
 * and calls fbcon_init() to set up the software console.
 *
 * @mboot_info_phys: Physical address of the Multiboot2 information
 *                   structure, or 0 if not available.
 * @info_phys:       Physical address of a flat struct uefi_gop_info,
 *                   or 0 if not available.
 *
 * Returns: 0 on success, -1 if no valid framebuffer was found.
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

/*
 * uefi_gop_is_active — Query whether the GOP framebuffer is active
 *
 * Returns the value of the internal g_gop_valid flag which is set
 * during a successful uefi_gop_init() call.
 *
 * Returns: 1 if a valid framebuffer was discovered and registered,
 *          0 otherwise.
 */
int uefi_gop_is_active(void)
{
    return g_gop_valid;
}

/*
 * uefi_gop_get_info — Retrieve the discovered GOP framebuffer parameters
 *
 * Copies the current g_gop_info fields into the provided output
 * pointers. Any pointer that is NULL is skipped, allowing callers
 * to query only the fields they need.
 *
 * @fb_addr: [out] Receives the physical framebuffer address, or unused
 *           if NULL.
 * @width:   [out] Receives the horizontal resolution in pixels, or
 *           unused if NULL.
 * @height:  [out] Receives the vertical resolution in pixels, or
 *           unused if NULL.
 * @pitch:   [out] Receives the bytes-per-scanline value, or unused
 *           if NULL.
 */
void uefi_gop_get_info(uint64_t *fb_addr, uint32_t *width,
                       uint32_t *height, uint32_t *pitch)
{
    if (fb_addr) *fb_addr = g_gop_info.fb_addr;
    if (width)   *width   = g_gop_info.width;
    if (height)  *height  = g_gop_info.height;
    if (pitch)   *pitch   = g_gop_info.pitch;
}

/*
 * uefi_gop_set_mode — Stub: switch GOP video mode
 *
 * Currently a placeholder that validates the g_gop_valid flag and
 * logs the requested mode number. A real implementation would invoke
 * the UEFI GOP SetMode() runtime service to change the display
 * resolution.
 *
 * @mode: Mode index (0-based). Mode 0 is the current/initial mode.
 *
 * Returns: 0 on success, -ENODEV if no GOP framebuffer is active.
 */
int uefi_gop_set_mode(uint32_t mode)
{
    if (!g_gop_valid) {
        kprintf("[GOP] uefi_gop_set_mode: no GOP framebuffer\n");
        return -ENODEV;
    }
    kprintf("[GOP] uefi_gop_set_mode: mode %u (current: %dx%d)\n",
            mode, g_gop_info.width, g_gop_info.height);
    return 0;
}

/*
 * uefi_gop_blt — Stub: blit image data to/from the framebuffer
 *
 * Currently a placeholder that performs NULL-pointer and g_gop_valid
 * validation, then logs the operation parameters. A real implementation
 * would copy pixel data between the supplied buffer and the linear
 * framebuffer at the specified source/destination rectangles, following
 * the UEFI GOP Blt() service semantics.
 *
 * @buffer:    Pointer to image data (input or output depending on
 *             @operation). If operation == 2 (VideoFill, no data
 *             transfer), buffer may be NULL.
 * @operation: Blt operation code.
 * @src_x, @src_y: Source rectangle origin (in pixels).
 * @dst_x, @dst_y: Destination rectangle origin (in pixels).
 * @width:     Rectangle width in pixels.
 * @height:    Rectangle height in pixels.
 * @delta:     Source/destination scanline stride in bytes.
 *
 * Returns: 0 on success, -EINVAL if buffer is NULL on a video-to-buffer
 *          operation, -ENODEV if no GOP framebuffer is active.
 */
int uefi_gop_blt(void *buffer, uint32_t operation,
                 uint32_t src_x, uint32_t src_y,
                 uint32_t dst_x, uint32_t dst_y,
                 uint32_t width, uint32_t height, uint32_t delta)
{
    if (!buffer && operation != 2) {
        kprintf("[GOP] uefi_gop_blt: invalid buffer\n");
        return -EINVAL;
    }
    if (!g_gop_valid) {
        kprintf("[GOP] uefi_gop_blt: no GOP framebuffer\n");
        return -ENODEV;
    }
    kprintf("[GOP] uefi_gop_blt: operation=%u (%ux%u at %u,%u)\n",
            operation, width, height, dst_x, dst_y);
    return 0;
}

/*
 * uefi_gop_query_mode — Stub: query video mode parameters
 *
 * Returns the framebuffer parameters for a given mode index. Currently
 * only mode 0 (the current/initial mode) is supported. For any other
 * mode index, -EINVAL is returned.
 *
 * NULL-pointer validation is performed on @width, @height, and @pitch
 * — all three must be non-NULL.
 *
 * @mode:   Mode index to query (0-based). Only mode 0 is implemented.
 * @width:  [out] Receives the horizontal resolution in pixels.
 * @height: [out] Receives the vertical resolution in pixels.
 * @pitch:  [out] Receives the bytes-per-scanline value.
 *
 * Returns: 0 on success, -EINVAL if parameters are NULL or mode is
 *          unsupported, -ENODEV if no GOP framebuffer is active.
 */
int uefi_gop_query_mode(uint32_t mode, uint32_t *width,
                        uint32_t *height, uint32_t *pitch)
{
    if (!width || !height || !pitch)
        return -EINVAL;
    if (!g_gop_valid) {
        kprintf("[GOP] uefi_gop_query_mode: no GOP framebuffer\n");
        return -ENODEV;
    }
    if (mode == 0) {
        *width  = g_gop_info.width;
        *height = g_gop_info.height;
        *pitch  = g_gop_info.pitch;
        return 0;
    }
    kprintf("[GOP] uefi_gop_query_mode: mode %u not available\n", mode);
    return -EINVAL;
}
