/* cmd_cmp.c — compare two files byte-by-byte */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_cmp(const char *args) {
    if (!args || !*args) { kprintf("Usage: cmp <file1> <file2>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }

    char f1[64], f2[64];
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < 63) f1[i++] = *p++;
    f1[i] = '\0';
    while (*p == ' ') p++;
    i = 0;
    while (*p && *p != ' ' && i < 63) f2[i++] = *p++;
    f2[i] = '\0';

    if (!f1[0] || !f2[0]) { kprintf("Usage: cmp <file1> <file2>\n"); return; }

    char p1[64], p2[64];
    if (f1[0] != '/') { p1[0] = '/'; strncpy(p1+1, f1, 62); p1[63] = '\0'; }
    else strncpy(p1, f1, 63);
    p1[63] = '\0';
    if (f2[0] != '/') { p2[0] = '/'; strncpy(p2+1, f2, 62); p2[63] = '\0'; }
    else strncpy(p2, f2, 63);
    p2[63] = '\0';

    static char buf1[4096], buf2[4096];
    uint32_t s1, s2;
    if (fs_read_file(p1, buf1, sizeof(buf1), &s1) < 0) { kprintf("Cannot read: %s\n", f1); return; }
    if (fs_read_file(p2, buf2, sizeof(buf2), &s2) < 0) { kprintf("Cannot read: %s\n", f2); return; }

    uint32_t min = s1 < s2 ? s1 : s2;
    uint32_t line = 1, col = 1;
    for (uint32_t j = 0; j < min; j++) {
        if (buf1[j] != buf2[j]) {
            kprintf("%s %s differ: byte %u, line %u\n", f1, f2, (unsigned long)(j+1), (unsigned long)line);
            return;
        }
        if (buf1[j] == '\n') { line++; col = 1; } else col++;
    }
    if (s1 != s2)
        kprintf("%s %s differ: size %u vs %u\n", f1, f2, (unsigned long)s1, (unsigned long)s2);
    else
        kprintf("%s %s are identical\n", f1, f2);
}
