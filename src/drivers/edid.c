/*
 * edid.c — EDID (Extended Display Identification Data) parser
 *
 * Parses standard 128-byte EDID blocks from displays.
 * Extects preferred timing, manufacturer info, and display descriptors.
 */

#include "edid.h"
#include "printf.h"
#include "string.h"

/* EDID fixed header */
static const uint8_t edid_header[8] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
};

int edid_validate_checksum(const uint8_t *raw) {
    if (!raw)
        return -1;

    uint8_t sum = 0;
    for (int i = 0; i < EDID_SIZE; i++)
        sum += raw[i];

    return (sum == 0) ? 0 : -1;
}

/* Parse standard timing from a standard timing byte pair */
static void parse_standard_timing(const uint8_t *data, struct edid_timing *t) {
    if (!data || !t)
        return;

    uint8_t b1 = data[0];
    uint8_t b2 = data[1];

    if (b1 == 0x01 && b2 == 0x01) {
        /* Manufacturer-specific, skip */
        t->h_pixels = 0;
        t->v_lines = 0;
        return;
    }

    uint8_t aspect = (b1 >> 6) & 0x03;
    uint8_t freq = (b1 & 0x3F);
    uint16_t h_pixels = (uint16_t)(b2 & 0xE0);
    h_pixels = ((h_pixels << 3) | (b2 & 0x1F)) * 8;

    t->h_pixels = h_pixels;
    t->v_freq = freq + 60;  /* Vertical frequency = freq + 60 Hz */

    switch (aspect) {
        case 0: t->v_lines = h_pixels * 10 / 16; break; /* 16:10 */
        case 1: t->v_lines = h_pixels * 3 / 4; break;   /* 4:3 */
        case 2: t->v_lines = h_pixels * 4 / 5; break;   /* 5:4 */
        case 3: t->v_lines = h_pixels * 9 / 16; break;  /* 16:9 */
    }
}

/* Parse detailed timing descriptor (18 bytes) */
static int parse_detailed_timing(const uint8_t *desc, struct edid_timing *t) {
    if (!desc || !t)
        return -1;

    uint16_t h_pixels = (uint16_t)(desc[2]) | ((uint16_t)(desc[4] & 0xF0) << 4);
    uint16_t h_blank = (uint16_t)(desc[3]) | ((uint16_t)(desc[4] & 0x0F) << 8);
    uint16_t v_lines = (uint16_t)(desc[5]) | ((uint16_t)(desc[7] & 0xF0) << 4);
    uint16_t v_blank = (uint16_t)(desc[6]) | ((uint16_t)(desc[7] & 0x0F) << 8);

    uint16_t h_sync_off = (uint16_t)(desc[8]) | ((uint16_t)(desc[11] & 0xC0) << 2);
    uint16_t h_sync_wid = (uint16_t)(desc[9]) | ((uint16_t)(desc[11] & 0x30) << 4);
    uint16_t v_sync_off = ((uint16_t)(desc[10] >> 4)) | ((uint16_t)(desc[11] & 0x0C) << 2);
    uint16_t v_sync_wid = ((uint16_t)(desc[10] & 0x0F)) | ((uint16_t)(desc[11] & 0x03) << 4);

    uint16_t h_total = h_pixels + h_blank;
    uint16_t v_total = v_lines + v_blank;

    if (h_total == 0 || v_total == 0)
        return -1;

    /* Pixel clock in 10kHz units */
    uint16_t pixel_clock = (uint16_t)(desc[0]) | ((uint16_t)(desc[1]) << 8);

    t->h_pixels = h_pixels;
    t->v_lines = v_lines;
    t->interlaced = (desc[17] & 0x80) ? 1 : 0;

    if (pixel_clock > 0) {
        uint64_t vfreq = (uint64_t)pixel_clock * 10000ULL / (uint64_t)h_total / (uint64_t)v_total;
        t->v_freq = (uint8_t)vfreq;
    }

    return 0;
}

