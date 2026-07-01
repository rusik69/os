/*
 * drm_display.c — DRM display mode setting (CVT/EDID mode parsing)
 *
 * Provides display mode management for DRM connectors:
 *   - CVT (Coordinated Video Timings) timing calculation
 *   - EDID block 0 parsing (established, standard, detailed timings)
 *   - Standard mode fallback when no EDID is available
 *   - Mode list management per connector
 *
 * Architecture:
 *   - drm_display_cvt_mode() computes full timing from resolution + refresh
 *   - drm_display_edid_parse() extracts modes from a 128-byte EDID block
 *   - drm_display_fill_modes() generates a standard set of common modes
 *   - drm_display_add_mode() appends a mode to a connector's mode list
 *   - Connector mode list is used by drm_ioctl_mode_getconnector() to
 *     report available modes to userspace.
 *
 * Item D143 task 4 — DRM mode setting (CVT/EDID mode parsing)
 */

#define KERNEL_INTERNAL
#include "drm.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "kernel.h"

/* ═══════════════════════════════════════════════════════════════════
 *  CVT (Coordinated Video Timings) — Reduced Blanking v1.2
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * CVT-RB (Reduced Blanking) timing parameters.
 *
 * The CVT-RB standard defines tighter blanking intervals to save
 * bandwidth while remaining compatible with most modern displays.
 *
 * Horizontal:
 *   - Front porch: 48 pixels
 *   - HSync width:  32 pixels
 *   - Total blank:  160 pixels
 *
 * Vertical:
 *   - Front porch:  3 lines
 *   - VSync width:  6 lines (reduced), 10 lines (standard + border)
 *   - Total blank:  computed from margin + sync + porch
 *
 * Reference: VESA Coordinated Video Timings (CVT) v1.2, Section 4.2
 */

/* CVT-RB fixed horizontal timings (pixels) */
#define CVT_RB_H_FRONT       48
#define CVT_RB_H_SYNC        32
#define CVT_RB_H_BLANK       160  /* front + sync + back = 48+32+80 */

/* CVT standard horizontal timings (pixels) */
#define CVT_H_FRONT          56
#define CVT_H_SYNC           80
#define CVT_H_BACK           64
#define CVT_H_BLANK          200 /* front + sync + back = 56+80+64 */

/* CVT-RB vertical timings (lines) */
#define CVT_RB_V_FRONT       3
#define CVT_RB_V_SYNC        6
#define CVT_RB_V_BLANK_RATIO 550  /* 5.5 × (total lines) / cell_gran */

/* CVT standard vertical blanking lines */
#define CVT_MIN_V_BLANK      460  /* lines, rounded to cell granularity */
#define CVT_V_SYNC           4
#define CVT_V_FRONT          3
#define CVT_V_BACK           3

/* CVT cell granularity (both modes round to 8 pixels) */
#define CVT_CELL_GRAN        8
#define CVT_C_PRIME          40     /* 0.4 MHz (400 kHz) for clock rounding */
#define CVT_MARGIN_PERCENT   18     /* 1.8% margin for GTF compliance */

/* Helper: round up to nearest multiple of cell_gran */
static inline uint32_t round_to_cell(uint32_t val, uint32_t cell)
{
    return ((val + cell - 1) / cell) * cell;
}

/*
 * drm_display_cvt_mode — Generate CVT-RB or CVT standard timings.
 *
 * Implements the VESA CVT v1.2 algorithm for reduced blanking
 * (reduced=1) and standard blanking (reduced=0).
 *
 * Returns 0 on success, -EINVAL for invalid parameters.
 */
int drm_display_cvt_mode(uint32_t width, uint32_t height,
                          uint32_t refresh, int reduced,
                          struct drm_display_mode *out)
{
    uint64_t h_period;    /* horizontal period in picoseconds / 100 */
    uint64_t v_field_cycle; /* vertical field cycle in .01 Hz units */
    uint32_t h_pixels;
    uint32_t v_lines;
    uint32_t v_blank;
    uint32_t h_blank;
    uint32_t cell_gran = CVT_CELL_GRAN;
    uint32_t pixel_clock; /* in 10 kHz units during calc, kHz at end */

    if (!out)
        return -EINVAL;
    if (width < 320 || height < 200 || refresh < 24)
        return -EINVAL;
    if (width > 8192 || height > 8192)
        return -EINVAL;

