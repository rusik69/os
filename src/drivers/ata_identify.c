/*
 * ata_identify.c — ATA IDENTIFY DEVICE data parsing and display.
 *
 * Provides structured parsing of the 256-word IDENTIFY DEVICE response
 * (ATA command 0xEC) into a human-readable form, with helpers for
 * extracting string fields (model, serial, firmware revision),
 * capacity information, transfer mode capabilities, and device features.
 *
 * The raw IDENTIFY data is an array of 256 uint16_t words.  Strings are
 * byte-swapped (big-endian character pairs stored in little-endian words).
 */

#include "ata_identify.h"
#include "string.h"
#include "printf.h"

/* ====================================================================
 *  String extraction
 * ==================================================================== */

void ata_id_string(const uint16_t *ident, unsigned int offset,
                   unsigned int len, char *buf)
{
    unsigned int i;
    unsigned int idx;

    if (!ident || !buf || offset >= 256 || len > 256 - offset) {
        if (buf)
            buf[0] = '\0';
        return;
    }

    for (i = 0; i < len; i++) {
        idx = i * 2;
        if (idx + 1 >= len * 2)
            break;
        /* ATA strings store the high byte (first character) in the
         * low-order byte of the word from the host's perspective
         * (byte-swapped little-endian).  Swap bytes. */
        buf[idx]     = (char)(ident[offset + i] >> 8);
        buf[idx + 1] = (char)(ident[offset + i] & 0xFF);
    }
    buf[len * 2] = '\0';

    /* Strip trailing spaces */
    {
        int slen = (int)(len * 2) - 1;

        while (slen >= 0 && (buf[slen] == ' ' || buf[slen] == '\0'))
            buf[slen--] = '\0';
    }
}

static char g_id_string_buf[ATA_ID_MODEL_LEN];  /* largest string */

const char *ata_id_get_string(const uint16_t *ident, unsigned int offset,
                              unsigned int len)
{
    ata_id_string(ident, offset, len, g_id_string_buf);
    return g_id_string_buf;
}

/* ====================================================================
 *  Major version to human-readable string
 * ==================================================================== */

static const char *ata_major_version_str(unsigned int bits)
{
    if (bits & ATA_VER_ATA8_ACS3) return "ATA8-ACS3";
    if (bits & ATA_VER_ATA8_ACS2) return "ATA8-ACS2";
    if (bits & ATA_VER_ATA8)      return "ATA8-ACS";
    if (bits & ATA_VER_ATA7)      return "ATA/ATAPI-7";
    if (bits & ATA_VER_ATA6)      return "ATA/ATAPI-6";
    if (bits & ATA_VER_ATA5)      return "ATA/ATAPI-5";
    if (bits & ATA_VER_ATA4)      return "ATA/ATAPI-4";
    if (bits & ATA_VER_ATA3)      return "ATA-3";
    if (bits & ATA_VER_ATA2)      return "ATA-2";
    if (bits & ATA_VER_ATA1)      return "ATA-1";
    return "Unknown";
}

/* ====================================================================
 *  Transfer mode name helpers
 * ==================================================================== */

static const char *udma_mode_name(int mode)
{
    switch (mode) {
    case 0:  return "UDMA/33";
    case 1:  return "UDMA/33";
    case 2:  return "UDMA/66";
    case 3:  return "UDMA/100";
    case 4:  return "UDMA/133";
    case 5:  return "UDMA/166";
    case 6:  return "UDMA/200";
    default: return "UDMA/?";
    }
}

static const char *mwdma_mode_name(int mode)
{
    switch (mode) {
    case 0:  return "MWDMA0 (16.7 MB/s)";
    case 1:  return "MWDMA1 (25.0 MB/s)";
    case 2:  return "MWDMA2 (33.3 MB/s)";
    default: return "MWDMA/?";
    }
}

/* ====================================================================
 *  Form factor string
 * ==================================================================== */

