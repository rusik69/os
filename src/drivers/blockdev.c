#include "blockdev.h"
#include "string.h"

struct blockdev_entry {
    int active;
    char name[16];
    blockdev_read_fn read_fn;
    blockdev_write_fn write_fn;
    blockdev_size_fn size_fn;
};

static struct blockdev_entry g_blockdevs[BLOCKDEV_MAX_DEVICES];

void blockdev_init(void) {
    memset(g_blockdevs, 0, sizeof(g_blockdevs));
}

int blockdev_register(int id, const char *name,
                      blockdev_read_fn read_fn,
                      blockdev_write_fn write_fn,
                      blockdev_size_fn size_fn) {
    if (id < 0 || id >= BLOCKDEV_MAX_DEVICES) return -1;
    if (!read_fn) return -1;

    g_blockdevs[id].active = 1;
    g_blockdevs[id].read_fn = read_fn;
    g_blockdevs[id].write_fn = write_fn;
    g_blockdevs[id].size_fn = size_fn;

    if (name && *name) {
        strncpy(g_blockdevs[id].name, name, sizeof(g_blockdevs[id].name) - 1);
        g_blockdevs[id].name[sizeof(g_blockdevs[id].name) - 1] = '\0';
    } else {
        g_blockdevs[id].name[0] = '\0';
    }
    return 0;
}

int blockdev_is_registered(int id) {
    if (id < 0 || id >= BLOCKDEV_MAX_DEVICES) return 0;
    return g_blockdevs[id].active;
}

int blockdev_read_sectors(int id, uint32_t lba, uint8_t count, void *buf) {
    if (!blockdev_is_registered(id)) return -1;
    return g_blockdevs[id].read_fn(lba, count, buf);
}

int blockdev_write_sectors(int id, uint32_t lba, uint8_t count, const void *buf) {
    if (!blockdev_is_registered(id)) return -1;
    if (!g_blockdevs[id].write_fn) return -1;
    return g_blockdevs[id].write_fn(lba, count, buf);
}

uint32_t blockdev_get_sectors(int id) {
    if (!blockdev_is_registered(id)) return 0;
    if (!g_blockdevs[id].size_fn) return 0;
    return g_blockdevs[id].size_fn();
}

const char *blockdev_name(int id) {
    if (!blockdev_is_registered(id)) return "";
    return g_blockdevs[id].name;
}
