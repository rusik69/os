/* cmd_sync.c — sync command: flush filesystem buffers */
#include "shell_cmds.h"
#include "fat32.h"
#include "printf.h"

void cmd_sync(void) {
    if (fat32_is_mounted()) {
        if (fat32_sync() == 0)
            kprintf("sync: FAT32 buffers flushed\n");
        else
            kprintf("sync: error flushing FAT32\n");
    } else {
        kprintf("sync: no FAT32 filesystem mounted\n");
    }
}