static const char *ata_form_factor_str(int ff)
{
    switch (ff) {
    case ATA_FORM_5_25:      return "5.25\"";
    case ATA_FORM_3_5:       return "3.5\"";
    case ATA_FORM_2_5:       return "2.5\"";
    case ATA_FORM_1_8:       return "1.8\"";
    case ATA_FORM_LESS_1_8:  return "<1.8\"";
    case ATA_FORM_M2:        return "M.2";
    case ATA_FORM_CF:        return "CompactFlash";
    case ATA_FORM_MICRO_SSD: return "Micro SSD";
    default:                 return "Unknown";
    }
}

/* ====================================================================
 *  ata_id_parse — Parse raw IDENTIFY data
 * ==================================================================== */

int ata_id_parse(const uint16_t *ident, struct ata_device_info *info)
{
    uint64_t lba48;
    uint16_t cmd_supp;
    uint16_t feature_en;

    if (!ident || !info)
        return -EINVAL;

    memset(info, 0, sizeof(*info));

    /* Extract strings */
    ata_id_string(ident, ATA_ID_SERIAL, 10, info->serial);
    ata_id_string(ident, ATA_ID_FW_REV, 4,  info->firmware);
    ata_id_string(ident, ATA_ID_MODEL,  20, info->model);

    /* Device type */
    info->is_atapi     = ata_id_is_atapi(ident);
    info->is_removable = !!(ident[ATA_ID_GENERAL] & ATA_GEN_REMOVABLE);

    /* Capacity */
    info->lba28_sectors = (uint64_t)ident[ATA_ID_LBA_CAP] |
                          ((uint64_t)ident[ATA_ID_LBA_CAP + 1] << 16);
    lba48 = (uint64_t)ident[ATA_ID_LBA48_CAP] |
            ((uint64_t)ident[ATA_ID_LBA48_CAP + 1] << 16) |
            ((uint64_t)ident[ATA_ID_LBA48_CAP + 2] << 32) |
            ((uint64_t)ident[ATA_ID_LBA48_CAP + 3] << 48);
    info->lba48_sectors = lba48;
    info->lba_total     = lba48 ? lba48 : info->lba28_sectors;

    /* Basic capabilities from word 49 */
    info->lba_supported  = !!(ident[ATA_ID_CAPS] & ATA_CAP_LBA);
    info->dma_supported  = !!(ident[ATA_ID_CAPS] & ATA_CAP_DMA);
    info->iordy_supported = !!(ident[ATA_ID_CAPS] & ATA_CAP_IORDY);

    /* Command sets supported (words 82-83) */
    cmd_supp = ident[ATA_ID_CMD_SET_SUPP];
    info->smart_supported    = !!(cmd_supp & ATA_CMD_SMART);
    info->security_supported = !!(cmd_supp & ATA_CMD_SECURITY);
    info->flush_supported    = !!(cmd_supp & ATA_CMD_FLUSH_CACHE);

    /* Word 83 for LBA48 */
    info->lba48_supported = !!(ident[ATA_ID_CMD_SET_SUPP2] & ATA_CMD_LBA48);

    /* Features enabled (word 85) */
    feature_en = ident[ATA_ID_CMD_SET_EN];

    /* Transfer modes */
    info->udma_current  = ata_id_udma_mode(ident);
    info->mwdma_current = ata_id_mwdma_mode(ident);

    /* Determine current PIO mode based on word 64 */
    {
        uint16_t pio_modes = ident[ATA_ID_PIO_MODES];

        if (pio_modes & ATA_PIO_MODE4)
            info->pio_mode_current = 4;
        else if (pio_modes & ATA_PIO_MODE3)
            info->pio_mode_current = 3;
        else if (info->iordy_supported)
            info->pio_mode_current = 2;
        else
            info->pio_mode_current = 0;
    }

    /* SATA capabilities */
    info->sata_gen      = ata_id_sata_gen(ident);
    info->ncq_supported = ata_id_ncq_supported(ident);
    info->queue_depth   = ata_id_queue_depth(ident);

    /* TRIM support */
    info->trim_supported = ata_id_trim_supported(ident);

    /* Physical characteristics */
    info->rotation_rate = ata_id_rotation_rate(ident);
    info->form_factor   = ident[ATA_ID_FORM_FACTOR];

    /* World Wide Name */
    info->wwn = ata_id_wwn(ident);

    /* Acoustic management */
    if (ident[ATA_ID_ACOUSTIC] != 0) {
        info->acoustic_current     = (int)(ident[ATA_ID_ACOUSTIC] & 0x00FF);
        info->acoustic_recommended = (int)((ident[ATA_ID_ACOUSTIC] >> 8) & 0x00FF);
    } else {
        info->acoustic_current     = -1;
        info->acoustic_recommended = -1;
    }

    /* Logical sector size (word 106 bits 12-14 indicate validity and size) */
    {
        uint16_t ls = ident[ATA_ID_LOGICAL_SECT];

        if (ls != 0)
            info->logical_sector_size = (int)ls;
        else
            info->logical_sector_size = 512;
    }

    /* Security */
    {
        uint16_t sec = ident[ATA_ID_SECURITY];

        info->security_enabled = !!(sec & ATA_SEC_ENABLED);
        info->security_locked  = !!(sec & ATA_SEC_LOCKED);
    }

    /* Major version */
    info->major_version_bits = (unsigned int)ident[ATA_ID_MAJOR_VER];

    /* Transport (word 222) */
    {
        uint16_t trans = ident[ATA_ID_TRANSPORT];

        if (trans & ATA_TRANSPORT_SERIAL)
            strcpy(info->transport, "Serial ATA");
        else if (trans & ATA_TRANSPORT_PARALLEL)
            strcpy(info->transport, "Parallel ATA");
        else
            strcpy(info->transport, "ATA");
    }

    return 0;
}

