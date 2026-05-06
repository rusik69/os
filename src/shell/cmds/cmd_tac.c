/* cmd_tac.c — tac: print file lines in reverse order */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_tac(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: tac <file>\n");
        return;
    }

    char path[64];
    if (args[0] != '/') {
        path[0] = '/';
        strncpy(path + 1, args, 62);
        path[63] = '\0';
    } else {
        strncpy(path, args, 63);
        path[63] = '\0';
    }

    static char buf[4096];
    uint32_t size = 0;
    if (vfs_read(path, buf, sizeof(buf) - 1, &size) != 0) {
        kprintf("tac: %s: not found\n", path);
        return;
    }
    buf[size] = '\0';

    /* Collect line start offsets */
    int starts[256];
    int nlines = 0;
    starts[nlines++] = 0;
    for (uint32_t i = 0; i < size && nlines < 255; i++) {
        if (buf[i] == '\n' && i + 1 < size)
            starts[nlines++] = (int)(i + 1);
    }

    /* Print lines in reverse */
    for (int i = nlines - 1; i >= 0; i--) {
        int start = starts[i];
        int end   = (i + 1 < nlines) ? starts[i + 1] : (int)size;
        for (int j = start; j < end; j++)
            kprintf("%c", (uint64_t)(unsigned char)buf[j]);
        /* Ensure newline if last line lacks one */
        if (end > start && buf[end - 1] != '\n')
            kprintf("\n");
    }
}
