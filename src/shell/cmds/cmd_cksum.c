/* cmd_cksum.c — CRC checksum (simplified additive checksum) */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"

/* Simple CRC32 using the standard polynomial */
static uint32_t crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

void cmd_cksum(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: cksum <file>\n");
        return;
    }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); }
    else strncpy(path, args, 63);
    path[63] = '\0';
    int pl = strlen(path);
    while (pl > 0 && path[pl-1] == ' ') path[--pl] = '\0';

    static uint8_t buf[65536];
    uint32_t size = 0;
    if (vfs_read(path, buf, sizeof(buf), &size) != 0) {
        kprintf("cksum: cannot read '%s'\n", path);
        return;
    }

    uint32_t cksum = crc32(buf, size);
    kprintf("%u %u %s\n", (unsigned int)cksum, (unsigned int)size, path);
}
