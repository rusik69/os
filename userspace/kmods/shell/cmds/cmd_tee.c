/* cmd_tee.c -- Copy stdin to stdout and to files */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_tee(int argc, char **argv) {
    int append = 0;
    int optind = 1;

    while (optind < argc && argv[optind][0] == '-') {
        if (strcmp(argv[optind], "-a") == 0) {
            append = 1;
            optind++;
        } else if (strcmp(argv[optind], "--") == 0) {
            optind++;
            break;
        } else {
            kprintf("tee: unknown option '%s'\n", argv[optind]);
            return 1;
        }
    }

    /* Read stdin */
    if (!shell_has_stdin()) {
        kprintf("Usage: tee [-a] <file>...\n");
        return 1;
    }

    static char fbuf[16384];
    uint32_t fsize = (uint32_t)shell_stdin_read(fbuf, (int)sizeof(fbuf) - 1);
    fbuf[fsize] = '\0';

    /* Output to stdout */
    kprintf("%s", fbuf);

    /* Write to each file */
    for (int i = optind; i < argc; i++) {
        const char *fn = argv[i];
        char path[64];
        if (fn[0] != '/') { path[0] = '/'; strncpy(path + 1, fn, 61); path[62] = '\0'; }
        else { strncpy(path, fn, 63); path[63] = '\0'; }
        int pl = (int)strlen(path);
        while (pl > 0 && path[pl - 1] == ' ') path[--pl] = '\0';

        if (append) {
            /* For append, we need to read existing content and combine.
               VFS write overwrites, so we read first then write combined.
               But that's complex. For now, just write (this may overwrite). */
            /* Try to read existing content */
            static char existing[16384];
            uint32_t exist_size = 0;
            int rc = vfs_read(path, existing, (uint32_t)(sizeof(existing) - 1), &exist_size);
            if (rc == 0 && exist_size > 0) {
                /* Truncate then write both */
                /* Use truncate-like behavior: write existing + new */
                if (exist_size + fsize < sizeof(existing)) {
                    memcpy(existing + exist_size, fbuf, fsize);
                    if (vfs_write(path, existing, exist_size + fsize) != 0) {
                        kprintf("tee: cannot append to '%s'\n", fn);
                    }
                } else {
                    kprintf("tee: buffer too small for append\n");
                }
            } else {
                /* File doesn't exist yet, just write */
                if (vfs_write(path, fbuf, fsize) != 0) {
                    kprintf("tee: cannot write to '%s'\n", fn);
                }
            }
        } else {
            if (vfs_write(path, fbuf, fsize) != 0) {
                kprintf("tee: cannot write to '%s'\n", fn);
            }
        }
    }
    return 0;
}
