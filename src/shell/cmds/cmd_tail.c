/* cmd_tail.c -- tail command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

static uint32_t parse_uint2(const char **s) {
    uint32_t v = 0;
    while (**s >= '0' && **s <= '9') { v = v * 10 + (**s - '0'); (*s)++; }
    return v;
}

void cmd_tail(const char *args) {
    static char fbuf[4096];
    uint32_t size = 0;
    uint32_t n = 10;

    const char *p = args ? args : "";
    if (*p == '-' && *(p+1) == 'n') {
        p += 2; while (*p == ' ') p++;
        n = parse_uint2(&p);
        while (*p == ' ') p++;
    }

    if (!*p) {
        if (!shell_has_stdin()) { kprintf("Usage: tail [-n N] <file>\n"); return; }
        size = (uint32_t)shell_stdin_read(fbuf, (int)sizeof(fbuf) - 1);
    } else {
        char name[64]; int ni = 0;
        while (*p && *p != ' ' && ni < 63) name[ni++] = *p++;
        name[ni] = '\0';
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') n = parse_uint2(&p);
        char path[64];
        if (name[0] != '/')
            snprintf(path, sizeof(path), "/%s", name);
        else
            snprintf(path, sizeof(path), "%s", name);
        if (!ata_is_present()) { kprintf("No disk\n"); return; }
        if (fs_read_file(path, fbuf, sizeof(fbuf) - 1, &size) < 0) {
            kprintf("Cannot read: %s\n", name);
            return;
        }
    }
    fbuf[size] = '\0';
    uint32_t total_lines = 0;
    for (uint32_t i = 0; i < size; i++)
        if (fbuf[i] == '\n') total_lines++;
    uint32_t skip = (total_lines > n) ? (total_lines - n) : 0;
    uint32_t line = 0;
    for (uint32_t i = 0; i < size; i++) {
        if (line >= skip) kprintf("%c", (unsigned int)(unsigned char)fbuf[i]);
        if (fbuf[i] == '\n') line++;
    }
    if (size > 0 && fbuf[size - 1] != '\n') kprintf("\n");
}