    memset(out, 0, sizeof(*out));

    if (reduced) {
        /* ═══════════════════════════════════════════════════
         *  CVT-RB (Reduced Blanking) v1.2
         * ═══════════════════════════════════════════════════ */

        /* Round horizontal active to cell granularity */
        h_pixels = round_to_cell(width, cell_gran);

        /* Vertical active, rounded to cell granularity */
        v_lines = round_to_cell(height, cell_gran);

        /* Vertical blanking: 5.5 × total lines / cell_gran + margins */
        {
            uint32_t v_total_est = v_lines + CVT_RB_V_FRONT + CVT_RB_V_SYNC;
            /* Minimum vertical blanking = 460 lines rounded up */
            uint32_t min_v_blank = round_to_cell(CVT_MIN_V_BLANK, cell_gran);
            /* Calculate v_blank as fraction of total */
            v_blank = (v_total_est * CVT_RB_V_BLANK_RATIO) / 1000;
            v_blank = round_to_cell(v_blank, cell_gran);
            if (v_blank < min_v_blank)
                v_blank = min_v_blank;
        }

        /* Horizontal blanking (fixed for RB) */
        h_blank = CVT_RB_H_BLANK;

        /* Compute pixel clock:
         *   pixel_clock (kHz) = h_total × v_total × refresh / 1000
         *   Work in 10 kHz units to avoid overflow, then scale. */
        {
            uint32_t h_total = h_pixels + h_blank;
            uint32_t v_total = v_lines + v_blank;
            uint64_t clk = (uint64_t)h_total * (uint64_t)v_total
                         * (uint64_t)refresh;
            /* Divide by 1000: clock is in Hz, we want kHz.
             * But (total * refresh) gives Hz, to get kHz divide by 1000.
             * We use 64-bit math to avoid overflow at 4K 120Hz. */
            pixel_clock = (uint32_t)(clk / 1000U);
        }

        /* Round pixel clock to 0.25 MHz precision */
        {
            uint32_t rem = pixel_clock % 250;
            if (rem >= 125)
                pixel_clock += (250 - rem);
            else
                pixel_clock -= rem;
            if (pixel_clock < 1000)
                pixel_clock = 1000;
        }

        /* Fill mode structure */
        out->clock     = pixel_clock;
        out->hdisplay  = (uint16_t)h_pixels;
        out->hsync_start = (uint16_t)(h_pixels + CVT_RB_H_FRONT);
        out->hsync_end   = (uint16_t)(h_pixels + CVT_RB_H_FRONT + CVT_RB_H_SYNC);
        out->htotal      = (uint16_t)(h_pixels + h_blank);

        out->vdisplay    = (uint16_t)v_lines;
        out->vsync_start = (uint16_t)(v_lines + CVT_RB_V_FRONT);
        out->vsync_end   = (uint16_t)(v_lines + CVT_RB_V_FRONT + CVT_RB_V_SYNC);
        out->vtotal      = (uint16_t)(v_lines + v_blank);

        out->flags   = DRM_DISPLAY_MODE_FLAG_PHSYNC | DRM_DISPLAY_MODE_FLAG_NVSYNC;
        out->type    = DRM_MODE_TYPE_DRIVER;

    } else {
        /* ═══════════════════════════════════════════════════
         *  CVT Standard (with blanking)
         * ═══════════════════════════════════════════════════ */

        /* Round horizontal active to cell granularity */
        h_pixels = round_to_cell(width, cell_gran);

        /* Vertical lines, rounded */
        v_lines = round_to_cell(height, cell_gran);

        /* Estimate total lines for vertical blank calculation */
        {
            uint32_t v_total_est = v_lines + CVT_MIN_V_BLANK / 10;
            /* v_blank formula: (v_total * 0.05) rounded to cell_gran, min 460 */
            v_blank = (v_total_est * 5 + 50) / 100;
            v_blank = round_to_cell(v_blank, cell_gran);
            if (v_blank < CVT_MIN_V_BLANK / 10)
                v_blank = CVT_MIN_V_BLANK / 10;
            if (v_blank > 1000)
                v_blank = 1000;
        }

        h_blank = CVT_H_BLANK;

        /* Compute pixel clock */
        {
            uint32_t h_total = h_pixels + h_blank;
            uint32_t v_total = v_lines + v_blank;
            uint64_t clk = (uint64_t)h_total * (uint64_t)v_total
                         * (uint64_t)refresh;
            pixel_clock = (uint32_t)(clk / 1000U);
        }

        /* Round pixel clock */
        {
            uint32_t rem = pixel_clock % 250;
            if (rem >= 125)
                pixel_clock += (250 - rem);
            else
                pixel_clock -= rem;
            if (pixel_clock < 1000)
                pixel_clock = 1000;
        }

        /* Fill mode structure */
        out->clock     = pixel_clock;
        out->hdisplay  = (uint16_t)h_pixels;
        out->hsync_start = (uint16_t)(h_pixels + CVT_H_FRONT);
        out->hsync_end   = (uint16_t)(h_pixels + CVT_H_FRONT + CVT_H_SYNC);
        out->htotal      = (uint16_t)(h_pixels + h_blank);

        out->vdisplay    = (uint16_t)v_lines;
        out->vsync_start = (uint16_t)(v_lines + CVT_V_FRONT);
        out->vsync_end   = (uint16_t)(v_lines + CVT_V_FRONT + CVT_V_SYNC);
        out->vtotal      = (uint16_t)(v_lines + v_blank);

        out->flags   = DRM_DISPLAY_MODE_FLAG_NHSYNC | DRM_DISPLAY_MODE_FLAG_PVSYNC;
        out->type    = DRM_MODE_TYPE_DRIVER;
    }

