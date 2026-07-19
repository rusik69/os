#ifndef ATA_IDENTIFY_H
#define ATA_IDENTIFY_H

/*
 * ata_identify.h — ATA IDENTIFY DEVICE data parsing.
 *
 * The IDENTIFY DEVICE command (0xEC) returns 256 16-bit words of
 * parameter information describing the ATA/ATAPI device.  This header
 * defines word-offset constants, bitfield masks, and helper functions
 * for extracting structured data from the raw identify buffer.
 *
 * Strings (model, serial, firmware revision) are stored in the
 * device's native byte order — for big-endian (BE) devices the bytes
 * within each word are swapped relative to the host's little-endian
 * view.  ATA-strings are always 16-bit-word-aligned with the high
 * byte of each word representing the earlier character.
 *
 * All offsets are word indices into a uint16_t[256] buffer.
 *
 * Reference: ATA/ATAPI-8 (ACS-3), T13/2161-D Revision 3.
 */

#include "types.h"

/* ====================================================================
 *  Word Offsets — ATA IDENTIFY DEVICE data
 * ==================================================================== */

/* General information */
#define ATA_ID_GENERAL          0   /* General configuration */
#define ATA_ID_LOG_CYL          1   /* Number of logical cylinders */
#define ATA_ID_LOG_HEADS        3   /* Number of logical heads */
#define ATA_ID_LOG_SECT         6   /* Number of logical sectors per track */
#define ATA_ID_SERIAL           10  /* Serial number (10 words, 20 chars) */
#define ATA_ID_BUF_TYPE         20  /* Buffer type */
#define ATA_ID_BUF_SIZE         21  /* Buffer size in sectors */
#define ATA_ID_ECC_BYTES        22  /* Number of ECC bytes */
#define ATA_ID_FW_REV           23  /* Firmware revision (4 words, 8 chars) */
#define ATA_ID_MODEL            27  /* Model number (20 words, 40 chars) */
#define ATA_ID_MULTI_SECT_MAX   47  /* Max sectors per R/W multiple */
#define ATA_ID_CAPS             49  /* Capabilities word 0 */
#define ATA_ID_CAPS2            50  /* Capabilities word 1 */
#define ATA_ID_PIO_TIMING       51  /* PIO data transfer cycle timing */
#define ATA_ID_DMA_TIMING       52  /* Single-word DMA transfer timing */
#define ATA_ID_FIELD_VALID      53  /* Field validity */
#define ATA_ID_CUR_LOG_CYL      54  /* Current logical cylinders */
#define ATA_ID_CUR_LOG_HEADS    55  /* Current logical heads */
#define ATA_ID_CUR_LOG_SECT     56  /* Current logical sectors per track */
#define ATA_ID_CUR_CAP          57  /* Current capacity (28-bit LBA) */
#define ATA_ID_MULTI_SECT_CUR   59  /* Multiple sector setting */
#define ATA_ID_LBA_CAP          60  /* Total addressable sectors (28-bit) */
#define ATA_ID_SWDMA_MODES      62  /* Single-word DMA modes */
#define ATA_ID_MWDMA_MODES      63  /* Multiword DMA modes */
#define ATA_ID_PIO_MODES        64  /* PIO transfer modes */
#define ATA_ID_MIN_MWDMA_CYCLE  65  /* Min multiword DMA cycle time (ns) */
#define ATA_ID_REC_MWDMA_CYCLE  66  /* Recommended multiword DMA cycle */
#define ATA_ID_MIN_PIO_CYCLE    67  /* Min PIO cycle time w/o flow ctrl (ns) */
#define ATA_ID_MIN_PIO_CYCLE_I  68  /* Min PIO cycle time with IORDY (ns) */
#define ATA_ID_ADD_SUPP          69  /* Additional supported (ACS-2) */
#define ATA_ID_ADD_SUPP2         70  /* Additional supported (ACS-3) */
#define ATA_ID_RSVD_71          71  /* Reserved */
#define ATA_ID_RSVD_72          72
#define ATA_ID_RSVD_73          73
#define ATA_ID_RSVD_74          74
#define ATA_ID_QUEUE_DEPTH      75  /* Maximum queue depth (NCQ) */
#define ATA_ID_SATA_CAPS        76  /* Serial ATA capabilities */
#define ATA_ID_SATA_CAPS2       77  /* Serial ATA additional capabilities */
#define ATA_ID_SATA_FEAT_SUPP   78  /* Serial ATA features supported */
#define ATA_ID_SATA_FEAT_EN     79  /* Serial ATA features enabled */
#define ATA_ID_MAJOR_VER        80  /* Major version number */
#define ATA_ID_MINOR_VER        81  /* Minor version number */
#define ATA_ID_CMD_SET_SUPP     82  /* Command set supported */
#define ATA_ID_CMD_SET_SUPP2    83  /* Command sets supported (cont.) */
#define ATA_ID_CMD_SET_EN       84  /* Command sets enabled */
#define ATA_ID_CMD_SET_EN2      85  /* Command sets enabled (cont.) */
#define ATA_ID_CMD_SET_DFLT     86  /* Command sets default */
#define ATA_ID_CMD_SET_DFLT2    87  /* Command sets default (cont.) */
#define ATA_ID_UDMA_MODES       88  /* Ultra DMA modes */
#define ATA_ID_ERASE_TIME       89  /* Time for security erase completion */
#define ATA_ID_ENH_ERASE_TIME   90  /* Time for enhanced erase completion */
#define ATA_ID_APM_LEVEL        91  /* Advanced power management level */
#define ATA_ID_MASTER_PWD       92  /* Master password identifier */
#define ATA_ID_HW_RESET         93  /* Hardware reset result */
#define ATA_ID_ACOUSTIC         94  /* Acoustic management values */
#define ATA_ID_STREAM_MIN_REQ   95  /* Streaming minimum request size */
#define ATA_ID_STREAM_XFER_TIME 96  /* Streaming transfer time */
#define ATA_ID_STREAM_ACC_LAT   97  /* Streaming access latency */
#define ATA_ID_STREAM_PERF      98  /* Streaming performance granularity */
#define ATA_ID_STREAM_RSVD      99  /* Streaming reserved */
#define ATA_ID_LBA48_CAP        100 /* LBA48 sector count (4 words, 64-bit) */
#define ATA_ID_LBA48_CAP2       104 /* LBA48 sector count extra */
#define ATA_ID_RSVD_105         105
#define ATA_ID_RSVD_106         106
#define ATA_ID_RSVD_107         107
#define ATA_ID_WWN              108 /* World Wide Name (4 words, 64-bit) */
#define ATA_ID_RSVD_112         112
#define ATA_ID_RSVD_113         113
#define ATA_ID_RSVD_114         114
#define ATA_ID_RSVD_115         115
#define ATA_ID_LOGICAL_SECT     116 /* Logical sector size (16-bit) */
#define ATA_ID_LOGICAL_ALIGN    117 /* Logical sector alignment offset */
#define ATA_ID_CMD_SET_SUPP3    119 /* Command sets supported (extended) */
#define ATA_ID_CMD_SET_EN3      120 /* Command sets enabled (extended) */
#define ATA_ID_RSVD_121         121
#define ATA_ID_RSVD_122         122
#define ATA_ID_RSVD_123         123
#define ATA_ID_RSVD_124         124
#define ATA_ID_RSVD_125         125
#define ATA_ID_RSVD_126         126
#define ATA_ID_RSVD_127         127
#define ATA_ID_SECURITY         128 /* Device security status */
#define ATA_ID_VENDOR_129       129 /* Vendor-specific */
#define ATA_ID_VENDOR_159       159 /* Vendor-specific (end) */
#define ATA_ID_CFA_POWER        160 /* CFA power mode */
#define ATA_ID_CFA_POWER2       161 /* CFA power mode (cont.) */
#define ATA_ID_RSVD_162         162
#define ATA_ID_RSVD_163         163
#define ATA_ID_RSVD_164         164
#define ATA_ID_RSVD_165         165
#define ATA_ID_RSVD_166         166
#define ATA_ID_RSVD_167         167
#define ATA_ID_FORM_FACTOR      168 /* Device nominal form factor */
#define ATA_ID_DSM_TRIM         169 /* DATA SET MANAGEMENT (TRIM) support */
#define ATA_ID_RSVD_170         170
#define ATA_ID_RSVD_171         171
#define ATA_ID_RSVD_172         172
#define ATA_ID_RSVD_173         173
#define ATA_ID_RSVD_174         174
#define ATA_ID_RSVD_175         175
#define ATA_ID_RSVD_176         176
#define ATA_ID_RSVD_177         177
#define ATA_ID_TRIM_SECT_COUNT  178 /* Max sectors per TRIM (2 words) */
#define ATA_ID_RSVD_180         180
#define ATA_ID_RSVD_181         181
#define ATA_ID_RSVD_182         182
#define ATA_ID_RSVD_183         183
#define ATA_ID_RSVD_184         184
#define ATA_ID_RSVD_185         185
#define ATA_ID_RSVD_186         186
#define ATA_ID_RSVD_187         187
#define ATA_ID_ROTATION_RATE    188 /* Nominal media rotation rate */
#define ATA_ID_RSVD_189         189
#define ATA_ID_RSVD_190         190
#define ATA_ID_RSVD_191         191
#define ATA_ID_RSVD_192         192
#define ATA_ID_RSVD_193         193
#define ATA_ID_RSVD_194         194
#define ATA_ID_RSVD_195         195
#define ATA_ID_RSVD_196         196
#define ATA_ID_RSVD_197         197
#define ATA_ID_RSVD_198         198
#define ATA_ID_RSVD_199         199
#define ATA_ID_RSVD_200         200
#define ATA_ID_RSVD_201         201
#define ATA_ID_RSVD_202         202
#define ATA_ID_RSVD_203         203
#define ATA_ID_RSVD_204         204
#define ATA_ID_RSVD_205         205
#define ATA_ID_RSVD_206         206
#define ATA_ID_RSVD_207         207
#define ATA_ID_RSVD_208         208
#define ATA_ID_RSVD_209         209
#define ATA_ID_RSVD_210         210
#define ATA_ID_RSVD_211         211
#define ATA_ID_RSVD_212         212
#define ATA_ID_RSVD_213         213
#define ATA_ID_RSVD_214         214
#define ATA_ID_RSVD_215         215
#define ATA_ID_RSVD_216         216
#define ATA_ID_RSVD_217         217
#define ATA_ID_RSVD_218         218
#define ATA_ID_RSVD_219         219
#define ATA_ID_RSVD_220         220
#define ATA_ID_RSVD_221         221
#define ATA_ID_TRANSPORT        222 /* Transport major version */
#define ATA_ID_RSVD_223         223
#define ATA_ID_RSVD_224         224
#define ATA_ID_RSVD_225         225
#define ATA_ID_RSVD_226         226
#define ATA_ID_RSVD_227         227
#define ATA_ID_RSVD_228         228
#define ATA_ID_RSVD_229         229
#define ATA_ID_EXT_LBA_CAP      230 /* Extended LBA capacity (4 words) */
#define ATA_ID_RSVD_234         234
#define ATA_ID_RSVD_254         254
#define ATA_ID_CRC              255 /* Integrity CRC */

