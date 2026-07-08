/*
 * f_mass_storage.c — USB gadget mass storage function
 *
 * Exposes a host-side file or block device as a USB mass storage device
 * using the Bulk-Only Transport (BOT) protocol.
 *
 * SCSI command handling:
 *   - READ10 / WRITE10
 *   - INQUIRY
 *   - MODE_SENSE
 *   - TEST_UNIT_READY
 *   - READ_CAPACITY
 *   - REQUEST_SENSE
 *
 * File-backed LUN: reads/writes to a memory-backed store, optionally
 * backed by a regular file on the kernel FS.
 *
 * For simplicity, the LUN data is held in a memory buffer.  A real
 * implementation would use the block device layer for file I/O.
 *
 * References:
 *   USB Mass Storage Class — Bulk-Only Transport, Rev 1.0
 *   SCSI Primary Commands (SPC-4)
 *   SCSI Block Commands (SBC-3)
 *
 * Item S48 — USB gadget mass storage
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "pmm.h"
#include "errno.h"
#include "heap.h"

/* ── SCSI command codes ────────────────────────────────────────── */

#define SCSI_TEST_UNIT_READY   0x00
#define SCSI_REQUEST_SENSE     0x03
#define SCSI_INQUIRY           0x12
#define SCSI_MODE_SENSE6       0x1A
#define SCSI_READ_CAPACITY10   0x25
#define SCSI_READ10            0x28
#define SCSI_WRITE10           0x2A
#define SCSI_MODE_SENSE10      0x5A

/* ── BOT constants ─────────────────────────────────────────────── */

#define BOT_CBW_SIGNATURE  0x43425355u
#define BOT_CSW_SIGNATURE  0x53425355u
#define BOT_CBW_SIZE       31
#define BOT_CSW_SIZE       13

/* BOT command status values */
#define BOT_STATUS_GOOD    0x00
#define BOT_STATUS_FAILED  0x01
#define BOT_STATUS_PHASE   0x02

/* Direction flags */
#define BOT_DIR_IN         0x80  /* device to host */
#define BOT_DIR_OUT        0x00  /* host to device */

/* ── CBW / CSW structures ──────────────────────────────────────── */

#pragma pack(push, 1)
struct bot_cbw {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_len;
    uint8_t  flags;      /* 0x80 = IN, 0x00 = OUT */
    uint8_t  lun;
    uint8_t  cb_len;
    uint8_t  cb[16];
};

struct bot_csw {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;
};
#pragma pack(pop)

/* ── SCSI sense data ───────────────────────────────────────────── */

struct scsi_sense_data {
    uint8_t  error_code;      /* 0x70 = current, 0x71 = deferred */
    uint8_t  segment;
    uint8_t  sense_key;       /* 0x00-0x0F */
    uint8_t  info[4];
    uint8_t  add_sense_len;   /* n-7 */
    uint8_t  cmd_spec_info[4];
    uint8_t  asc;             /* Additional Sense Code */
    uint8_t  ascq;            /* Additional Sense Code Qualifier */
    uint8_t  fru;
    uint8_t  sense_key_spec[3];
} __attribute__((packed));

/* Sense keys */
#define SENSE_KEY_NO_SENSE      0x00
#define SENSE_KEY_RECOVERED    0x01
#define SENSE_KEY_NOT_READY    0x02
#define SENSE_KEY_MEDIUM_ERROR 0x03
#define SENSE_KEY_HARDWARE     0x04
#define SENSE_KEY_ILLEGAL_REQ  0x05
#define SENSE_KEY_UNIT_ATTN    0x06
#define SENSE_KEY_DATA_PROTECT 0x07
#define SENSE_KEY_BLANK_CHECK  0x08
#define SENSE_KEY_VENDOR       0x09
#define SENSE_KEY_COPY_ABORTED 0x0A
#define SENSE_KEY_ABORTED_CMD  0x0B
#define SENSE_KEY_VOLUME_OVERFLOW 0x0D
#define SENSE_KEY_MISCOMPARE   0x0E

/* ASC/ASCQ */
#define ASC_NO_ADDITIONAL      0x0000
#define ASC_NOT_READY          0x0400
#define ASC_MEDIUM_NOT_PRESENT 0x3A00
#define ASC_LBA_OUT_OF_RANGE   0x2100
#define ASC_INVALID_FIELD      0x2400
#define ASC_WRITE_PROTECTED    0x2700