    /* Compute vrefresh in mHz: (pixel_clock * 1000) / (htotal * vtotal) */
    {
        uint32_t h_total = out->htotal;
        uint32_t v_total = out->vtotal;
        if (h_total > 0 && v_total > 0) {
            uint64_t refresh_mhz = (uint64_t)out->clock * 1000000ULL
                                 / ((uint64_t)h_total * (uint64_t)v_total);
            out->vrefresh = (uint32_t)refresh_mhz;
        }
    }

    /* Create human-readable name */
    {
        int n = snprintf(out->name, sizeof(out->name),
                         "%dx%d@%d%s",
                         width, height, refresh,
                         reduced ? "rb" : "");
        if (n < 0)
            out->name[0] = '\0';
    }

    out->in_use = 1;

    kprintf("[DRM display] CVT mode: %s — clock=%u kHz, "
            "hdisp=%u htotal=%u vdisp=%u vtotal=%u vrefresh=%u mHz\n",
            out->name,
            out->clock, out->hdisplay, out->htotal,
            out->vdisplay, out->vtotal, out->vrefresh);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  EDID Parsing (block 0 — 128-byte Base EDID)
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * EDID block 0 structure (128 bytes):
 *
 *   Bytes  0-7    Header: 00 FF FF FF FF FF FF 00
 *   Bytes  8-17   Manufacturer ID + Product Code + Serial
 *   Bytes 18     Week of manufacture
 *   Bytes 19     Year of manufacture (offset 1990)
 *   Bytes 20-24  EDID version / revision (1.3 or 1.4)
 *   Bytes 25-34  Basic display parameters
 *   Bytes 35-49  Chromaticity coordinates
 *   Bytes 50-54  Established timings (3 bytes + 2 reserved)
 *   Bytes 55-70  Standard timings (8 × 2 bytes each)
 *   Bytes 71-122 Detailed timing descriptors (4 × 18 bytes each)
 *   Byte  123    Extension flag (number of extension blocks)
 *   Byte  127    Checksum (sum of all 128 bytes ≡ 0 mod 256)
 */

#define EDID_HEADER_SIZE        8
#define EDID_BLOCK_SIZE         128

/* EDID header bytes */
static const uint8_t edid_header[8] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
};

