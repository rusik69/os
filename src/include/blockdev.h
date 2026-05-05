#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include "types.h"

#define BLOCKDEV_MAX_DEVICES 32

#define BLOCKDEV_ATA    0
#define BLOCKDEV_AHCI   1
#define BLOCKDEV_USB0  16

typedef int      (*blockdev_read_fn)(uint32_t lba, uint8_t count, void *buf);
typedef int      (*blockdev_write_fn)(uint32_t lba, uint8_t count, const void *buf);
typedef uint32_t (*blockdev_size_fn)(void);

void blockdev_init(void);
int  blockdev_register(int id, const char *name,
                       blockdev_read_fn read_fn,
                       blockdev_write_fn write_fn,
                       blockdev_size_fn size_fn);
int  blockdev_is_registered(int id);
int  blockdev_read_sectors(int id, uint32_t lba, uint8_t count, void *buf);
int  blockdev_write_sectors(int id, uint32_t lba, uint8_t count, const void *buf);
uint32_t blockdev_get_sectors(int id);
const char *blockdev_name(int id);

#endif
