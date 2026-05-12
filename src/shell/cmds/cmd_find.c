/* cmd_find.c — Search for files matching a pattern */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"

void cmd_find(const char *args) {
    if (!args || !args[0]) { kprintf("Usage: find <pattern>\n"); return; }

    char pattern[64];
    strncpy(pattern, args, 63); pattern[63] = '\0';
    /* strip trailing spaces */
    int pl = strlen(pattern);
    while (pl > 0 && pattern[pl-1] == ' ') pattern[--pl] = '\0';

    /* List all files and match using fnmatch for proper glob support */
    char names[128][FS_MAX_NAME];
    int n = fs_list_names("/", 0, names, 128);

    int found = 0;
    for (int i = 0; i < n; i++) {
        if (fnmatch(pattern, names[i], 0) == 0) {
            kprintf("  /%s\n", names[i]);
            found++;
        }
    }
    if (!found) kprintf("No files matching '%s'\n", pattern);
}