/* ── LUN (Logical Unit) structure ──────────────────────────────── */

#define MAX_LUNS 8
#define BLOCK_SIZE 512
#define LUN_DEFAULT_SIZE (64 * 1024 * 1024)  /* 64 MB default */

struct ms_lun {
    int     in_use;
    char    file_path[128];    /* optional backing file path */
    uint8_t *data;             /* in-memory buffer */
    uint32_t buf_size;         /* allocated buffer size in bytes */
    uint32_t num_blocks;       /* actual number of 512-byte blocks */
    int     readonly;
    int     removable;
};

/* ── Gadget function state ─────────────────────────────────────── */

struct f_ms_config {
    struct ms_lun   luns[MAX_LUNS];
    int             num_luns;
    spinlock_t      lock;
    /* Current command state */
    struct scsi_sense_data sense;
    uint8_t         *data_buf;
    uint32_t         data_buf_size;
};

/* ── Global state ──────────────────────────────────────────────── */

static struct f_ms_config g_ms_config;

/* ═══════════════════════════════════════════════════════════════════
 *  LUN operations
 * ═══════════════════════════════════════════════════════════════════ */

static int f_ms_add_lun(uint32_t size_mb, int readonly)
{
    if (g_ms_config.num_luns >= MAX_LUNS)
        return -1;

    struct ms_lun *lun = &g_ms_config.luns[g_ms_config.num_luns];
    memset(lun, 0, sizeof(*lun));

    lun->readonly = readonly;
    lun->removable = 1;

    /* Allocate memory buffer for the LUN */
    uint32_t size = (size_mb > 0) ? (size_mb * 1024 * 1024) : LUN_DEFAULT_SIZE;
    lun->data = (uint8_t *)kmalloc(size);
    if (!lun->data) {
        kprintf("[f_ms] failed to allocate %u bytes for LUN\n", size);
        return -1;
    }
    memset(lun->data, 0, size);
    lun->buf_size = size;
    lun->num_blocks = size / BLOCK_SIZE;
    lun->in_use = 1;

    g_ms_config.num_luns++;

    kprintf("[f_ms] LUN %d: %u MB (%u blocks, %s)\n",
            g_ms_config.num_luns - 1, size_mb,
            lun->num_blocks, readonly ? "ro" : "rw");

    return g_ms_config.num_luns - 1;
}

static int lun_read(struct ms_lun *lun, uint32_t lba,
                    uint8_t *buf, uint32_t blocks)
{
    if (!lun || !lun->data || !buf) return -1;

    uint32_t offset = lba * BLOCK_SIZE;
    uint32_t size = blocks * BLOCK_SIZE;

    if (offset + size > lun->buf_size)
        return -1;

    memcpy(buf, lun->data + offset, size);
    return 0;
}