/* ====================================================================
 *  ata_id_print_kernel — Print parsed info summary to kernel log
 * ==================================================================== */

void ata_id_print_kernel(const struct ata_device_info *info,
                         const char *prefix)
{
    const char *pfx;
    char cap_buf[32];
    uint64_t capacity_mb;
    uint64_t capacity_gb;

    if (!info)
        return;

    pfx = prefix ? prefix : "";

    kprintf("[%s] %s: %s\n", pfx, info->model[0] ? "Model" : "Unknown device",
            info->model[0] ? info->model : "N/A");

    if (info->serial[0])
        kprintf("[%s]   Serial: %s\n", pfx, info->serial);

    if (info->firmware[0])
        kprintf("[%s]   Firmware: %s\n", pfx, info->firmware);

    capacity_mb = info->lba_total / 2048;
    capacity_gb = capacity_mb / 1024;

    if (capacity_gb > 0)
        snprintf(cap_buf, sizeof(cap_buf), "%llu GB (%llu MB)",
                 (unsigned long long)capacity_gb,
                 (unsigned long long)capacity_mb);
    else
        snprintf(cap_buf, sizeof(cap_buf), "%llu MB",
                 (unsigned long long)capacity_mb);

    kprintf("[%s]   Capacity: %llu sectors (%s)\n", pfx,
            (unsigned long long)info->lba_total, cap_buf);

    kprintf("[%s]   Type: %s%s\n", pfx,
            info->is_atapi ? "ATAPI" : "ATA",
            info->is_removable ? " (removable)" : "");

    kprintf("[%s]   Transport: %s Gen%u\n", pfx,
            info->transport,
            info->sata_gen ? (unsigned int)info->sata_gen : 0u);

    kprintf("[%s]   Major version: %s\n", pfx,
            ata_major_version_str(info->major_version_bits));

    /* Rotation rate / SSD */
    if (info->rotation_rate == ATA_RPM_SSD)
        kprintf("[%s]   Media: SSD\n", pfx);
    else if (info->rotation_rate > 0)
        kprintf("[%s]   Rotation: %d RPM\n", pfx, info->rotation_rate);

    if (info->lba48_supported)
        kprintf("[%s]   Features: LBA48%s%s%s\n", pfx,
                info->ncq_supported ? " NCQ" : "",
                info->trim_supported ? " TRIM" : "",
                info->flush_supported ? " FLUSH" : "");
    else
        kprintf("[%s]   Features:%s%s%s\n", pfx,
                info->ncq_supported ? " NCQ" : "",
                info->trim_supported ? " TRIM" : "",
                info->flush_supported ? " FLUSH" : "");

    /* Transfer mode */
    if (info->udma_current >= 0)
        kprintf("[%s]   Current mode: %s\n", pfx,
                udma_mode_name(info->udma_current));
    else if (info->mwdma_current >= 0)
        kprintf("[%s]   Current mode: %s\n", pfx,
                mwdma_mode_name(info->mwdma_current));
    else
        kprintf("[%s]   Current mode: PIO %d\n", pfx,
                info->pio_mode_current);

    if (info->queue_depth > 1)
        kprintf("[%s]   NCQ queue depth: %d\n", pfx, info->queue_depth);

    if (info->wwn)
        kprintf("[%s]   WWN: 0x%016llX\n", pfx,
                (unsigned long long)info->wwn);

    if (info->logical_sector_size != 512)
        kprintf("[%s]   Logical sector size: %d bytes\n", pfx,
                info->logical_sector_size);

    if (info->security_enabled)
        kprintf("[%s]   Security: %s\n", pfx,
                info->security_locked ? "LOCKED" : "ENABLED");

    if (info->acoustic_current >= 0)
        kprintf("[%s]   Acoustic: current=%d recommended=%d\n", pfx,
                info->acoustic_current, info->acoustic_recommended);
}

