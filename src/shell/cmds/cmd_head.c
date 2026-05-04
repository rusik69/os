/* cmd_head.c — head command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "fs.h"
#include "ata.h"

static uint32_t parse_uint(const char **s) {
    uint32_t v = 0;
    while (**s >= '0' && **s <= '9') { v = v * 10 + (**s - '0'); (*s)++; }
    return v;
}

void cmd_head(const char *args) {
    if (!args) { kprintf("Usage: head <file> [n]\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    const char *p = args;
    char name[64];
    int ni = 0;
    while (*p && *p != ' ' && ni < 63) name[ni++] = *p++;
    name[ni] = '\0';
    while (*p == ' ') p++;
    uint32_t n = 10;
    if (*p >= '0' && *p <= '9') n = parse_uint(&p);
    char path[64];
    if (name[0] != '/') { path[0] = '/'; strcpy(path + 1, name); }
    else strcpy(path, name);
    static char fbuf[4096];
    uint32_t size;
    if (fs_read_file(path, fbuf, sizeof(fbuf) - 1, &size) < 0) {
        kprintf("Cannot read: %s\n", name);
        return;
    }
    fbuf[size] = '\0';
    uint32_t line = 0;
    for (uint32_t i = 0; i < size && line < n; i++) {
        kprintf("%c", (uint64_t)(uint8_t)fbuf[i]);
        if (fbuf[i] == '\n') line++;
    }
    if (size > 0 && fbuf[size - 1] != '\n') kprintf("\n");
}