/* ====================================================================
 *  Field-Validity Masks (Word 53)
 * ==================================================================== */
#define ATA_IDV_CUR_LOG         (1u << 0)  /* Words 54–58 valid */
#define ATA_IDV_RESERVED        (1u << 1)  /* Reserved */
#define ATA_IDV_SERIAL          (1u << 2)  /* Words 88 and 64–70 valid */
#define ATA_IDV_88_6470         ATA_IDV_SERIAL

/* ====================================================================
 *  Capabilities — Word 49 (ATA_ID_CAPS)
 * ==================================================================== */
#define ATA_CAP_STANDBY         (1u << 0)  /* Standby timer values */
#define ATA_CAP_IORDY           (1u << 11) /* IORDY supported */
#define ATA_CAP_IORDY_DIS       (1u << 10) /* IORDY may be disabled */
#define ATA_CAP_LBA             (1u << 9)  /* LBA supported */
#define ATA_CAP_DMA             (1u << 8)  /* DMA supported */

/* ====================================================================
 *  Major Version — Word 80 (ATA_ID_MAJOR_VER)
 * ==================================================================== */
#define ATA_VER_ATA1            (1u << 0)  /* ATA-1 */
#define ATA_VER_ATA2            (1u << 1)  /* ATA-2 */
#define ATA_VER_ATA3            (1u << 2)  /* ATA-3 */
#define ATA_VER_ATA4            (1u << 3)  /* ATA/ATAPI-4 */
#define ATA_VER_ATA5            (1u << 4)  /* ATA/ATAPI-5 */
#define ATA_VER_ATA6            (1u << 5)  /* ATA/ATAPI-6 */
#define ATA_VER_ATA7            (1u << 6)  /* ATA/ATAPI-7 */
#define ATA_VER_ATA8            (1u << 7)  /* ATA/ATAPI-8-ACS */
#define ATA_VER_ATA8_ACS2       (1u << 8)  /* ATA/ATAPI-8-ACS-2 */
#define ATA_VER_ATA8_ACS3       (1u << 9)  /* ATA/ATAPI-8-ACS-3 */

