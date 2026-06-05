/*
 * cmd_fsck.c — Online filesystem integrity check (fsck) shell command
 *
 * Usage:
 *   fsck [-v] [-q] [<mountpoint>]
 *
 * Options:
 *   -v    Verbose: show detailed progress for each block group
 *   -q    Quiet: suppress informational messages, show only errors
 *   -f    Fix: attempt to repair simple errors (WARNING: experimental)
 *
 * Without a mountpoint argument, checks the root filesystem ("/").
 */

#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "fsck.h"

void cmd_fsck(const char *args)
{
    int flags = FSCK_FLAG_VERBOSE;  /* default: show some output */
    const char *mountpoint = "/";
    int errors = 0;

    /* Parse simple arguments */
    if (args) {
        /* Tokenize by spaces */
        char buf[256];
        size_t alen = strlen(args);
        if (alen >= sizeof(buf)) alen = sizeof(buf) - 1;
        memcpy(buf, args, alen);
        buf[alen] = '\0';

        char *token = strtok(buf, " ");
        while (token) {
            if (token[0] == '-') {
                for (int i = 1; token[i]; i++) {
                    switch (token[i]) {
                        case 'v': flags |= FSCK_FLAG_VERBOSE;  break;
                        case 'q': flags &= ~FSCK_FLAG_VERBOSE;
                                  flags |= FSCK_FLAG_QUIET;    break;
                        case 'f': flags |= FSCK_FLAG_FIX;      break;
                        default:
                            kprintf("fsck: Unknown flag -%c\n", token[i]);
                            kprintf("Usage: fsck [-v] [-q] [-f] [<mountpoint>]\n");
                            return;
                    }
                }
            } else {
                mountpoint = token;
            }
            token = strtok(NULL, " ");
        }
    }

    kprintf("FSCK: Checking '%s'...\n", mountpoint);

    int ret = fsck_check(mountpoint, flags, &errors);

    if (ret < 0) {
        kprintf("FSCK: Error checking filesystem: %d\n", ret);
    } else if (errors == 0) {
        kprintf("FSCK: Filesystem is CLEAN.\n");
    } else {
        kprintf("FSCK: Found %d error(s).\n", errors);
    }
}
