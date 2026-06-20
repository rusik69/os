/*
 * cmd_fsck.c — Online filesystem integrity check (fsck) shell command
 *
 * Items S148, S153 — ext2 consistency check
 *
 * Usage:
 *   fsck [-a] [-c] [-f] [-v] [-q] [-y] [<mountpoint>]
 *
 * Options:
 *   -a    Auto-repair mode: fix link counts, salvage orphans to lost+found
 *   -c    Check blocks: verify all blocks (scan for cross-linked/free metadata)
 *   -f    Force check even if filesystem appears clean
 *   -v    Verbose: show detailed progress for each block group
 *   -q    Quiet: suppress informational messages, show only errors
 *   -y    Assume yes to all prompts (non-interactive)
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
    int flags = 0;  /* default: quiet output unless -v */
    const char *mountpoint = "/";
    int errors = 0;

    /* Parse simple arguments */
    if (args) {
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
                        case 'f':
                            /* -f means force check */
                            flags |= FSCK_FLAG_FORCE;
                            /* Also set FIX flag for consistency */
                            flags |= FSCK_FLAG_FIX;
                            break;
                        case 'a':
                            flags |= FSCK_FLAG_AUTO_REPAIR;
                            flags |= FSCK_FLAG_FIX;  /* auto-repair implies fix */
                            break;
                        case 'c':
                            flags |= FSCK_FLAG_CHECK_BLOCKS;
                            break;
                        case 'y':
                            flags |= FSCK_FLAG_ASSUME_YES;
                            break;
                        default:
                            kprintf("fsck: Unknown flag -%c\n", token[i]);
                            kprintf("Usage: fsck [-a] [-c] [-f] [-v] [-q] [-y] [<mountpoint>]\n");
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

    /* Report flags */
    if (flags & FSCK_FLAG_AUTO_REPAIR)
        kprintf("FSCK: Auto-repair mode enabled\n");
    if (flags & FSCK_FLAG_CHECK_BLOCKS)
        kprintf("FSCK: Block checking enabled\n");
    if (flags & FSCK_FLAG_FORCE)
        kprintf("FSCK: Force check enabled\n");

    int ret = fsck_check(mountpoint, flags, &errors);

    if (ret < 0) {
        kprintf("FSCK: Error checking filesystem: %d\n", ret);
    } else if (errors == 0) {
        kprintf("FSCK: Filesystem is CLEAN.\n");
    } else {
        kprintf("FSCK: Found %d error(s). Use -a for auto-repair or -f to fix.\n", errors);
    }
}
