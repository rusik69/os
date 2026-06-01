#ifndef EDID_H
#define EDID_H

#include "types.h"

#define EDID_SIZE       128
#define EDID_EXT_SIZE   128

/* EDID timing modes */
struct edid_timing {
    uint16_t h_pixels;
    uint16_t v_lines;
    uint8_t  h_freq;     /* Horizontal frequency in kHz */
    uint8_t  v_freq;     /* Vertical frequency in Hz */
    uint8_t  interlaced;
};

/* Display descriptor (detailed timing or monitor descriptor) */
struct edid_display_desc {
    uint8_t  tag;
    char     text[14];
};

/* EDID parsed data */
struct edid_data {
    int      valid;
    char     manufacturer[4];
    uint16_t product_code;
    uint32_t serial;
    uint8_t  week;
    uint8_t  year;
    uint8_t  edid_version;
    uint8_t  edid_revision;
    uint8_t  video_input_type;
    uint8_t  max_h_size;   /* cm */
    uint8_t  max_v_size;   /* cm */
    uint8_t  gamma;
    uint8_t  features;
    uint16_t red_x, red_y;
    uint16_t green_x, green_y;
    uint16_t blue_x, blue_y;
    uint16_t white_x, white_y;
    struct edid_timing standard_timings[8];
    struct edid_timing preferred_timing;
    struct edid_display_desc descriptors[4];
};

/* API */
int  edid_parse(const uint8_t *raw, struct edid_data *edid);
int  edid_validate_checksum(const uint8_t *raw);
void edid_print_info(const struct edid_data *edid);

#endif /* EDID_H */