/* ====================================================================
 *  Transport Major Version — Word 222 (ATA_ID_TRANSPORT)
 * ==================================================================== */
#define ATA_TRANSPORT_PARALLEL  (1u << 0)  /* Parallel ATA */
#define ATA_TRANSPORT_SERIAL    (1u << 1)  /* Serial ATA */

/* ====================================================================
 *  Serial ATA Capabilities — Word 76 (ATA_ID_SATA_CAPS)
 * ==================================================================== */
#define ATA_SATA_GEN1           (1u << 0)  /* Gen1 (1.5 Gbps) */
#define ATA_SATA_GEN2           (1u << 1)  /* Gen2 (3.0 Gbps) */
#define ATA_SATA_GEN3           (1u << 2)  /* Gen3 (6.0 Gbps) */
#define ATA_SATA_NCQ            (1u << 8)  /* NCQ supported */
#define ATA_SATA_RECV_NCQ       (1u << 9)  /* Receipt of NCQ log */
#define ATA_SATA_NCQ_PRIO      (1u << 12) /* NCQ priority supported */
#define ATA_SATA_NCQ_UNLOAD    (1u << 13) /* NCQ non-data unload */

/* ====================================================================
 *  Command Sets — Word 82 (ATA_ID_CMD_SET_SUPP)
 * ==================================================================== */
