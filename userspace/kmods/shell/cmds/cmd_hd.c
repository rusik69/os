/* cmd_hd.c — hex dump */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

#define HD_BUF_SIZE 4096

static void dump_line(const uint8_t *data, uint64_t offset, size_t len)
{
    kprintf("%016llx  ", offset);
    for (size_t i = 0; i < 16; i++) {
        if (i < len)
            kprintf("%02x ", data[i]);
        else
            kprintf("   ");
        if (i == 7)
            kprintf(" ");
    }
    kprintf(" |");
    for (size_t i = 0; i < 16 && i < len; i++) {
        char c = (char)data[i];
        kprintf("%c", (c >= 32 && c < 127) ? c : '.');
    }
    kprintf("|\n");
}

int cmd_hd(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: hd <file>\n");
        return 1;
    }

    const char *fname = argv[1];

    /* Read the file using vfs_read */
    static uint8_t buf[HD_BUF_SIZE];
    uint32_t size = 0;

    if (vfs_read(fname, buf, HD_BUF_SIZE, &size) != 0) {
        kprintf("hd: cannot read '%s'\n", fname);
        return 1;
    }

    if (size == 0) {
        kprintf("hd: '%s' is empty\n", fname);
        return 0;
    }

    /* Hex dump the data in 16-byte lines */
    uint64_t offset = 0;
    uint32_t pos = 0;
    while (pos < size) {
        size_t chunk = (size - pos < 16) ? (size - pos) : 16;
        dump_line(buf + pos, offset, chunk);
        pos += 16;
        offset += 16;
    }

    kprintf("%016llx\n", (unsigned long long)size);
    return 0;
}

void hd_init(void)
{
    kprintf("[OK] cmd_hd: hex dump command ready\n");
}