/* ====================================================================
 *  ata_id_print_detailed — Print all parsed fields
 * ==================================================================== */

void ata_id_print_detailed(const uint16_t *ident, const char *prefix)
{
    struct ata_device_info info;
    const char *pfx;
    char model[ATA_ID_MODEL_LEN];
    char serial[ATA_ID_SERIAL_LEN];
    char fw[ATA_ID_FW_REV_LEN];
    int i;

    if (!ident)
        return;

    pfx = prefix ? prefix : "";

    if (ata_id_parse(ident, &info) < 0)
        return;

    /* Print summary first */
    ata_id_print_kernel(&info, prefix);

    /* Print raw word data for diagnostic purposes */
    kprintf("[%s]   Raw IDENTIFY data (non-zero words):\n", pfx);
    for (i = 0; i < 256; i++) {
        if (ident[i] != 0 || i < 32) {
            kprintf("[%s]     [%3d] 0x%04x\n", pfx, i, ident[i]);
        }
    }

    /* Re-extract strings for display */
    ata_id_string(ident, ATA_ID_MODEL, 20, model);
    ata_id_string(ident, ATA_ID_SERIAL, 10, serial);
    ata_id_string(ident, ATA_ID_FW_REV, 4, fw);

    kprintf("[%s]   Model:      \"%s\"\n", pfx, model);
    kprintf("[%s]   Serial:     \"%s\"\n", pfx, serial);
    kprintf("[%s]   Firmware:   \"%s\"\n", pfx, fw);

    if (info.wwn)
        kprintf("[%s]   WWN:        0x%016llX\n", pfx,
                (unsigned long long)info.wwn);

    kprintf("[%s]   Sectors:    28-bit=%llu  48-bit=%llu  total=%llu\n",
            pfx,
            (unsigned long long)info.lba28_sectors,
            (unsigned long long)info.lba48_sectors,
            (unsigned long long)info.lba_total);

    kprintf("[%s]   Caps:       LBA=%d DMA=%d IORDY=%d\n", pfx,
            info.lba_supported, info.dma_supported, info.iordy_supported);

    kprintf("[%s]   Trans:      %s Gen%u  SATA caps=0x%04x\n", pfx,
            info.transport,
            info.sata_gen ? (unsigned int)info.sata_gen : 0u,
            ident[ATA_ID_SATA_CAPS]);
}

/* ====================================================================
 *  Module hooks (when built as a loadable module)
 * ==================================================================== */

#ifdef MODULE
#include "module.h"

int init_module(void)
{
    kprintf("[ATA IDENTIFY] Module loaded\n");
    return 0;
}

void cleanup_module(void)
{
    kprintf("[ATA IDENTIFY] Module unloaded\n");
}

MODULE_LICENSE("GPL");
MODULE_VERSION("2.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("ATA IDENTIFY DEVICE data parser — structured extraction of "
                   "model, serial, firmware, capacity, transfer modes, "
                   "and feature support from raw IDENTIFY data");
MODULE_ALIAS("ata_identify");
#endif /* MODULE */
