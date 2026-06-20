/* cmd_sed.c — sed command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"

static void sed_replace(char *line, const char *old_s, const char *new_s) {
    char buf[1024];
    int bi = 0;
    int li = 0;
    int old_len = (int)strlen(old_s);

    while (line[li]) {
        if ((int)strlen(line + li) >= old_len && strncmp(line + li, old_s, old_len) == 0) {
            const char *ns = new_s;
            while (*ns && bi < 1023) { buf[bi++] = *ns++; }
            li += old_len;
        } else {
            if (bi < 1023) buf[bi++] = line[li++];
            else li++;
        }
    }
    buf[bi] = '\0';
    kprintf("%s\n", buf);
}

void cmd_sed(const char *args) {
    if (!args) {
        kprintf("Usage: sed 's/old/new/' <file>\n");
        return;
    }

    /* Parse s/old/new/ */
    if (args[0] != '\'' || args[1] != 's' || args[2] != '/') {
        kprintf("Error: Only simple substitution 's/old/new/' is supported\n");
        return;
    }

    char old_s[64], new_s[64];
    int oi = 0, ni = 0;
    int i = 3;
    while (args[i] && args[i] != '/') {
        if (oi < 63) old_s[oi++] = args[i];
        i++;
    }
    old_s[oi] = '\0';
    i++; // skip /
    while (args[i] && args[i] != '/') {
        if (ni < 63) new_s[ni++] = args[i];
        i++;
    }
    new_s[ni] = '\0';

    /* Find the file argument */
    const char *file = NULL;
    const char *p = args;
    while (*p) {
        if (*p == '\'') {
            p++;
            while (*p && *p != '\'') p++;
            if (*p == '\'') {
                p++;
                while (*p == ' ') p++;
                if (*p) {
                    file = p;
                    break;
                }
            }
        } else {
            p++;
        }
    }

    if (!file) {
        kprintf("Error: No file specified\n");
        return;
    }

    /* Read file and process lines */
    char buf[1024];
    uint32_t size;
    if (libc_fs_read_file(file, buf, 1023, &size) != 0) {
        kprintf("Error reading file %s\n", file);
        return;
    }
    buf[size] = '\0';

    /* Simple line-by-line processing */
    char *line = buf;
    while (*line) {
        char *next_line = strchr(line, '\n');
        if (next_line) {
            *next_line = '\0';
            sed_replace(line, old_s, new_s);
            line = next_line + 1;
        } else {
            sed_replace(line, old_s, new_s);
            line = line + strlen(line);
        }
    }
}
