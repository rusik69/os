/*
 * edid.c — EDID (Extended Display Identification Data) parser
 *
 * Parses standard 128-byte EDID blocks (block 0) and CEA-861
 * extension blocks (block 1+).  Extracts base display parameters,
 * preferred timing, manufacturer info, display descriptors, and
 * additional CEA short video descriptors, audio blocks, and
 * detailed timings from extension blocks.
 *
 * Item D143 task 11 — EDID parsing (block 0, block 1)
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
    h_pixels = (uint16_t)(((h_pixels << 3) | (b2 & 0x1F)) * 8);
    t->h_pixels = h_pixels;
    t->v_freq = freq + 60;  /* Vertical frequency = freq + 60 Hz */

    switch (aspect) {
        case 0: t->v_lines = (uint16_t)(h_pixels * 10 / 16); break; /* 16:10 */
        case 1: t->v_lines = (uint16_t)(h_pixels * 3 / 4); break;   /* 4:3 */
        case 2: t->v_lines = (uint16_t)(h_pixels * 4 / 5); break;   /* 5:4 */
        case 3: t->v_lines = (uint16_t)(h_pixels * 9 / 16); break;  /* 16:9 */
        default:
            break;
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
    edid->year = (uint8_t)(raw[0x11] + 1990);  /* Note: bare uint8_t wraps after 2245; use uint16_t if needed beyond */

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

    /* Extension count (byte 126/0x7E) */
    edid->extension_count = raw[0x7E];

    return 0;
}

