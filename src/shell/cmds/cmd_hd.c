/* cmd_hd.c — hex dump */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

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
    /* Stub: print a dummy dump */
    (void)fname;
    kprintf("hd: hex dump of '%s' (stub)\n", fname);

    uint8_t dummy[16];
    for (int i = 0; i < 16; i++)
        dummy[i] = (uint8_t)(i * 0x11);

    dump_line(dummy, 0, 16);
    return 0;
}

void hd_init(void)
{
    kprintf("[OK] cmd_hd: hex dump command ready\n");
}