#define ATA_CMD_SMART           (1u << 0)  /* SMART */
#define ATA_CMD_SECURITY        (1u << 1)  /* Security mode */
#define ATA_CMD_REMOVABLE       (1u << 2)  /* Removable media */
#define ATA_CMD_POWER_MGMT      (1u << 3)  /* Power management */
#define ATA_CMD_PACKET          (1u << 4)  /* Packet command (ATAPI) */
#define ATA_CMD_WRITE_DMA       (1u << 6)  /* Write DMA */
#define ATA_CMD_FLUSH_CACHE     (1u << 10) /* Write cache flush */
#define ATA_CMD_LOOK_AHEAD      (1u << 11) /* Read look-ahead */
#define ATA_CMD_FEATURE_EN      (1u << 12) /* Feature enable/disable */

/* Word 83 — Command sets supported (cont.) */
#define ATA_CMD_LBA48           (1u << 10) /* 48-bit LBA */
#define ATA_CMD_DEVICE_LOG      (1u << 12) /* Device log */

/* Word 84 — Command sets supported (cont.) */
#define ATA_CMD_DOWNLOAD_MICRO  (1u << 4)  /* Download microcode */
#define ATA_CMD_DOWNLOAD_MICRO2 (1u << 5)  /* Download microcode (DMA) */

/* ====================================================================
 *  Command Sets Enabled — Word 85 (ATA_ID_CMD_SET_EN)
 * ==================================================================== */