int edid_parse(const uint8_t *raw, struct edid_data *edid) {
    if (!raw || !edid)
        return -1;

    memset(edid, 0, sizeof(struct edid_data));

    /* Validate header */
    if (memcmp(raw, edid_header, 8) != 0)
        return -1;

    /* Check checksum */
    if (edid_validate_checksum(raw) < 0)
        return -1;

    edid->valid = 1;

    /* Manufacturer ID: 3 letters packed in 2 bytes */
    uint16_t mfg = (uint16_t)(raw[0x08]) | ((uint16_t)(raw[0x09]) << 8);
    edid->manufacturer[0] = (char)('A' + ((mfg >> 10) & 0x1F) - 1);
    edid->manufacturer[1] = (char)('A' + ((mfg >> 5) & 0x1F) - 1);
    edid->manufacturer[2] = (char)('A' + (mfg & 0x1F) - 1);
    edid->manufacturer[3] = '\0';

    /* Product code */
    edid->product_code = (uint16_t)(raw[0x0A]) | ((uint16_t)(raw[0x0B]) << 8);

    /* Serial number */
    edid->serial = (uint32_t)(raw[0x0C]) | ((uint32_t)(raw[0x0D]) << 8) |
                   ((uint32_t)(raw[0x0E]) << 16) | ((uint32_t)(raw[0x0F]) << 24);

    /* Week and year */
    edid->week = raw[0x10];
    edid->year = raw[0x11] + 1990;

    /* EDID version/revision */
    edid->edid_version = raw[0x12];
    edid->edid_revision = raw[0x13];

    /* Basic display parameters */
    edid->video_input_type = raw[0x14];
    edid->max_h_size = raw[0x15];
    edid->max_v_size = raw[0x16];
    edid->gamma = raw[0x17];
    edid->features = raw[0x18];

    /* Chromaticity coordinates */
    uint8_t *chroma = (uint8_t *)&raw[0x19];
    edid->red_x   = (uint16_t)(chroma[0] << 2) | ((chroma[2] >> 6) & 0x03);
    edid->red_y   = (uint16_t)(chroma[1] << 2) | ((chroma[2] >> 4) & 0x03);
    edid->green_x = (uint16_t)(chroma[3] << 2) | ((chroma[2] >> 2) & 0x03);
    edid->green_y = (uint16_t)(chroma[4] << 2) | ((chroma[2] >> 0) & 0x03);
    edid->blue_x  = (uint16_t)(chroma[5] << 2) | ((chroma[3] >> 6) & 0x03);
    edid->blue_y  = (uint16_t)(chroma[6] << 2) | ((chroma[3] >> 4) & 0x03);
    edid->white_x = (uint16_t)(chroma[7] << 2) | ((chroma[3] >> 2) & 0x03);
    edid->white_y = (uint16_t)(chroma[8] << 2) | ((chroma[3] >> 0) & 0x03);

    /* Standard timings (bytes 0x26-0x35) */
    for (int i = 0; i < 8; i++) {
        parse_standard_timing(&raw[0x26 + i * 2], &edid->standard_timings[i]);
    }

    /* Detailed timing / monitor descriptors (bytes 0x36-0x6F, 4 x 18 bytes) */
    for (int i = 0; i < 4; i++) {
        const uint8_t *desc = &raw[0x36 + i * 18];

        /* Check if it's a detailed timing descriptor */
        if (desc[0] != 0x00 || desc[1] != 0x00) {
            /* First detailed timing = preferred timing */
            if (i == 0) {
                parse_detailed_timing(desc, &edid->preferred_timing);
            }
        } else {
            /* Monitor descriptor */
            edid->descriptors[i].tag = desc[3];
            if (desc[3] == 0xFC) {
                /* Monitor name */
                int len = desc[0x0A] > 13 ? 13 : desc[0x0A];
                memcpy(edid->descriptors[i].text, &desc[5], len);
                edid->descriptors[i].text[len] = '\0';
            } else if (desc[3] == 0xFF) {
                /* Serial number string */
                int len = desc[0x0A] > 13 ? 13 : desc[0x0A];
                memcpy(edid->descriptors[i].text, &desc[5], len);
                edid->descriptors[i].text[len] = '\0';
            }
        }
    }

    return 0;
}

void edid_print_info(const struct edid_data *edid) {
    if (!edid || !edid->valid) {
        kprintf("EDID: Invalid or not parsed\n");
        return;
    }

    kprintf("EDID: Manufacturer=%s Product=0x%04X Serial=0x%08X\n",
            edid->manufacturer, edid->product_code, edid->serial);
    kprintf("  Version: %d.%d, Year: %d\n",
            edid->edid_version, edid->edid_revision, edid->year);
    kprintf("  Size: %dx%d cm, Gamma: %d.%d\n",
            edid->max_h_size, edid->max_v_size,
            edid->gamma / 100, edid->gamma % 100);

    if (edid->preferred_timing.h_pixels > 0) {
        kprintf("  Preferred timing: %dx%d @ %d Hz%s\n",
                edid->preferred_timing.h_pixels,
                edid->preferred_timing.v_lines,
                edid->preferred_timing.v_freq,
                edid->preferred_timing.interlaced ? "i" : "");
    }

    /* Print monitor name if found */
    for (int i = 0; i < 4; i++) {
        if (edid->descriptors[i].tag == 0xFC && edid->descriptors[i].text[0]) {
            kprintf("  Monitor: %s\n", edid->descriptors[i].text);
        }
    }
}

/* ── Stub: edid_find_mode ─────────────────────────────── */
int edid_find_mode(const void *edid, int width, int height, void *mode)
{
    (void)edid;
    (void)width;
    (void)height;
    (void)mode;
    kprintf("[edid] edid_find_mode: not yet implemented\n");
    return 0;
}
/* ── Stub: edid_valid ─────────────────────────────── */
int edid_valid(const void *edid)
{
    (void)edid;
    kprintf("[edid] edid_valid: not yet implemented\n");
    return 0;
}
