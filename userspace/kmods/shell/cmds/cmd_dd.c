/* cmd_dd.c — Data Duplicator (simple file copy with count/skip) */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_dd(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: dd if=<src> of=<dst> [count=N] [skip=N]\n");
        return;
    }

    char ifile[64] = {0};
    char ofile[64] = {0};
    int count = 0;  /* 0 = read all */
    int skip = 0;

    /* Parse key=value arguments */
    char buf[256];
    strncpy(buf, args, 255);
    buf[255] = '\0';

    char *tok = strtok(buf, " ");
    while (tok) {
        if (strncmp(tok, "if=", 3) == 0) {
            strncpy(ifile, tok + 3, 63);
        } else if (strncmp(tok, "of=", 3) == 0) {
            strncpy(ofile, tok + 3, 63);
        } else if (strncmp(tok, "count=", 6) == 0) {
            count = (int)strtoul(tok + 6, NULL, 10);
        } else if (strncmp(tok, "skip=", 5) == 0) {
            skip = (int)strtoul(tok + 5, NULL, 10);
        }
        tok = strtok(NULL, " ");
    }

    if (ifile[0] == '\0' || ofile[0] == '\0') {
        kprintf("dd: missing if= or of=\n");
        return;
    }

    /* Normalize paths */
    char src[64], dst[64];
    if (ifile[0] != '/') { src[0] = '/'; strncpy(src + 1, ifile, 62); }
    else strncpy(src, ifile, 63);
    src[63] = '\0';

    if (ofile[0] != '/') { dst[0] = '/'; strncpy(dst + 1, ofile, 62); }
    else strncpy(dst, ofile, 63);
    dst[63] = '\0';

    /* Read source file */
    static uint8_t data[65536];
    uint32_t size = 0;
    if (vfs_read(src, data, sizeof(data), &size) != 0) {
        kprintf("dd: cannot read '%s'\n", src);
        return;
    }

    /* Apply skip */
    uint32_t offset = (uint32_t)(skip * 512);
    if (offset >= size) {
        kprintf("dd: skip past end of file\n");
        return;
    }

    /* Apply count (in 512-byte blocks) */
    uint32_t copy_size = size - offset;
    if (count > 0) {
        uint32_t count_bytes = (uint32_t)(count * 512);
        if (count_bytes < copy_size) copy_size = count_bytes;
    }

    /* Write to destination */
    if (vfs_write(dst, data + offset, copy_size) != 0) {
        kprintf("dd: cannot write '%s'\n", dst);
        return;
    }

    int blocks_in = (size + 511) / 512;
    int blocks_out = (copy_size + 511) / 512;
    kprintf("%d+0 records in\n", blocks_in);
    kprintf("%d+0 records out\n", blocks_out);
    kprintf("%d bytes copied\n", copy_size);
}
