/* getopt.c — parse command options */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: getopt <optstring> [args...]\n");
        return 1;
    }

    const char *optstring = argv[1];
    int optind = 2;

    /* Parse options from argv[2..] according to optstring */

    while (optind < argc) {
        const char *arg = argv[optind];
        if (arg[0] != '-' || arg[1] == 0) {
            break;  /* Non-option argument */
        }

        if (strcmp(arg, "--") == 0) {
            optind++;
            break;
        }

        /* Parse each option character in this argument */
        const char *opt = arg + 1;
        while (*opt) {
            char c = *opt;
            /* Check if this option exists in optstring */
            const char *found = strchr(optstring, c);
            if (!found) {
                /* Unknown option */
                printf("getopt: unknown option -%c\n", c);
                return 1;
            }

            /* Check if this option takes an argument */
            if (found[1] == ':') {
                /* Option takes argument */
                if (opt[1]) {
                    /* Argument is in the same argv element */
                    printf(" -%c %s", c, opt + 1);
                    opt = ""; /* Consumed rest */
                } else if (optind + 1 < argc) {
                    /* Argument is next argv element */
                    printf(" -%c %s", c, argv[optind + 1]);
                    optind++;
                } else {
                    printf("getopt: option -%c requires an argument\n", c);
                    return 1;
                }
            } else {
                printf(" -%c", c);
            }
            opt++;
        }
        optind++;
    }

    printf(" --");

    /* Print remaining non-option arguments */
    while (optind < argc) {
        printf(" %s", argv[optind]);
        optind++;
    }

    printf("\n");
    return 0;
}
