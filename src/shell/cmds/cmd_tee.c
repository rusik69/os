/* cmd_tee.c — Read from pipe input file, write to file + stdout */

#include "shell_cmds.h"
#include "vfs.h"
#include "printf.h"
#include "string.h"

void cmd_tee(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: cmd | tee <file>\n");
        return;
    }

    /* When used in a pipe, args = "<target_file> /.pipe_tmp"
     * The pipe_tmp contains the piped input. */
    char target[64];
    char input_file[64];
    const char *p = args;
    int i = 0;

    /* Parse first arg (target file or pipe file) */
    while (*p && *p != ' ' && i < 63) target[i++] = *p++;
    target[i] = '\0';
    while (*p == ' ') p++;

    /* If there's a second arg, that's the pipe input file */
    if (*p) {
        strncpy(input_file, p, 63);
        input_file[63] = '\0';
        int il = strlen(input_file);
        while (il > 0 && input_file[il-1] == ' ') input_file[--il] = '\0';
    } else {
        /* No pipe — read from target as regular file, just cat it */
        strcpy(input_file, target);
        target[0] = '\0';
    }

    /* Resolve input path */
    char ipath[64];
    if (input_file[0] != '/') { ipath[0] = '/'; strncpy(ipath + 1, input_file, 62); }
    else strncpy(ipath, input_file, 63);
    ipath[63] = '\0';

    static char buf[4096];
    uint32_t size = 0;
    if (vfs_read(ipath, buf, 4095, &size) != 0) {
        kprintf("tee: cannot read input\n");
        return;
    }
    buf[size] = '\0';

    /* Write to stdout */
    for (uint32_t j = 0; j < size; j++)
        kprintf("%c", (uint64_t)(uint8_t)buf[j]);

    /* Write to target file if specified */
    if (target[0]) {
        char tpath[64];
        if (target[0] != '/') { tpath[0] = '/'; strncpy(tpath + 1, target, 62); }
        else strncpy(tpath, target, 63);
        tpath[63] = '\0';
        vfs_write(tpath, buf, size);
    }
}
