/* cmd_wc.c — wc command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_wc(const char *args) {
    if (!args) { kprintf("Usage: wc <file>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strcpy(path + 1, args); }
    else strcpy(path, args);
    static char fbuf[4096];
    uint32_t size;
    if (fs_read_file(path, fbuf, sizeof(fbuf) - 1, &size) < 0) {
        kprintf("Cannot read: %s\n", path);
        return;
    }
    fbuf[size] = '\0';
    uint32_t lines = 0, words = 0;
    int in_word = 0;
    for (uint32_t i = 0; i < size; i++) {
        if (fbuf[i] == '\n') lines++;
        if (fbuf[i] == ' ' || fbuf[i] == '\n' || fbuf[i] == '\t') {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            words++;
        }
    }
    kprintf("  %u %u %u %s\n", (uint64_t)lines, (uint64_t)words,
            (uint64_t)size, args);
}
