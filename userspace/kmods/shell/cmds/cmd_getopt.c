/* cmd_getopt.c — parse getopt-style options */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_getopt(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: getopt <optstring> [parameters...]\n");
        kprintf("  Parses options using getopt-like semantics and shows results.\n");
        kprintf("  Example: getopt ab:c: -a -b foo -c bar file\n");
        return 1;
    }

    const char *optstring = argv[1];
    int optind = 2;  /* index into argv */

    kprintf("getopt: parsing with optstring='%s'\n", optstring);
    kprintf("  argc=%d argv:", argc);
    for (int i = 0; i < argc; i++)
        kprintf(" '%s'", argv[i]);
    kprintf("\n\n");

    if (optind >= argc) {
        kprintf("  (no parameters to parse)\n");
        return 0;
    }

    /* Simple getopt-like parser */
    int non_opts = 0;
    while (optind < argc) {
        const char *arg = argv[optind];

        /* End of options marker */
        if (strcmp(arg, "--") == 0) {
            optind++;
            break;
        }

        /* Not an option */
        if (arg[0] != '-') {
            break;
        }

        /* Single char option (no long options) */
        if (arg[1] == '\0') {
            kprintf("  found option '-' (bare dash)\n");
            optind++;
            continue;
        }

        /* Parse each character in the option string */
        for (int ci = 1; arg[ci] != '\0'; ci++) {
            char optchar = arg[ci];

            /* Check if this option requires a value */
            int requires_arg = 0;
            int found = 0;
            for (int osi = 0; optstring[osi] != '\0'; osi++) {
                if (optstring[osi] == optchar) {
                    found = 1;
                    if (optstring[osi + 1] == ':') {
                        requires_arg = 1;
                        /* Check for optional arg (::) */
                        if (optstring[osi + 2] == ':')
                            requires_arg = 2;  /* optional arg */
                    }
                    break;
                }
            }

            if (!found) {
                kprintf("  found illegal option '-%c'\n", optchar);
                continue;
            }

            if (requires_arg) {
                const char *optarg = NULL;
                if (requires_arg == 1) {
                    /* Mandatory arg: can be next argv or right after option */
                    if (arg[ci + 1] != '\0') {
                        /* arg is right after option char: -obar */
                        optarg = arg + ci + 1;
                        ci = (int)strlen(arg);  /* skip rest of this arg */
                    } else if (optind + 1 < argc) {
                        /* arg is next argv */
                        optind++;
                        optarg = argv[optind];
                    } else {
                        kprintf("  option '-%c' requires an argument (missing)\n", optchar);
                        break;
                    }
                } else {
                    /* Optional arg: check if it's right after or in next argv */
                    if (arg[ci + 1] != '\0') {
                        optarg = arg + ci + 1;
                        ci = (int)strlen(arg);
                    } else if (optind + 1 < argc && argv[optind + 1][0] != '-') {
                        optind++;
                        optarg = argv[optind];
                    }
                    /* else: no arg provided, that's OK for optional */
                }

                if (optarg)
                    kprintf("  option '-%c' with value '%s'\n", optchar, optarg);
                else
                    kprintf("  option '-%c' (no value)\n", optchar);
            } else {
                kprintf("  option '-%c'\n", optchar);
            }
        }
        optind++;
    }

    /* Remaining non-option arguments */
    kprintf("\n  remaining arguments:");
    if (optind >= argc && non_opts == 0) {
        kprintf(" (none)\n");
    } else {
        kprintf("\n");
        while (optind < argc) {
            kprintf("    [%d] '%s'\n", optind, argv[optind]);
            optind++;
        }
    }

    return 0;
}

void getopt_init(void)
{
    kprintf("[OK] cmd_getopt: option parser ready\n");
}
