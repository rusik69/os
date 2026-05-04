/* cmd_find.c — Search for files matching a pattern */

#include "shell_cmds.h"
#include "fs.h"
#include "printf.h"
#include "string.h"

/* Simple wildcard match: * matches any sequence, ? matches one char */
static int wildmatch(const char *pattern, const char *str) {
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1; /* trailing * matches all */
            while (*str) { if (wildmatch(pattern, str)) return 1; str++; }
            return 0;
        } else if (*pattern == '?') {
            if (!*str) return 0;
            pattern++; str++;
        } else {
            if (*pattern != *str) return 0;
            pattern++; str++;
        }
    }
    return *str == '\0';
}

void cmd_find(const char *args) {
    if (!args || !args[0]) { kprintf("Usage: find <pattern>\n"); return; }

    char pattern[64];
    strncpy(pattern, args, 63); pattern[63] = '\0';
    /* strip trailing spaces */
    int pl = strlen(pattern);
    while (pl > 0 && pattern[pl-1] == ' ') pattern[--pl] = '\0';

    /* List all files and match */
    char names[128][FS_MAX_NAME];
    int n = fs_list_names("/", 0, names, 128);

    int found = 0;
    for (int i = 0; i < n; i++) {
        if (wildmatch(pattern, names[i])) {
            kprintf("  /%s\n", names[i]);
            found++;
        }
    }
    if (!found) kprintf("No files matching '%s'\n", pattern);
}