#define ATA_EN_SMART            (1u << 0)
#define ATA_EN_SECURITY         (1u << 1)
#define ATA_EN_REMOVABLE        (1u << 2)
#define ATA_EN_POWER_MGMT       (1u << 3)
#define ATA_EN_PACKET           (1u << 4)
#define ATA_EN_WRITE_DMA        (1u << 6)
#define ATA_EN_FLUSH_CACHE      (1u << 10)
#define ATA_EN_LOOK_AHEAD       (1u << 11)
#define ATA_EN_FEATURE_EN       (1u << 12)

/* ====================================================================
 *  Ultra DMA Modes — Word 88 (ATA_ID_UDMA_MODES)
 * ==================================================================== */
#define ATA_UDMA_MODE0          (1u << 0)
#define ATA_UDMA_MODE1          (1u << 1)
#define ATA_UDMA_MODE2          (1u << 2)
#define ATA_UDMA_MODE3          (1u << 3)
#define ATA_UDMA_MODE4          (1u << 4)
#define ATA_UDMA_MODE5          (1u << 5)
#define ATA_UDMA_MODE6          (1u << 6)
#define ATA_UDMA_CURR_MASK      0x00FF  /* Current UDMA mode bits (8-13) */
#define ATA_UDMA_CURR_SHIFT     8
#define ATA_UDMA_MODE0_CURR     (1u << 8)
#define ATA_UDMA_MODE1_CURR     (1u << 9)
#define ATA_UDMA_MODE2_CURR     (1u << 10)
#define ATA_UDMA_MODE3_CURR     (1u << 11)
#define ATA_UDMA_MODE4_CURR     (1u << 12)
#define ATA_UDMA_MODE5_CURR     (1u << 13)
#define ATA_UDMA_MODE6_CURR     (1u << 14)

/* ====================================================================
 *  PIO Modes — Word 64 (ATA_ID_PIO_MODES)
 * ==================================================================== */
#define ATA_PIO_MODE3           (1u << 0)
#define ATA_PIO_MODE4           (1u << 1)
#define ATA_PIO_MODE_MASK       0x00FF

/* ====================================================================
 *  Multiword DMA Modes — Word 63 (ATA_ID_MWDMA_MODES)
 * ==================================================================== */
#define ATA_MWDMA_MODE0         (1u << 0)
#define ATA_MWDMA_MODE1         (1u << 1)
#define ATA_MWDMA_MODE2         (1u << 2)
#define ATA_MWDMA_CURR_MASK     0x00FF
#define ATA_MWDMA_CURR_SHIFT    8

/* ====================================================================
 *  General Configuration — Word 0 (ATA_ID_GENERAL)
 * ==================================================================== */
#define ATA_GEN_ATAPI           (1u << 15) /* 1=ATAPI, 0=ATA */
#define ATA_GEN_REMOVABLE       (1u << 7)  /* Removable media device */
#define ATA_GEN_NONMAGNETIC     (1u << 6)  /* Non-magnetic device */

/* ====================================================================
 *  Security Status — Word 128 (ATA_ID_SECURITY)
 * ==================================================================== */
#define ATA_SEC_SUPPORTED       (1u << 0)
#define ATA_SEC_ENABLED         (1u << 1)
#define ATA_SEC_LOCKED          (1u << 2)
#define ATA_SEC_FROZEN          (1u << 3)
#define ATA_SEC_COUNT_EXP       (1u << 4)
#define ATA_SEC_LEVEL           (1u << 5)  /* 1=maximum, 0=high */

/* ====================================================================
 *  DSM TRIM — Word 169 (ATA_ID_DSM_TRIM)
 * ==================================================================== */
#define ATA_DSM_TRIM_SUPP       (1u << 0)
#define ATA_DSM_TRIM_DMA        (1u << 1)  /* DMA support */

/* ====================================================================
 *  Rotation Rate — Word 188 (ATA_ID_ROTATION_RATE)
 * ==================================================================== */
#define ATA_RPM_SSD             1          /* Non-rotating (SSD) */

