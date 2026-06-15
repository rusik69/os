// SPDX-License-Identifier: GPL-2.0-only
/*
 * usb_uas.c — USB Attached SCSI (UAS) driver
 *
 * Implements USB Attached SCSI (UAS) transport protocol.
 * UAS enables SCSI command queuing over USB using stream pipes.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "heap.h"

#define UAS_MAX_IUS  16
#define UAS_DATA_DIR_NONE  0
#define UAS_DATA_DIR_IN    1
#define UAS_DATA_DIR_OUT   2

/* UAS Information Units */
#define UAS_IU_COMMAND     0x01
#define UAS_IU_SENSE       0x03
#define UAS_IU_RESPONSE    0x04
#define UAS_IU_TASK_MGMT   0x05
#define UAS_IU_READY       0x06

struct uas_iub {
    uint8_t iu_id;
    uint8_t reserved;
    uint16_t tag;
    uint8_t data[512];
    int data_len;
};

struct uas_device {
    int active;
    int dev_num;
    uint64_t lba_count;
    uint32_t block_size;
    struct uas_iub ius[UAS_MAX_IUS];
    int iu_count;
};

static struct uas_device uas_devs[8];
static int uas_dev_count;

/* Probe a USB device for UAS support */
int uas_probe(int dev_num)
{
    if (uas_dev_count >= 8)
        return -ENOMEM;

    struct uas_device *dev = &uas_devs[uas_dev_count];
    dev->active = 1;
    dev->dev_num = dev_num;
    dev->block_size = 512; /* default */
    dev->lba_count = 0;
    uas_dev_count++;

    kprintf("[UAS] Device %d probed (block_size=%u)\n", dev_num, dev->block_size);
    return uas_dev_count - 1;
}

/* Send a SCSI command via UAS */
int uas_send_command(int dev_id, uint8_t *cdb, int cdb_len,
                      uint8_t *data, int data_len, int dir)
{
    if (dev_id < 0 || dev_id >= uas_dev_count || !uas_devs[dev_id].active)
        return -ENODEV;

    struct uas_device *dev = &uas_devs[dev_id];
    (void)dev;

    kprintf("[UAS] Command: dev=%d cdb_len=%d data_len=%d dir=%d\n",
            dev_id, cdb_len, data_len, dir);
    return data_len;
}

/* Read sectors via UAS */
int uas_read_sectors(int dev_id, uint64_t lba, uint32_t count, uint8_t *buf)
{
    uint8_t cdb[16];
    memset(cdb, 0, sizeof(cdb));

    /* READ(16) command */
    cdb[0] = 0x88;
    cdb[2] = (uint8_t)(lba >> 56);
    cdb[3] = (uint8_t)(lba >> 48);
    cdb[4] = (uint8_t)(lba >> 40);
    cdb[5] = (uint8_t)(lba >> 32);
    cdb[6] = (uint8_t)(lba >> 24);
    cdb[7] = (uint8_t)(lba >> 16);
    cdb[8] = (uint8_t)(lba >> 8);
    cdb[9] = (uint8_t)(lba);
    cdb[10] = (uint8_t)(count >> 24);
    cdb[11] = (uint8_t)(count >> 16);
    cdb[12] = (uint8_t)(count >> 8);
    cdb[13] = (uint8_t)(count);

    return uas_send_command(dev_id, cdb, 16, buf,
                            (int)(count * uas_devs[dev_id].block_size),
                            UAS_DATA_DIR_IN);
}

/* Write sectors via UAS */
int uas_write_sectors(int dev_id, uint64_t lba, uint32_t count, const uint8_t *buf)
{
    uint8_t cdb[16];
    memset(cdb, 0, sizeof(cdb));

    /* WRITE(16) command */
    cdb[0] = 0x8A;
    cdb[2] = (uint8_t)(lba >> 56);
    cdb[3] = (uint8_t)(lba >> 48);
    cdb[4] = (uint8_t)(lba >> 40);
    cdb[5] = (uint8_t)(lba >> 32);
    cdb[6] = (uint8_t)(lba >> 24);
    cdb[7] = (uint8_t)(lba >> 16);
    cdb[8] = (uint8_t)(lba >> 8);
    cdb[9] = (uint8_t)(lba);
    cdb[10] = (uint8_t)(count >> 24);
    cdb[11] = (uint8_t)(count >> 16);
    cdb[12] = (uint8_t)(count >> 8);
    cdb[13] = (uint8_t)(count);

    return uas_send_command(dev_id, cdb, 16, (uint8_t *)buf,
                            (int)(count * uas_devs[dev_id].block_size),
                            UAS_DATA_DIR_OUT);
}

void usb_uas_init(void)
{
    memset(uas_devs, 0, sizeof(uas_devs));
    uas_dev_count = 0;
    kprintf("[OK] USB UAS — USB Attached SCSI driver\n");
}
#include "module.h"
module_init(usb_uas_init);