/* Established timing bit definitions (bytes 50-52) */
static const struct {
    uint16_t width;
    uint16_t height;
    uint8_t  refresh;
} established_timings[] = {
    /* Byte 50 (bit 7..0) — IBM timings */
    { 720,  400,  70 },   /* bit 7: 720x400 @ 70Hz */
    { 720,  400,  88 },   /* bit 6: 720x400 @ 88Hz */
    { 640,  480,  60 },   /* bit 5: 640x480 @ 60Hz */
    { 640,  480,  67 },   /* bit 4: 640x480 @ 67Hz */
    { 640,  480,  72 },   /* bit 3: 640x480 @ 72Hz */
    { 640,  480,  75 },   /* bit 2: 640x480 @ 75Hz */
    { 800,  600,  56 },   /* bit 1: 800x600 @ 56Hz */
    { 800,  600,  60 },   /* bit 0: 800x600 @ 60Hz */
    /* Byte 51 (bit 7..0) — more established */
    { 832,  624,  75 },   /* bit 7: 832x624 @ 75Hz (Apple) */
    {1024,  768,  87 },   /* bit 6: 1024x768 @ 87Hz (interlaced) */
    {1280, 1024,  60 },   /* bit 5: 1280x1024 @ 60Hz (VESA) */
    {1152,  870,  75 },   /* bit 4: 1152x870 @ 75Hz (Apple) */
    {1280,  960,  60 },   /* bit 3: 1280x960 @ 60Hz (VESA) */
    { 640,  480,  85 },   /* bit 2: 640x480 @ 85Hz */
    { 800,  600,  72 },   /* bit 1: 800x600 @ 72Hz */
    { 800,  600,  75 },   /* bit 0: 800x600 @ 75Hz */
    /* Byte 52 (bit 7..0) — more established */
    {1024,  768,  60 },   /* bit 7: 1024x768 @ 60Hz */
    {1024,  768,  70 },   /* bit 6: 1024x768 @ 70Hz */
    {1024,  768,  75 },   /* bit 5: 1024x768 @ 75Hz */
    {1280, 1024,  75 },   /* bit 4: 1280x1024 @ 75Hz */
    {1152,  864,  75 },   /* bit 3: 1152x864 @ 75Hz (Apple) */
};

#define ESTABLISHED_COUNT \
    (sizeof(established_timings) / sizeof(established_timings[0]))

/*
 * edid_checksum_valid — Verify EDID checksum.
 * Returns 1 if valid, 0 if invalid.
 */
static int edid_checksum_valid(const uint8_t *edid)
{
    uint8_t sum = 0;
    for (int i = 0; i < EDID_BLOCK_SIZE; i++)
        sum += edid[i];
    return (sum == 0);
}

/*
 * edid_parse_established_timings — Parse 3-byte established timing bitmap
 * and add modes via CVT.
 */
static int edid_parse_established_timings(struct drm_connector *conn,
                                           const uint8_t *edid)
{
    int count = 0;

    /* Bytes 50-52: established timing bitmap */
    for (unsigned i = 0; i < ESTABLISHED_COUNT; i++) {
        unsigned byte_idx = 50 + i / 8;
        unsigned bit_pos = 7 - (i % 8);  /* bit 7 is MSB of byte 50 */
        if (edid[byte_idx] & (1U << bit_pos)) {
            struct drm_display_mode mode;
            if (drm_display_cvt_mode(established_timings[i].width,
                                      established_timings[i].height,
                                      established_timings[i].refresh,
                                      1, /* reduced blanking */
                                      &mode) == 0) {
                mode.type |= DRM_MODE_TYPE_BUILTIN;
                if (drm_display_add_mode(conn, &mode) == 0)
                    count++;
            }
        }
    }

    return count;
}

/*
 * edid_parse_standard_timings — Parse standard timing entries (8 × 2 bytes).
 *
 * Each entry: byte1 = horizontal active / 8 - 31; byte2 = aspect + refresh.
 */
static int edid_parse_standard_timings(struct drm_connector *conn,
                                        const uint8_t *edid)
{
    int count = 0;

    /* Bytes 55-70: 8 standard timing descriptors, 2 bytes each */
    for (int i = 0; i < 8; i++) {
        unsigned offset = 55 + i * 2;
        uint8_t b1 = edid[offset];
        uint8_t b2 = edid[offset + 1];

        /* 01 01 marks unused entry */
        if (b1 == 0x01 && b2 == 0x01)
            continue;

        /* Horizontal active pixels */
        uint32_t width = (uint32_t)(b1 + 31) * 8;
        if (width < 256 || width > 4096)
            continue;

        /* Aspect ratio and refresh from byte 2 */
        uint32_t height;
        uint32_t refresh;
        uint8_t aspect = (b2 >> 6) & 0x03;
        refresh = (uint32_t)(b2 & 0x3F) + 60;

        switch (aspect) {
            case 0: height = width * 10 / 16; break;  /* 16:10 */
            case 1: height = width * 3 / 4;   break;  /* 4:3 */
            case 2: height = width * 9 / 16;  break;  /* 16:9 */
            case 3: height = width * 4 / 5;   break;  /* 5:4 */
            default: continue;
        }

        struct drm_display_mode mode;
        if (drm_display_cvt_mode(width, height, refresh, 1, &mode) == 0) {
            mode.type |= DRM_MODE_TYPE_BUILTIN;
            if (drm_display_add_mode(conn, &mode) == 0)
                count++;
        }
    }

    return count;
}