/* ====================================================================
 *  Form Factor — Word 168 (ATA_ID_FORM_FACTOR)
 * ==================================================================== */
#define ATA_FORM_5_25           0x0000     /* 5.25 inch */
#define ATA_FORM_3_5            0x0001     /* 3.5 inch */
#define ATA_FORM_2_5            0x0002     /* 2.5 inch */
#define ATA_FORM_1_8            0x0003     /* 1.8 inch */
#define ATA_FORM_LESS_1_8       0x0004     /* < 1.8 inch */
#define ATA_FORM_M2             0x0005     /* M.2 */
#define ATA_FORM_CF             0x0006     /* CompactFlash */
#define ATA_FORM_MICRO_SSD      0x0007     /* Micro SSD */

/* ====================================================================
 *  Acoustic Management — Word 94 (ATA_ID_ACOUSTIC)
 * ==================================================================== */
#define ATA_ACOUSTIC_CUR(val)   ((val) & 0x00FF)    /* Current level */
#define ATA_ACOUSTIC_REC(val)   (((val) >> 8) & 0x00FF)  /* Recommended */

/* ====================================================================
 *  Queue Depth — Word 75 (ATA_ID_QUEUE_DEPTH)
 * ==================================================================== */
#define ATA_QUEUE_DEPTH(val)    ((val) & 0x001F)  /* NCQ queue depth (1-32) */

/* ====================================================================
 *  String extraction helpers
 * ==================================================================== */

/**
 * ata_id_string — Extract an ATA string from the identify buffer.
 * @ident:   Pointer to raw IDENTIFY data (256 uint16_t words)
 * @offset:  Word offset of the start of the string
 * @len:     Number of words in the string field
 * @buf:     Output buffer (minimum len*2 + 1 bytes)
 *
 * ATA strings are stored as big-endian characters (high byte of each
 * word is the earlier character).  This function converts to a standard
 * null-terminated C string, swapping bytes and stripping trailing spaces.
 */
void ata_id_string(const uint16_t *ident, unsigned int offset,
                   unsigned int len, char *buf);

/**
 * ata_id_get_string — Return a pointer to a static buffer with the string.
 * @ident:   Raw IDENTIFY data
 * @offset:  Word offset
 * @len:     Number of words
 *
 * Returns a pointer to a static buffer containing the null-terminated
 * string.  The buffer is reused on each call — copy the result if you
 * need to keep it across calls.
 */
const char *ata_id_get_string(const uint16_t *ident, unsigned int offset,
                              unsigned int len);

/* ====================================================================
 *  Parsed ATA device information structure
 * ==================================================================== */

/** Maximum ATA string length (including null terminator) */
#define ATA_ID_SERIAL_LEN    21   /* 20 chars + NUL */
#define ATA_ID_FW_REV_LEN    9    /* 8 chars + NUL */
#define ATA_ID_MODEL_LEN     41   /* 40 chars + NUL */

/** Maximum transport string length */
#define ATA_ID_TRANSPORT_LEN 16

struct ata_device_info {
    /* Identity strings */
    char serial[ATA_ID_SERIAL_LEN];
    char firmware[ATA_ID_FW_REV_LEN];
    char model[ATA_ID_MODEL_LEN];
    char transport[ATA_ID_TRANSPORT_LEN];

    /* Capacity */
    uint64_t lba28_sectors;   /* 28-bit LBA capacity (words 60-61) */
    uint64_t lba48_sectors;   /* 48-bit LBA capacity (words 100-103) */
    uint64_t lba_total;       /* Total sectors (prefers LBA48) */

    /* Device type */
    int is_atapi;             /* Non-zero if ATAPI device */
    int is_removable;         /* Non-zero if removable media */

    /* Capabilities */
    int lba_supported;        /* LBA supported */
    int dma_supported;        /* DMA supported */
    int iordy_supported;      /* IORDY supported */
    int flush_supported;      /* FLUSH CACHE supported */
    int smart_supported;      /* SMART supported */
    int security_supported;   /* Security mode supported */
    int lba48_supported;      /* 48-bit LBA supported */
    int trim_supported;       /* DATA SET MANAGEMENT (TRIM) */
    int ncq_supported;        /* Native Command Queuing */