static int lun_write(struct ms_lun *lun, uint32_t lba,
                     const uint8_t *buf, uint32_t blocks)
{
    if (!lun || !lun->data || lun->readonly || !buf) return -1;

    uint32_t offset = lba * BLOCK_SIZE;
    uint32_t size = blocks * BLOCK_SIZE;

    if (offset + size > lun->buf_size)
        return -1;

    memcpy(lun->data + offset, buf, size);

    /* Update block count if we wrote past end */
    uint32_t end_lba = lba + blocks;
    if (end_lba > lun->num_blocks)
        lun->num_blocks = end_lba;

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  SCSI command handlers
 * ═══════════════════════════════════════════════════════════════════ */

static void set_sense(struct f_ms_config *cfg, uint8_t key,
                      uint16_t asc_ascq)
{
    memset(&cfg->sense, 0, sizeof(cfg->sense));
    cfg->sense.error_code = 0x70;  /* current error */
    cfg->sense.sense_key = key;
    cfg->sense.add_sense_len = 10;
    cfg->sense.asc = (uint8_t)(asc_ascq >> 8);
    cfg->sense.ascq = (uint8_t)(asc_ascq & 0xFF);
}

/* TEST UNIT READY */
static int handle_test_unit_ready(struct f_ms_config *cfg,
                                   const uint8_t *cb, int lun_idx)
{
    (void)cb;
    if (lun_idx >= cfg->num_luns || !cfg->luns[lun_idx].in_use) {
        set_sense(cfg, SENSE_KEY_NOT_READY, ASC_NOT_READY);
        return -1;
    }
    set_sense(cfg, SENSE_KEY_NO_SENSE, ASC_NO_ADDITIONAL);
    return 0;
}

/* INQUIRY */
static int handle_inquiry(struct f_ms_config *cfg,
                           const uint8_t *cb, int lun_idx,
                           uint8_t *buf, uint32_t *len)
{
    (void)lun_idx;
    /* EVPD = cb[1] & 1 */
    if (cb[1] & 1) {
        /* Vital Product Data — not supported */
        set_sense(cfg, SENSE_KEY_ILLEGAL_REQ, ASC_INVALID_FIELD);
        return -1;
    }

    uint8_t inquiry_data[36];
    memset(inquiry_data, 0, sizeof(inquiry_data));

    inquiry_data[0] = 0x00;  /* Peripheral Device Type: direct-access */
    inquiry_data[1] = 0x80;  /* RMB = 1 (removable) */
    inquiry_data[2] = 0x06;  /* Version: SPC-4 */
    inquiry_data[3] = 0x02;  /* Response Data Format */
    inquiry_data[4] = 31;    /* Additional length (n-4) */

    /* Vendor ID (8 bytes) */
    memcpy(&inquiry_data[8], "HermesOS", 8);
    /* Product ID (16 bytes) */
    memcpy(&inquiry_data[16], "USB Mass Storage", 16);
    /* Product revision (4 bytes) */
    memcpy(&inquiry_data[32], "1.00", 4);

    uint32_t alloc_len = ((uint32_t)cb[3] << 8) | (uint32_t)cb[4];
    *len = (alloc_len < sizeof(inquiry_data)) ? alloc_len : sizeof(inquiry_data);
    if (*len > 36) *len = 36;

    memcpy(buf, inquiry_data, *len);
    set_sense(cfg, SENSE_KEY_NO_SENSE, ASC_NO_ADDITIONAL);
    return 0;
}

/* READ CAPACITY (10) */
static int handle_read_capacity10(struct f_ms_config *cfg,
                                   const uint8_t *cb, int lun_idx,
                                   uint8_t *buf, uint32_t *len)
{
    (void)cb;
    struct ms_lun *lun = &cfg->luns[lun_idx];
    if (lun_idx >= cfg->num_luns || !lun->in_use) {
        set_sense(cfg, SENSE_KEY_NOT_READY, ASC_NOT_READY);
        return -1;
    }

    /* Return last LBA (big-endian) and block size (big-endian) */
    uint32_t last_lba = (lun->num_blocks > 0) ? (lun->num_blocks - 1) : 0;
    buf[0] = (uint8_t)(last_lba >> 24);
    buf[1] = (uint8_t)(last_lba >> 16);
    buf[2] = (uint8_t)(last_lba >> 8);
    buf[3] = (uint8_t)(last_lba & 0xFF);
    buf[4] = (uint8_t)(BLOCK_SIZE >> 24);
    buf[5] = (uint8_t)(BLOCK_SIZE >> 16);
    buf[6] = (uint8_t)(BLOCK_SIZE >> 8);
    buf[7] = (uint8_t)(BLOCK_SIZE & 0xFF);

    *len = 8;
    set_sense(cfg, SENSE_KEY_NO_SENSE, ASC_NO_ADDITIONAL);
    return 0;
}

/* MODE SENSE (6) */
static int handle_mode_sense6(struct f_ms_config *cfg,
                               const uint8_t *cb, int lun_idx,
                               uint8_t *buf, uint32_t *len)
{
    (void)lun_idx;
    uint8_t page = cb[2] & 0x3F;
    (void)page;

    /* Return minimal mode parameter header */
    buf[0] = 3;           /* mode data length (n-1) */
    buf[1] = 0;           /* medium type */
    buf[2] = 0;           /* device-specific parameter */
    buf[3] = 0;           /* block descriptor length */

    *len = 4;
    set_sense(cfg, SENSE_KEY_NO_SENSE, ASC_NO_ADDITIONAL);
    return 0;
}

/* REQUEST SENSE */
static int handle_request_sense(struct f_ms_config *cfg,
                                 const uint8_t *cb, int lun_idx,
                                 uint8_t *buf, uint32_t *len)
{
    (void)lun_idx;
    uint32_t alloc_len = (uint32_t)cb[4];

    uint32_t sense_len = sizeof(struct scsi_sense_data);
    if (sense_len > alloc_len) sense_len = alloc_len;
    if (sense_len > 18) sense_len = 18;

    memcpy(buf, &cfg->sense, sense_len);
    *len = sense_len;

    /* Clear sense after reporting */
    set_sense(cfg, SENSE_KEY_NO_SENSE, ASC_NO_ADDITIONAL);
    return 0;
}

/* READ10 */
static int handle_read10(struct f_ms_config *cfg,
                          const uint8_t *cb, int lun_idx,
                          uint8_t *buf, uint32_t *len)
{
    struct ms_lun *lun = &cfg->luns[lun_idx];
    if (lun_idx >= cfg->num_luns || !lun->in_use) {
        set_sense(cfg, SENSE_KEY_NOT_READY, ASC_NOT_READY);
        return -1;
    }

    uint32_t lba = ((uint32_t)cb[2] << 24) | ((uint32_t)cb[3] << 16) |
                   ((uint32_t)cb[4] << 8)  | (uint32_t)cb[5];
    uint32_t blocks = ((uint32_t)cb[7] << 8) | (uint32_t)cb[8];
    if (blocks == 0) blocks = 256;

    /* Check bounds */
    if (lba + blocks > lun->num_blocks) {
        /* Try to read what we can */
        if (lba >= lun->num_blocks) {
            set_sense(cfg, SENSE_KEY_ILLEGAL_REQ, ASC_LBA_OUT_OF_RANGE);
            return -1;
        }
        blocks = lun->num_blocks - lba;
    }

    int ret = lun_read(lun, lba, buf, blocks);
    if (ret < 0) {
        set_sense(cfg, SENSE_KEY_MEDIUM_ERROR, ASC_NOT_READY);
        return -1;
    }

    *len = blocks * BLOCK_SIZE;
    set_sense(cfg, SENSE_KEY_NO_SENSE, ASC_NO_ADDITIONAL);
    return 0;
}

/* WRITE10 */
static int handle_write10(struct f_ms_config *cfg,
                           const uint8_t *cb, int lun_idx,
                           const uint8_t *data, uint32_t data_len)
{
    struct ms_lun *lun = &cfg->luns[lun_idx];
    if (lun_idx >= cfg->num_luns || !lun->in_use) {
        set_sense(cfg, SENSE_KEY_NOT_READY, ASC_NOT_READY);
        return -1;
    }

    if (lun->readonly) {
        set_sense(cfg, SENSE_KEY_DATA_PROTECT, ASC_WRITE_PROTECTED);
        return -1;
    }

    uint32_t lba = ((uint32_t)cb[2] << 24) | ((uint32_t)cb[3] << 16) |
                   ((uint32_t)cb[4] << 8)  | (uint32_t)cb[5];
    uint32_t blocks = ((uint32_t)cb[7] << 8) | (uint32_t)cb[8];
    if (blocks == 0) blocks = 256;

    uint32_t expected_len = blocks * BLOCK_SIZE;
    if (data_len < expected_len) {
        /* Partial write — not enough data */
        return -1;
    }

    int ret = lun_write(lun, lba, data, blocks);
    if (ret < 0) {
        set_sense(cfg, SENSE_KEY_MEDIUM_ERROR, ASC_NOT_READY);
        return -1;
    }

    set_sense(cfg, SENSE_KEY_NO_SENSE, ASC_NO_ADDITIONAL);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  BOT command dispatch
 * ═══════════════════════════════════════════════════════════════════ */

static int f_ms_submit_command(const struct bot_cbw *cbw,
                         uint8_t *data_buf, uint32_t *data_len)
{
    if (!cbw || !data_buf || !data_len)
        return BOT_STATUS_FAILED;

    struct f_ms_config *cfg = &g_ms_config;
    int lun = cbw->lun;
    int status = BOT_STATUS_GOOD;

    if (lun >= cfg->num_luns) {
        set_sense(cfg, SENSE_KEY_ILLEGAL_REQ, ASC_INVALID_FIELD);
        return BOT_STATUS_FAILED;
    }

    uint8_t opcode = cbw->cb[0];

    switch (opcode) {
        case SCSI_TEST_UNIT_READY:
            if (handle_test_unit_ready(cfg, cbw->cb, lun) < 0)
                status = BOT_STATUS_FAILED;
            break;

        case SCSI_REQUEST_SENSE:
            if (handle_request_sense(cfg, cbw->cb, lun, data_buf, data_len) < 0)
                status = BOT_STATUS_FAILED;
            break;

        case SCSI_INQUIRY:
            if (handle_inquiry(cfg, cbw->cb, lun, data_buf, data_len) < 0)
                status = BOT_STATUS_FAILED;
            break;

        case SCSI_MODE_SENSE6:
        case SCSI_MODE_SENSE10:
            if (handle_mode_sense6(cfg, cbw->cb, lun, data_buf, data_len) < 0)
                status = BOT_STATUS_FAILED;
            break;

        case SCSI_READ_CAPACITY10:
            if (handle_read_capacity10(cfg, cbw->cb, lun, data_buf, data_len) < 0)
                status = BOT_STATUS_FAILED;
            break;

        case SCSI_READ10:
            if (handle_read10(cfg, cbw->cb, lun, data_buf, data_len) < 0)
                status = BOT_STATUS_FAILED;
            break;

        case SCSI_WRITE10:
            /* For WRITE10, data flows from host to device */
            if (cbw->flags & BOT_DIR_IN) {
                set_sense(cfg, SENSE_KEY_ILLEGAL_REQ, ASC_INVALID_FIELD);
                status = BOT_STATUS_FAILED;
            } else {
                if (handle_write10(cfg, cbw->cb, lun, data_buf, *data_len) < 0)
                    status = BOT_STATUS_FAILED;
            }
            break;

        default:
            set_sense(cfg, SENSE_KEY_ILLEGAL_REQ, ASC_INVALID_FIELD);
            status = BOT_STATUS_FAILED;
            break;
    }

    return status;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Module init/exit
 * ═══════════════════════════════════════════════════════════════════ */

static void f_mass_storage_init(void)
{
    memset(&g_ms_config, 0, sizeof(g_ms_config));
    spinlock_init(&g_ms_config.lock);

    /* Default sense: no error */
    set_sense(&g_ms_config, SENSE_KEY_NO_SENSE, ASC_NO_ADDITIONAL);

    kprintf("[f_ms] USB gadget mass storage function initialised\n");
}

static void f_mass_storage_exit(void)
{
    /* Free all LUN memory buffers */
    for (int i = 0; i < g_ms_config.num_luns; i++) {
        if (g_ms_config.luns[i].data) {
            kfree(g_ms_config.luns[i].data);
            g_ms_config.luns[i].data = NULL;
        }
        g_ms_config.luns[i].in_use = 0;
    }
    g_ms_config.num_luns = 0;
    kprintf("[f_ms] USB gadget mass storage function exited\n");
}

/* ── Implement: f_mass_storage_read ─────────────────────────────── */
static int f_mass_storage_read(void *file, void *buf, size_t count)
{
    (void)file;
    if (!buf || count == 0) return -EINVAL;

    /* Read from the first available LUN */
    struct f_ms_config *cfg = &g_ms_config;
    if (cfg->num_luns == 0) return -ENODEV;

    /* Read from LUN 0 starting at the current offset */
    struct ms_lun *lun = &cfg->luns[0];
    if (!lun->data || !lun->in_use) return -ENODEV;

    size_t to_read = count;
    if (to_read > lun->buf_size) to_read = lun->buf_size;
    memcpy(buf, lun->data, to_read);
    return (int)to_read;
}
/* ── Implement: f_mass_storage_write ─────────────────────────────── */
static int f_mass_storage_write(void *file, const void *buf, size_t count)
{
    (void)file;
    if (!buf || count == 0) return -EINVAL;

    struct f_ms_config *cfg = &g_ms_config;
    if (cfg->num_luns == 0) return -ENODEV;

    struct ms_lun *lun = &cfg->luns[0];
    if (!lun->data || !lun->in_use) return -ENODEV;
    if (lun->readonly) return -EACCES;

    size_t to_write = count;
    if (to_write > lun->buf_size) to_write = lun->buf_size;
    memcpy(lun->data, buf, to_write);
    return (int)to_write;
}