/*
 * Detailed timing descriptor types (bytes 71-122, 4 × 18 bytes)
 *
 * The first byte of each descriptor determines its type:
 *   0x00 = dummy descriptor (padding)
 *   0x10 = "Dummy" / reserved
 *   0xFB = Monitor range limits (serial or range limits)
 *   0xFC = Monitor name string
 *   0xFD = Monitor range limits
 *   <other> = Detailed timing (first byte = non-zero pixel clock/256)
 */
#define DTD_DUMMY       0x00
#define DTD_SERIAL      0xFF
#define DTD_STRING      0xFE
#define DTD_RANGE_LIMITS 0xFD
#define DTD_NAME        0xFC
#define DTD_SERIAL2     0xFB
#define DTD_WHITE_POINT 0xFA
#define DTD_STD_TIMING  0xF9

/*
 * edid_parse_detailed_timing — Parse an 18-byte detailed timing descriptor.
 * Returns 0 on success, -1 if not a valid timing descriptor.
 */
static int edid_parse_detailed_timing(struct drm_connector *conn,
                                       const uint8_t *dtd)
{
    struct drm_display_mode mode;
    memset(&mode, 0, sizeof(mode));

    /* Pixel clock in 10 kHz units */
    uint16_t clock_10khz = (uint16_t)(dtd[0]) | ((uint16_t)(dtd[1]) << 8);
    if (clock_10khz == 0)
        return -1;
    mode.clock = (uint32_t)clock_10khz * 10;  /* convert to kHz */

    /* Horizontal timing */
    uint16_t h_active_lo   = dtd[2];
    uint16_t h_blank_lo    = dtd[3];
    uint8_t  h_active_hi   = (dtd[4] >> 4) & 0x0F;
    uint8_t  h_blank_hi    = dtd[4] & 0x0F;

    mode.hdisplay = (uint16_t)((h_active_hi << 8) | h_active_lo);
    uint16_t h_blank = (uint16_t)((h_blank_hi << 8) | h_blank_lo);
    mode.htotal   = mode.hdisplay + h_blank;

    uint16_t h_sync_off_lo = dtd[5];
    uint8_t  h_sync_width_lo = dtd[6];
    uint8_t  h_sync_off_hi = (dtd[7] >> 6) & 0x03;
    uint8_t  h_sync_width_hi = (dtd[7] >> 4) & 0x03;

    uint16_t h_sync_off = (uint16_t)((h_sync_off_hi << 8) | h_sync_off_lo);
    mode.hsync_start = mode.hdisplay + h_sync_off;

    uint16_t h_sync_wid = (uint16_t)((h_sync_width_hi << 8) | h_sync_width_lo);
    mode.hsync_end   = mode.hsync_start + h_sync_wid;

    /* Vertical timing */
    uint16_t v_active_lo   = dtd[7] & 0x0F;
    v_active_lo |= ((uint16_t)(dtd[8] & 0xF0)) << 4;
    uint16_t v_blank_lo    = dtd[8] & 0x0F;
    v_blank_lo |= ((uint16_t)(dtd[9] & 0xF0)) << 4;
    uint8_t  v_active_hi   = (dtd[10] >> 4) & 0x0F;
    uint8_t  v_blank_hi    = dtd[10] & 0x0F;

    mode.vdisplay = (uint16_t)((v_active_hi << 8) | v_active_lo);
    uint16_t v_blank = (uint16_t)((v_blank_hi << 8) | v_blank_lo);
    mode.vtotal   = mode.vdisplay + v_blank;

    uint16_t v_sync_off_lo  = dtd[11];
    uint8_t  v_sync_width_lo = dtd[12];
    uint8_t  v_sync_off_hi  = (dtd[13] >> 6) & 0x03;
    uint8_t  v_sync_width_hi = (dtd[13] >> 4) & 0x03;

    uint16_t v_sync_off = (uint16_t)((v_sync_off_hi << 8) | v_sync_off_lo);
    uint16_t v_sync_wid = (uint16_t)((v_sync_width_hi << 8) | v_sync_width_lo);

    mode.vsync_start = mode.vdisplay + v_sync_off;
    mode.vsync_end   = mode.vsync_start + v_sync_wid;

    /* Synchronisation flags */
    uint8_t flags = dtd[17];
    if (flags & 0x01)
        mode.flags |= DRM_DISPLAY_MODE_FLAG_INTERLACE;
    if (flags & 0x02)
        mode.flags |= DRM_DISPLAY_MODE_FLAG_DBLSCAN;
    if (flags & 0x04)
        mode.flags |= DRM_DISPLAY_MODE_FLAG_DBLSCAN; /* stereo modes */
    /* Sync polarity */
    if (flags & 0x08)
        mode.flags |= DRM_DISPLAY_MODE_FLAG_PHSYNC;
    else
        mode.flags |= DRM_DISPLAY_MODE_FLAG_NHSYNC;
    if (flags & 0x10)
        mode.flags |= DRM_DISPLAY_MODE_FLAG_PVSYNC;
    else
        mode.flags |= DRM_DISPLAY_MODE_FLAG_NVSYNC;

    /* Compute refresh rate in mHz */
    if (mode.htotal > 0 && mode.vtotal > 0) {
        uint64_t refresh_mhz = (uint64_t)mode.clock * 1000000ULL
                             / ((uint64_t)mode.htotal * (uint64_t)mode.vtotal);
        mode.vrefresh = (uint32_t)refresh_mhz;
    }

    mode.type  = DRM_MODE_TYPE_BUILTIN | DRM_MODE_TYPE_PREFERRED;
    mode.in_use = 1;

    /* Create name */
    snprintf(mode.name, sizeof(mode.name),
             "%dx%d", mode.hdisplay, mode.vdisplay);

    return drm_display_add_mode(conn, &mode);
}