    /* Transfer modes */
    int udma_current;         /* Current UDMA mode (0-6, -1 if none) */
    int mwdma_current;        /* Current multiword DMA mode (0-2, -1 if none) */
    int pio_mode_current;     /* Current PIO mode (0-4) */

    /* SATA generation */
    int sata_gen;             /* 1, 2, 3, or 0 */

    /* Physical characteristics */
    int rotation_rate;        /* RPM (1 = SSD) */
    int form_factor;          /* ATA_FORM_* value */

    /* World Wide Name */
    uint64_t wwn;

    /* Acoustic management */
    int acoustic_current;     /* 0-254, -1 if not available */
    int acoustic_recommended; /* 0-254, -1 if not available */

    /* Logical sector size */
    int logical_sector_size;  /* Bytes per logical sector (default 512) */

    /* NCQ queue depth */
    int queue_depth;          /* 1-32 */

    /* Security */
    int security_enabled;
    int security_locked;

    /* Major version string index */
    unsigned int major_version_bits;
};

/* ====================================================================
 *  Parsing and display functions
 * ==================================================================== */

/**
 * ata_id_parse — Parse raw IDENTIFY data into structured format.
 * @ident:  Raw 256-word IDENTIFY buffer
 * @info:   Output structure (caller-allocated)
 *
 * Returns 0 on success, -EINVAL if ident or info is NULL.
 */
int ata_id_parse(const uint16_t *ident, struct ata_device_info *info);

/**
 * ata_id_print_kernel — Log parsed identify info via kprintf.
 * @info:  Parsed device info
 * @prefix: Optional prefix string (e.g. "ata0" or "ahci0"), may be NULL
 *
 * Prints a human-readable summary to the kernel log.
 */
void ata_id_print_kernel(const struct ata_device_info *info,
                         const char *prefix);

/**
 * ata_id_print_detailed — Log detailed identify data (all fields).
 * @ident:  Raw IDENTIFY data
 * @prefix: Optional prefix
 *
 * Calls ata_id_parse internally and prints every parsed field.
 */
void ata_id_print_detailed(const uint16_t *ident, const char *prefix);

/* ====================================================================
 *  Utility helpers
 * ==================================================================== */

/**
 * ata_id_is_atapi — Quick check if device is ATAPI.
 * Returns non-zero if the device is a packet (ATAPI) device.
 */
static inline int ata_id_is_atapi(const uint16_t *ident)
{
    return !!(ident[ATA_ID_GENERAL] & ATA_GEN_ATAPI);
}

/**
 * ata_id_sector_count — Get the optimal sector count from identify data.
 * Prefers LBA48 if available, falls back to 28-bit.
 */
static inline uint64_t ata_id_sector_count(const uint16_t *ident)
{
    uint64_t lba48 = ((uint64_t)ident[ATA_ID_LBA48_CAP] |
                     ((uint64_t)ident[ATA_ID_LBA48_CAP + 1] << 16) |
                     ((uint64_t)ident[ATA_ID_LBA48_CAP + 2] << 32) |
                     ((uint64_t)ident[ATA_ID_LBA48_CAP + 3] << 48));
    if (lba48)
        return lba48;
    return ident[ATA_ID_LBA_CAP] | ((uint64_t)ident[ATA_ID_LBA_CAP + 1] << 16);
}

/**
 * ata_id_sata_gen — Get SATA generation as integer (1, 2, 3, or 0).
 */
static inline int ata_id_sata_gen(const uint16_t *ident)
{
    uint16_t caps = ident[ATA_ID_SATA_CAPS];
    if (caps & ATA_SATA_GEN3) return 3;
    if (caps & ATA_SATA_GEN2) return 2;
    if (caps & ATA_SATA_GEN1) return 1;
    return 0;
}

/**
 * ata_id_udma_mode — Get current UDMA mode number (0-6) or -1.
 */
