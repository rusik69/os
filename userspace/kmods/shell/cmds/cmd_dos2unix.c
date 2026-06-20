/* cmd_dos2unix.c — convert DOS CRLF to Unix LF */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_dos2unix(int argc, char **argv) {
    const char *inpath = NULL;
    const char *outpath = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outpath = argv[++i];
        } else if (argv[i][0] != '-') {
            inpath = argv[i];
        } else {
            kprintf("dos2unix: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (!inpath) {
        kprintf("Usage: dos2unix <input> [-o <output>]\n");
        return 1;
    }

    uint32_t size;
    uint8_t type;
    if (libc_fs_stat(inpath, &size, &type) < 0) {
        kprintf("dos2unix: cannot open '%s'\n", inpath);
        return 1;
    }

    char *buf = (char *)libc_malloc(size + 1);
    if (!buf) return 1;
    uint32_t out_size;
    if (libc_fs_read_file(inpath, buf, size, &out_size) < 0) {
        libc_free(buf);
        return 1;
    }
    buf[out_size] = '\0';

    /* Convert CRLF -> LF */
    char *outbuf = (char *)libc_malloc(size + 1);
    if (!outbuf) { libc_free(buf); return 1; }
    int j = 0;
    for (uint32_t i = 0; i < out_size; i++) {
        if (buf[i] == '\r' && i + 1 < out_size && buf[i + 1] == '\n') {
            continue; /* skip CR */
        }
        outbuf[j++] = buf[i];
    }
    outbuf[j] = '\0';

    const char *dest = outpath ? outpath : inpath;
    if (libc_fs_write_file(dest, outbuf, j) < 0) {
        kprintf("dos2unix: error writing '%s'\n", dest);
        libc_free(buf);
        libc_free(outbuf);
        return 1;
    }

    if (j != (int)out_size)
        kprintf("dos2unix: converted '%s' (%d -> %d bytes)\n", inpath, out_size, j);
    else
        kprintf("dos2unix: '%s' is already Unix format\n", inpath);

    libc_free(buf);
    libc_free(outbuf);
    return 0;
}
