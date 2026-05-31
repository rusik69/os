/* cmd_fmt.c — simple text formatter (folds long lines) */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

#define FMT_WIDTH 72

void cmd_fmt(const char *args) {
    if (!args || !*args) { kprintf("Usage: fmt <file>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path+1, args, 62); path[63] = '\0'; }
    else strncpy(path, args, 63);
    path[63] = '\0';

    static char fbuf[4096];
    uint32_t size;
    if (fs_read_file(path, fbuf, sizeof(fbuf) - 1, &size) < 0) {
        kprintf("fmt: cannot read '%s'\n", args);
        shell_set_exit_status(1);
        return;
    }
    fbuf[size] = '\0';

    /* Fold long lines */
    char out[4096];
    int opos = 0;
    int col = 0;
    int last_space = -1;
    int last_space_pos = -1;

    for (uint32_t i = 0; i < size && opos < 4095; i++) {
        char c = fbuf[i];
        if (c == '\n') {
            /* Hard newline — flush line */
            if (col == 0) {
                out[opos++] = '\n';
            } else {
                out[opos++] = '\n';
            }
            col = 0;
            last_space = -1;
            last_space_pos = -1;
        } else if (c == ' ') {
            col++;
            last_space = i;
            last_space_pos = opos;
            out[opos++] = c;
        } else {
            col++;
            out[opos++] = c;
            if (col > FMT_WIDTH && last_space >= 0) {
                /* Replace last space with newline */
                out[last_space_pos] = '\n';
                col = opos - last_space_pos - 1;
                last_space = -1;
                last_space_pos = -1;
            }
        }
    }
    out[opos] = '\0';
    kprintf("%s\n", out);
}