static inline int ata_id_udma_mode(const uint16_t *ident)
{
    uint16_t udma = ident[ATA_ID_UDMA_MODES];
    uint16_t curr = (udma >> 8) & 0x7F;
    int i;
    if (!curr)
        return -1;
    for (i = 0; i < 7; i++) {
        if (curr & (1u << i))
            return i;
    }
    return -1;
}

/**
 * ata_id_mwdma_mode — Get current multiword DMA mode (0-2) or -1.
 */
static inline int ata_id_mwdma_mode(const uint16_t *ident)
{
    uint16_t mwdma = ident[ATA_ID_MWDMA_MODES];
    uint16_t curr = (mwdma >> 8) & 0x07;
    int i;
    if (!curr)
        return -1;
    for (i = 0; i < 3; i++) {
        if (curr & (1u << i))
            return i;
    }
    return -1;
}

/**
 * ata_id_ncq_supported — Check if NCQ is supported.
 */
static inline int ata_id_ncq_supported(const uint16_t *ident)
{
    return !!(ident[ATA_ID_SATA_CAPS] & ATA_SATA_NCQ);
}

/**
 * ata_id_queue_depth — Get NCQ queue depth (1-32).
 */
static inline int ata_id_queue_depth(const uint16_t *ident)
{
    int depth = ident[ATA_ID_QUEUE_DEPTH] & 0x1F;
    return depth ? depth : 1;
}

/**
 * ata_id_rotation_rate — Get rotation rate (RPM) or 0 on invalid.
 */
static inline int ata_id_rotation_rate(const uint16_t *ident)
{
    return ident[ATA_ID_ROTATION_RATE];
}

/**
 * ata_id_trim_supported — Check if TRIM (DATA SET MANAGEMENT) is supported.
 */
static inline int ata_id_trim_supported(const uint16_t *ident)
{
    return !!(ident[ATA_ID_DSM_TRIM] & ATA_DSM_TRIM_SUPP);
}

/**
 * ata_id_wwn — Extract World Wide Name.
 */
static inline uint64_t ata_id_wwn(const uint16_t *ident)
{
    return ((uint64_t)ident[ATA_ID_WWN] |
            ((uint64_t)ident[ATA_ID_WWN + 1] << 16) |
            ((uint64_t)ident[ATA_ID_WWN + 2] << 32) |
            ((uint64_t)ident[ATA_ID_WWN + 3] << 48));
}

/* ====================================================================
 *  IDENTIFY data signature validation
 * ==================================================================== */

/**
 * ata_id_check_signature — Validate IDENTIFY data has a plausible signature.
 * @ident: Raw 256-word IDENTIFY DEVICE data buffer
 *
 * Returns 0 if the identify data appears valid, or -EINVAL if the buffer
 * is NULL, contains an all-zeros or all-ones word 0 (uninitialized/bus
 * floating), or the ATAPI signature check fails.
 *
 * Call this before parsing or consuming raw IDENTIFY data to guard against
 * parsing garbage from a missing, failed, or in-progress IDENTIFY command.
 */
static inline int ata_id_check_signature(const uint16_t *ident)
{
    if (!ident)
        return -EINVAL;

    /* Word 0 (General configuration) must not be all-zeros or all-ones.
     * 0x0000 = uninitialized / data not ready
     * 0xFFFF = bus floating / device absent */
    if (ident[ATA_ID_GENERAL] == 0x0000 ||
        ident[ATA_ID_GENERAL] == 0xFFFF)
        return -EINVAL;

    /* For ATAPI devices (word 0 bit 15 set), the identify signature in
     * words 2-4 must be valid per the ATA/ATAPI spec:
     *   word 2 = 0xEB14, word 3 = 0x0000, word 4 = 0x0000
     * (Per the ATA-8 ACS specification section 7.18.7.6) */
    if (ident[ATA_ID_GENERAL] & ATA_GEN_ATAPI) {
        if (ident[2] != 0xEB14 && ident[2] != 0x0000)
            return -EINVAL;
        if (ident[3] != 0x0000 || ident[4] != 0x0000)
            return -EINVAL;
    }

    return 0;
}

#endif /* ATA_IDENTIFY_H */