void edid_print_info(const struct edid_data *edid)
{
    if (!edid || !edid->valid) {
        kprintf("EDID: Invalid or not parsed\n");
        return;
    }

    kprintf("EDID: Manufacturer=%s Product=0x%04X Serial=0x%08X\n",
            edid->manufacturer, edid->product_code, edid->serial);
    kprintf("  Version: %d.%d, Year: %d, Ext blocks: %d\n",
            edid->edid_version, edid->edid_revision, edid->year,
            edid->extension_count);

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

    /* Print CEA extension block info */
    if (edid->has_cea_block) {
        kprintf("  CEA-861: rev %d, %d native DTDs, %d SVDs, %d audio blocks\n",
                edid->cea_revision, edid->cea_num_native_dtds,
                edid->num_svd, edid->num_audio_blocks);

        if (edid->num_svd > 0) {
            kprintf("  CEA SVD timings:");
            int show = (edid->num_svd < 6) ? edid->num_svd : 5;
            for (int i = 0; i < show; i++) {
                kprintf(" %dx%d%c%d",
                        edid->svd[i].h_pixels,
                        edid->svd[i].v_lines,
                        edid->svd[i].interlaced ? 'i' : 'p',
                        edid->svd[i].refresh);
            }
            if (edid->num_svd > 5)
                kprintf(" (+%d more)", edid->num_svd - 5);
            kprintf("\n");
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  CEA-861 Extension Block Parsing (EDID Block 1+)
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Extension block tag byte values (byte 0 of each 128-byte extension).
 */
#define EDID_EXT_TAG_CEA      0x02   /* CEA-861 extension */
#define EDID_EXT_TAG_VTB      0x10   /* Video Timing Block Extension */
#define EDID_EXT_TAG_DI       0x20   /* Display Information Extension */
#define EDID_EXT_TAG_LDI      0x50   /* Large Display Information Ext. */
#define EDID_EXT_TAG_USE      0xF0   /* Use blocks from another source */
#define EDID_EXT_TAG_BT_EXT   0x40   /* Block Type Extension (BT.2020) */

/* CEA data block tag types (bits 7:5 of header byte) */
#define CEA_DB_VIDEO         1   /* Short Video Descriptor */
#define CEA_DB_AUDIO         2   /* Short Audio Descriptor */
#define CEA_DB_SPEAKER       3   /* Speaker Allocation Data Block */
#define CEA_DB_VENDOR_SPEC   4   /* Vendor-Specific Data Block (VSDB) */
#define CEA_DB_COLORIMETRY   5   /* Colorimetry Data Block */
#define CEA_DB_Y420_VIDEO    7   /* YCbCr 4:2:0 Video Data Block */

/* ── CEA-861 Short Video Descriptor timing table ──────────────── */

/*
 * CEA-861 VIC (Video Identification Code) to timing parameters.
 *
 * Table entries for VICs 1 through 64 as defined in CEA-861-F/G.
 * VICs marked as "reserved" or "vendor-specific" (65-127, 129-255)
 * are not included.
 *
 *   h_pixels  — horizontal active pixels
 *   v_lines   — vertical active lines
 *   refresh   — nominal refresh rate in Hz
 *   interlaced — 1 if interlaced (i/p), 0 if progressive
 *   aspect_ratio — 0=4:3, 1=16:9, 2=64:27, 3=256:135
 *   pixel_clock — approximate pixel clock in kHz (at given refresh)
 */
static const struct edid_cea_svd_entry cea_svd_table[] = {
    /* Standard VICs as per CEA-861-F */
    {  640,  480, 60, 0, 0,  25175 }, /* VIC  1: 640x480p @ 60Hz 4:3 */
    {  720,  480, 60, 0, 0,  27000 }, /* VIC  2: 720x480p @ 60Hz 4:3 */
    {  720,  480, 60, 0, 1,  27000 }, /* VIC  3: 720x480p @ 60Hz 16:9 */
    { 1280,  720, 60, 0, 1,  74250 }, /* VIC  4: 1280x720p @ 60Hz 16:9 */
    { 1920, 1080, 60, 1, 1,  74250 }, /* VIC  5: 1920x1080i @ 60Hz 16:9 */
    {  720,  480, 60, 1, 0,  13500 }, /* VIC  6: 720x480i @ 60Hz 4:3 */
    {  720,  480, 60, 1, 1,  13500 }, /* VIC  7: 720x480i @ 60Hz 16:9 */
    { 1920, 1080, 60, 1, 1, 148500 }, /* VIC  8: 1920x1080i @ 60Hz 16:9 (weird) */
    { 1920, 1080, 60, 1, 1,  74250 }, /* VIC  9: duplicate of 5 */
    { 1920, 1080, 60, 1, 1,  74250 }, /* VIC 10: duplicate of 5 */
    { 1920, 1080, 60, 0, 1, 148500 }, /* VIC 11: 1920x1080p @ 60Hz */
    { 1920, 1080, 60, 0, 1, 148500 }, /* VIC 12: 1920x1080p @ 60Hz */
    { 1920, 1080, 60, 0, 1, 148500 }, /* VIC 13: 1920x1080p @ 60Hz */
    { 1440,  480, 60, 0, 0,  27000 }, /* VIC 14: 1440x480p @ 60Hz 4:3 */
    { 1440,  480, 60, 0, 1,  27000 }, /* VIC 15: 1440x480p @ 60Hz 16:9 */
    { 1920, 1080, 60, 0, 1, 148500 }, /* VIC 16: 1920x1080p @ 60Hz */
    {  720,  576, 50, 0, 0,  27000 }, /* VIC 17: 720x576p @ 50Hz 4:3 */
    {  720,  576, 50, 0, 1,  27000 }, /* VIC 18: 720x576p @ 50Hz 16:9 */
    { 1280,  720, 50, 0, 1,  74250 }, /* VIC 19: 1280x720p @ 50Hz 16:9 */
    { 1920, 1080, 50, 1, 1,  74250 }, /* VIC 20: 1920x1080i @ 50Hz 16:9 */
    {  720,  576, 50, 1, 0,  13500 }, /* VIC 21: 720x576i @ 50Hz 4:3 */
    {  720,  576, 50, 1, 1,  13500 }, /* VIC 22: 720x576i @ 50Hz 16:9 */
    { 1920, 1080, 50, 0, 1, 148500 }, /* VIC 23: 1920x1080p @ 50Hz 16:9 */
    { 1920, 1080, 50, 0, 1, 148500 }, /* VIC 24: 1920x1080p @ 50Hz 16:9 */
    { 1920, 1080, 50, 0, 1, 148500 }, /* VIC 25: duplicate of 24 */
    { 1920, 1080, 50, 0, 1, 148500 }, /* VIC 26: duplicate of 24 */
    { 1920, 1080, 50, 0, 1, 148500 }, /* VIC 27: duplicate of 24 */
    { 1920, 1080, 24, 0, 1,  74250 }, /* VIC 28: 1920x1080p @ 24Hz 16:9 */
    { 1920, 1080, 25, 0, 1,  74250 }, /* VIC 29: 1920x1080p @ 25Hz 16:9 */
    { 1920, 1080, 30, 0, 1,  74250 }, /* VIC 30: 1920x1080p @ 30Hz 16:9 */
    { 1920, 1080, 50, 1, 1,  74250 }, /* VIC 31: 1920x1080i @ 50Hz (smpte 274M) */
    { 1920, 1080, 50, 0, 1, 148500 }, /* VIC 32: 1920x1080p @ 50Hz */
    { 1440,  576, 50, 0, 0,  27000 }, /* VIC 33: 1440x576p @ 50Hz 4:3 */
    { 1440,  576, 50, 0, 1,  27000 }, /* VIC 34: 1440x576p @ 50Hz 16:9 */
    { 1920, 1080, 24, 0, 1,  74250 }, /* VIC 35: 1920x1080p @ 24Hz 16:9 */
    { 1920, 1080, 25, 0, 1,  74250 }, /* VIC 36: 1920x1080p @ 25Hz 16:9 */
    { 1920, 1080, 30, 0, 1,  74250 }, /* VIC 37: 1920x1080p @ 30Hz 16:9 */
    { 1920, 1080, 48, 0, 1, 148500 }, /* VIC 38: 1920x1080p @ 48Hz 16:9 */
    { 1920, 1080, 48, 0, 1, 148500 }, /* VIC 39: duplicate of 38 */
    { 1920, 1080, 60, 0, 1, 148500 }, /* VIC 40: 1920x1080p @ 60Hz (GTF) */
    { 1280,  720, 24, 0, 1,  59400 }, /* VIC 41: 1280x720p @ 24Hz 16:9 */
    { 1280,  720, 25, 0, 1,  59400 }, /* VIC 42: 1280x720p @ 25Hz 16:9 */
    { 1280,  720, 30, 0, 1,  74250 }, /* VIC 43: 1280x720p @ 30Hz 16:9 */
    { 1280,  720, 48, 0, 1,  90000 }, /* VIC 44: 1280x720p @ 48Hz 16:9 */
    { 1280,  720, 60, 0, 1,  90000 }, /* VIC 45: 1280x720p @ 60Hz 16:9 */
    { 1280,  720, 60, 0, 1,  90000 }, /* VIC 46: duplicate of 45 */
    { 1280,  720, 60, 0, 1,  90000 }, /* VIC 47: duplicate of 45 */
    { 1920, 1080, 24, 0, 1,  74250 }, /* VIC 48: 1920x1080p @ 24Hz (CEA) */
    { 1920, 1080, 25, 0, 1,  74250 }, /* VIC 49: 1920x1080p @ 25Hz (CEA) */
    { 1920, 1080, 30, 0, 1,  74250 }, /* VIC 50: 1920x1080p @ 30Hz (CEA) */
    { 1920, 1080, 60, 0, 1, 148500 }, /* VIC 51: 1920x1080p @ 60Hz (CEA) */
    { 1920, 1080, 60, 0, 1, 148500 }, /* VIC 52: duplicate of 51 */
    { 1920, 1080, 60, 0, 1, 148500 }, /* VIC 53: duplicate of 51 */
    { 1920, 1080, 60, 0, 1, 148500 }, /* VIC 54: duplicate of 51 */
    { 3840, 2160, 24, 0, 1, 297000 }, /* VIC 55: 3840x2160p @ 24Hz 16:9 */
    { 3840, 2160, 25, 0, 1, 297000 }, /* VIC 56: 3840x2160p @ 25Hz 16:9 */
    { 3840, 2160, 30, 0, 1, 297000 }, /* VIC 57: 3840x2160p @ 30Hz 16:9 */
    { 3840, 2160, 24, 0, 1, 297000 }, /* VIC 58: 3840x2160p @ 24Hz 16:9 */
    { 3840, 2160, 50, 0, 1, 594000 }, /* VIC 59: 3840x2160p @ 50Hz 16:9 */
    { 3840, 2160, 60, 0, 1, 594000 }, /* VIC 60: 3840x2160p @ 60Hz 16:9 */
    { 4096, 2160, 24, 0, 2, 297000 }, /* VIC 61: 4096x2160p @ 24Hz 256:135 */
    { 4096, 2160, 25, 0, 2, 297000 }, /* VIC 62: 4096x2160p @ 25Hz 256:135 */
    { 4096, 2160, 30, 0, 2, 297000 }, /* VIC 63: 4096x2160p @ 30Hz 256:135 */
    { 4096, 2160, 24, 0, 2, 297000 }, /* VIC 64: 4096x2160p @ 24Hz 256:135 */
};

#define CEA_SVD_COUNT (sizeof(cea_svd_table) / sizeof(cea_svd_table[0]))

/* ── CEA SVD timing lookup ────────────────────────────────────── */

const struct edid_cea_svd_entry *edid_cea_get_svd(uint8_t vic)
{
    /* VICs are 1-indexed in the table */
    if (vic < 1 || vic > CEA_SVD_COUNT)
        return NULL;

    return &cea_svd_table[vic - 1];
}

/* ── Parse CEA data block collection ──────────────────────────── */

/*
 * edid_parse_data_blocks — Parse CEA data block collection.
 *
 * @data:   Pointer to start of data block collection
 * @len:    Length of data block collection in bytes
 * @edid:   Output EDID data (SVDs, audio blocks, speaker fields filled)
 */
static void edid_parse_data_blocks(const uint8_t *data, int len,
                                   struct edid_data *edid)
{
    int offset = 0;

    if (!data || !edid || len <= 0)
        return;

    while (offset < len) {
        uint8_t hdr = data[offset];
        uint8_t tag = (hdr >> 5) & 0x07;
        uint8_t db_len = hdr & 0x1F;

        if (db_len == 0) {
            offset++;
            continue;
        }

        if (offset + 1 + db_len > len)
            break;

        const uint8_t *payload = &data[offset + 1];

        switch (tag) {
        case CEA_DB_VIDEO: {
            /* Short Video Descriptors */
            int count = (int)db_len;
            if (edid->num_svd + count > EDID_MAX_CEA_SVD)
                count = EDID_MAX_CEA_SVD - edid->num_svd;

            for (int i = 0; i < count; i++) {
                uint8_t svd_byte = payload[i];
                uint8_t vic = svd_byte & 0x7F;
                int native = (svd_byte & 0x80) ? 1 : 0;
                const struct edid_cea_svd_entry *t = edid_cea_get_svd(vic);

                if (!t)
                    continue;

                int idx = edid->num_svd++;
                edid->svd[idx].h_pixels    = t->h_pixels;
                edid->svd[idx].v_lines     = t->v_lines;
                edid->svd[idx].refresh     = t->refresh;
                edid->svd[idx].interlaced  = t->interlaced;
                edid->svd[idx].aspect_ratio = t->aspect_ratio;
                edid->svd[idx].pixel_clock = t->pixel_clock;
            }
            break;
        }

        case CEA_DB_AUDIO: {
            /* Short Audio Descriptors */
            int count = (int)db_len / 3; /* each audio descriptor is 3 bytes */
            if (edid->num_audio_blocks + count > EDID_MAX_AUDIO_BLOCKS)
                count = EDID_MAX_AUDIO_BLOCKS - edid->num_audio_blocks;

            for (int i = 0; i < count; i++) {
                int off = i * 3;
                int idx = edid->num_audio_blocks++;
                edid->audio_blocks[idx].format       = (payload[off] >> 3) & 0x0F;
                edid->audio_blocks[idx].channels     = (payload[off] & 0x07) + 1;
                edid->audio_blocks[idx].sample_rates = payload[off + 1];
                edid->audio_blocks[idx].sample_sizes = payload[off + 2] >> 2;
            }
            break;
        }

        case CEA_DB_SPEAKER: {
            /* Speaker Allocation Data Block (3 bytes) */
            if (db_len >= 3)
                edid->speaker_alloc = payload[0];
            break;
        }

        default:
            /* Skip unknown/unsupported data block types */
            break;
        }

        offset += 1 + db_len;
    }
}

/* ── Parse EDID extension blocks (block 1+) ───────────────────── */

int edid_parse_extensions(const uint8_t *raw, int count,
                          struct edid_data *edid)
{
    int parsed = 0;

    if (!raw || !edid || count <= 0)
        return 0;

    for (int ext = 0; ext < count; ext++) {
        const uint8_t *blk = &raw[(ext + 1) * EDID_SIZE];
        uint8_t tag = blk[0];

        if (tag == EDID_EXT_TAG_CEA) {
            uint8_t rev = blk[1];
            uint8_t dtd_offset = blk[2] & 0x3F; /* bits 4:0 = DTD offset */
            uint8_t features = blk[3];

            edid->has_cea_block = 1;
            edid->cea_revision = rev;
            edid->cea_dtd_offset = dtd_offset;
            edid->cea_num_native_dtds = (features >> 5) & 0x07;
            edid->cea_support_yuv444 = (features >> 4) & 0x01;
            edid->cea_support_yuv422 = (features >> 3) & 0x01;

            /* Parse data block collection (bytes 4 to dtd_offset-1) */
            int data_blk_len = (dtd_offset > 4) ? (dtd_offset - 4) : 0;
            if (data_blk_len > 0)
                edid_parse_data_blocks(&blk[4], data_blk_len, edid);

            /* Parse detailed timing descriptors after dtd_offset */
            if (dtd_offset >= 4 && dtd_offset < EDID_EXT_SIZE) {
                int dtd_avail = (EDID_EXT_SIZE - dtd_offset) / 18;
                if (dtd_avail > EDID_MAX_CEA_DTDS)
                    dtd_avail = EDID_MAX_CEA_DTDS;

                for (int i = 0; i < dtd_avail; i++) {
                    const uint8_t *desc = &blk[dtd_offset + i * 18];

                    /* Check if valid detailed timing (pixel clock non-zero) */
                    uint16_t clk = (uint16_t)(desc[0]) |
                                   ((uint16_t)(desc[1]) << 8);
                    if (clk == 0)
                        continue;

                    /* Parse 18-byte DTD into edid_timing */
                    struct edid_timing *t = &edid->cea_dtds[edid->num_cea_dtds];
                    uint16_t h_pixels = (uint16_t)(desc[2]) |
                                        ((uint16_t)(desc[4] & 0xF0) << 4);
                    uint16_t v_lines  = (uint16_t)(desc[5]) |
                                        ((uint16_t)(desc[7] & 0xF0) << 4);

                    t->h_pixels = h_pixels;
                    t->v_lines  = v_lines;
                    t->interlaced = (desc[17] & 0x80) ? 1 : 0;

                    /* Compute v_freq from pixel clock and dimensions */
                    uint16_t h_blank = (uint16_t)(desc[3]) |
                                       ((uint16_t)(desc[4] & 0x0F) << 8);
                    uint16_t v_blank = (uint16_t)(desc[6]) |
                                       ((uint16_t)(desc[7] & 0x0F) << 8);
                    uint16_t h_total = h_pixels + h_blank;
                    uint16_t v_total = v_lines + v_blank;

                    if (h_total > 0 && v_total > 0) {
                        uint64_t vfreq = (uint64_t)clk * 10ULL /
                                         (uint64_t)h_total /
                                         (uint64_t)v_total;
                        t->v_freq = (uint8_t)vfreq;

                        uint64_t hfreq = (uint64_t)clk * 10ULL /
                                         (uint64_t)h_total;
                        t->h_freq = (uint8_t)(hfreq / 1000);
                    }

                    edid->num_cea_dtds++;
                }
            }

            parsed++;
        }
        /* Other extension types (DTD-VTB, DI, LDI) silently ignored */
    }

    return parsed;
}

/* ── edid_find_mode — Find preferred mode matching resolution ─── */

int edid_find_mode(const void *edid_ptr, int width, int height, void *mode)
{
    const struct edid_data *edid = (const struct edid_data *)edid_ptr;
    (void)mode;

    if (!edid || !edid->valid || width <= 0 || height <= 0)
        return -1;

    /* Check preferred timing first */
    if (edid->preferred_timing.h_pixels == (uint16_t)width &&
        edid->preferred_timing.v_lines == (uint16_t)height) {
        return 0;
    }

    /* Check standard timings */
    for (int i = 0; i < 8; i++) {
        if (edid->standard_timings[i].h_pixels == (uint16_t)width &&
            edid->standard_timings[i].v_lines == (uint16_t)height) {
            return 0;
        }
    }

    /* Check CEA SVD timings */
    for (int i = 0; i < edid->num_svd; i++) {
        if (edid->svd[i].h_pixels == (uint16_t)width &&
            edid->svd[i].v_lines == (uint16_t)height) {
            return 0;
        }
    }

    /* Check CEA DTDs */
    for (int i = 0; i < edid->num_cea_dtds; i++) {
        if (edid->cea_dtds[i].h_pixels == (uint16_t)width &&
            edid->cea_dtds[i].v_lines == (uint16_t)height) {
            return 0;
        }
    }

    return -1;
}

/* ── edid_valid — Check if parsed EDID is valid ──────────────── */

int edid_valid(const void *edid_ptr)
{
    const struct edid_data *edid = (const struct edid_data *)edid_ptr;

    if (!edid_ptr)
        return 0;

    return edid->valid;
}