/*
 * edid_parse_detailed_descriptors — Parse the 4 detailed timing / monitor
 * descriptors at bytes 71-122.
 */
static int edid_parse_detailed_descriptors(struct drm_connector *conn,
                                            const uint8_t *edid)
{
    int count = 0;

    for (int i = 0; i < 4; i++) {
        const uint8_t *desc = edid + 71 + i * 18; /* 4 × 18 bytes */
        /* If pixel clock (bytes 0-1) is non-zero, it's a detailed timing */
        if (desc[0] != 0x00 || desc[1] != 0x00) {
            if (edid_parse_detailed_timing(conn, desc) == 0)
                count++;
        }
    }

    return count;
}

/*
 * drm_display_edid_parse — Parse a 128-byte EDID block and add modes.
 *
 * Returns number of modes added, or negative errno on error.
 */
int drm_display_edid_parse(struct drm_connector *conn,
                            const uint8_t *edid)
{
    int total = 0;

    if (!conn || !edid)
        return -EINVAL;

    /* Verify EDID header */
    if (memcmp(edid, edid_header, EDID_HEADER_SIZE) != 0) {
        kprintf("[DRM display] EDID: invalid header\n");
        return -EINVAL;
    }

    /* Verify checksum */
    if (!edid_checksum_valid(edid)) {
        kprintf("[DRM display] EDID: checksum failed\n");
        return -EINVAL;
    }

    kprintf("[DRM display] EDID: valid block 0, manufacturer=%c%c\n",
            'A' + ((edid[8] >> 2) & 0x1F) - 1,
            'A' + ((((edid[8] & 0x03) << 3) | ((edid[9] >> 5) & 0x07)) - 1));

    /* Parse established timings */
    int estab = edid_parse_established_timings(conn, edid);
    total += estab;
    kprintf("[DRM display] EDID: %d established timings\n", estab);

    /* Parse standard timings */
    int std = edid_parse_standard_timings(conn, edid);
    total += std;
    kprintf("[DRM display] EDID: %d standard timings\n", std);

    /* Parse detailed timing descriptors */
    int det = edid_parse_detailed_descriptors(conn, edid);
    total += det;
    kprintf("[DRM display] EDID: %d detailed timings\n", det);

    kprintf("[DRM display] EDID: parsed %d modes total\n", total);
    return total;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Standard mode fallback (when no EDID available)
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Common display resolutions for fallback mode generation.
 */
static const struct {
    uint32_t width;
    uint32_t height;
} standard_resolutions[] = {
    {  640,  480 },  /* VGA */
    {  800,  600 },  /* SVGA */
    { 1024,  768 },  /* XGA */
    { 1152,  864 },  /* XGA+ */
    { 1280,  720 },  /* HD 720p */
    { 1280,  800 },  /* WXGA */
    { 1280, 1024 },  /* SXGA */
    { 1366,  768 },  /* HD 720p wide */
    { 1440,  900 },  /* WXGA+ */
    { 1600,  900 },  /* HD+ */
    { 1680, 1050 },  /* WSXGA+ */
    { 1920, 1080 },  /* Full HD 1080p */
    { 1920, 1200 },  /* WUXGA */
    { 2560, 1440 },  /* QHD 1440p */
    { 2560, 1600 },  /* WQXGA */
    { 3200, 1800 },  /* 3K */
    { 3840, 2160 },  /* 4K UHD */
};

/*
 * drm_display_fill_modes — Populate a connector with standard CVT modes.
 *
 * Generates a set of common resolutions using CVT-RB, up to the given
 * maximum dimensions.  Uses 60 Hz for all modes unless max_width indicates
 * a smaller display (then uses 75 Hz for lower resolutions too).
 *
 * Returns number of modes added.
 */
int drm_display_fill_modes(struct drm_connector *conn,
                            int max_width, int max_height)
{
    int count = 0;

    if (!conn)
        return 0;

    if (max_width <= 0)
        max_width = 1920;
    if (max_height <= 0)
        max_height = 1080;

    for (unsigned i = 0; i < ARRAY_SIZE(standard_resolutions); i++) {
        uint32_t w = standard_resolutions[i].width;
        uint32_t h = standard_resolutions[i].height;

        if (w > (uint32_t)max_width || h > (uint32_t)max_height)
            continue;

        /* Try 60 Hz CVT-RB */
        {
            struct drm_display_mode mode;
            if (drm_display_cvt_mode(w, h, 60, 1, &mode) == 0) {
                mode.type = DRM_MODE_TYPE_DRIVER;
                /* Mark the most common res as preferred */
                if (w == (uint32_t)max_width && h == (uint32_t)max_height)
                    mode.type |= DRM_MODE_TYPE_PREFERRED;
                if (drm_display_add_mode(conn, &mode) == 0)
                    count++;
            }
        }

        /* For smaller resolutions, also add 75 Hz */
        if (w <= 1280 && h <= 1024) {
            struct drm_display_mode mode;
            if (drm_display_cvt_mode(w, h, 75, 1, &mode) == 0) {
                mode.type = DRM_MODE_TYPE_DRIVER;
                if (drm_display_add_mode(conn, &mode) == 0)
                    count++;
            }
        }
    }

    kprintf("[DRM display] fill_modes: added %d modes (max %dx%d)\n",
            count, max_width, max_height);
    return count;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Mode list management per connector
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * drm_display_add_mode — Add a display mode to a connector.
 *
 * @conn:  Connector to add to.
 * @mode:  Mode to add (copied).
 *
 * Returns 0 on success, -ENOMEM if connector mode list is full.
 */
int drm_display_add_mode(struct drm_connector *conn,
                          const struct drm_display_mode *mode)
{
    if (!conn || !mode || !mode->in_use)
        return -EINVAL;

    if (conn->num_modes >= DRM_MAX_DISPLAY_MODES)
        return -ENOMEM;

    conn->modes[conn->num_modes] = *mode;
    conn->num_modes++;

    return 0;
}

/*
 * drm_display_clear_modes — Remove all modes from a connector.
 */
void drm_display_clear_modes(struct drm_connector *conn)
{
    if (!conn)
        return;

    memset(conn->modes, 0, sizeof(conn->modes));
    conn->num_modes = 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Initialisation / teardown
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * drm_display_init — Initialise the display mode subsystem.
 *
 * At present, this is a no-op since we use statically-allocated
 * mode arrays within each connector.  Hooks are provided for future
 * dynamic mode table allocation.
 */
int drm_display_init(void)
{
    kprintf("[DRM display] mode subsystem initialised\n");
    return 0;
}

/*
 * drm_display_exit — Tear down the display mode subsystem.
 */
void drm_display_exit(void)
{
    kprintf("[DRM display] mode subsystem shut down\n");
}
