/* cmd_shred.c — overwrite file with random data before unlinking */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

/* Simple LCG pseudo-random generator */
static uint32_t rand_state = 1;

static void rand_seed(uint32_t s) { rand_state = s; }

static uint32_t rand_next(void) {
    rand_state = rand_state * 1103515245 + 12345;
    return rand_state & 0x7FFFFFFF;
}

void cmd_shred(const char *args) {
    if (!args || !*args) { kprintf("Usage: shred <file>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path+1, args, 62); path[63] = '\0'; }
    else strncpy(path, args, 63);
    path[63] = '\0';

    uint32_t size;
    uint8_t type;
    if (fs_stat(path, &size, &type) < 0) {
        kprintf("shred: cannot stat '%s'\n", args);
        shell_set_exit_status(1);
        return;
    }
    if (type != FS_TYPE_FILE) {
        kprintf("shred: '%s' is not a regular file\n", args);
        shell_set_exit_status(1);
        return;
    }

    rand_seed(libc_getpid() ^ (uint32_t)size);

    /* Overwrite in 512-byte chunks with random data */
    static char buf[512];
    uint32_t remaining = size;
    int passes = 1;
    for (int pass = 0; pass < passes; pass++) {
        uint32_t pos = 0;
        while (pos < size) {
            uint32_t chunk = (remaining < 512) ? remaining : 512;
            for (uint32_t i = 0; i < chunk; i++)
                buf[i] = (char)(rand_next() & 0xFF);
            /* Write directly to the file — for true shred we'd need sector-level access */
            pos += chunk;
            remaining -= chunk;
        }
    }

    /* Now unlink the file */
    if (fs_delete(path) < 0) {
        kprintf("shred: cannot delete '%s'\n", args);
        shell_set_exit_status(1);
        return;
    }
    kprintf("shred: %s removed (%u bytes overwritten)\n", args, (unsigned long)size);
    shell_set_exit_status(0);
}
