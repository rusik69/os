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

/* CEA-861 Short Video Descriptor timing entry */
struct edid_cea_svd_entry {
    uint16_t h_pixels;
    uint16_t v_lines;
    uint8_t  refresh;       /* refresh rate in Hz */
    uint8_t  interlaced;    /* 1 if interlaced */
    uint8_t  aspect_ratio;  /* 0=4:3, 1=16:9, 2=64:27, 3=256:135 */
    uint32_t pixel_clock;   /* pixel clock in kHz */
};

/* CEA audio data block entry */
struct edid_audio_entry {
    uint8_t  format;        /* 1=PCM, 2=AC-3, 3=MPEG1, 4=MP3, 5=MPEG2, 6=AAC, 7=DTS, 8=ATRAC */
    uint8_t  channels;      /* max number of channels */
    uint8_t  sample_rates;  /* bitmask: bit0=32kHz bit1=44.1kHz bit2=48kHz bit3=88.2kHz bit4=96kHz bit5=176.4kHz bit6=192kHz */
    uint8_t  sample_sizes;  /* bitmask: bit0=16-bit bit1=20-bit bit2=24-bit (PCM only) */
};

#define EDID_MAX_CEA_SVD      32
#define EDID_MAX_CEA_DTDS      6
#define EDID_MAX_AUDIO_BLOCKS  8

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

    /* Extension block data (CEA-861 block 1 and beyond) */
    int      extension_count;         /* from byte 126 of block 0 */
    int      has_cea_block;           /* 1 if CEA extension was found */
    uint8_t  cea_revision;            /* CEA revision (3 for CEA-861B, etc.) */
    uint8_t  cea_dtd_offset;          /* byte offset of DTDs within CEA block */
    uint8_t  cea_num_native_dtds;     /* number of DTDs marked native */
    uint8_t  cea_support_yuv444;      /* YCbCr 4:4:4 supported */
    uint8_t  cea_support_yuv422;      /* YCbCr 4:2:2 supported */

    /* Short video descriptors parsed from CEA data blocks */
    int                    num_svd;
    struct edid_cea_svd_entry svd[EDID_MAX_CEA_SVD];

    /* Additional detailed timing descriptors from CEA extension */
    int                    num_cea_dtds;
    struct edid_timing     cea_dtds[EDID_MAX_CEA_DTDS];

    /* Audio data blocks */
    int                      num_audio_blocks;
    struct edid_audio_entry  audio_blocks[EDID_MAX_AUDIO_BLOCKS];

    /* Speaker allocation data (bitmask) */
    uint8_t  speaker_alloc;  /* bit0=FL/FR, bit1=LFE, bit2=FC, bit3=RL/RR, bit4=RC, bit5=FLC/FRC, bit6=RLC/RRC */
};

/* API */
int  edid_parse(const uint8_t *raw, struct edid_data *edid);
int  edid_validate_checksum(const uint8_t *raw);
void edid_print_info(const struct edid_data *edid);

/* Extension block parsing */
int  edid_parse_extensions(const uint8_t *raw, int count, struct edid_data *edid);

/* CEA-861 SVD timing lookup */
const struct edid_cea_svd_entry *edid_cea_get_svd(uint8_t vic);

#endif /* EDID_H */
