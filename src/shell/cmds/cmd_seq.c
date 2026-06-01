/* cmd_seq.c -- Print a sequence of numbers */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_seq(int argc, char **argv) {
    long first = 1, inc = 1, last;

    if (argc < 2) {
        kprintf("Usage: seq [FIRST [INCREMENT]] LAST\n");
        return 1;
    }

    if (argc == 2) {
        last = atol(argv[1]);
    } else if (argc == 3) {
        first = atol(argv[1]);
        last  = atol(argv[2]);
    } else if (argc == 4) {
        first = atol(argv[1]);
        inc   = atol(argv[2]);
        last  = atol(argv[3]);
    } else {
        kprintf("seq: too many arguments\n");
        return 1;
    }

    if (inc == 0) {
        kprintf("seq: increment cannot be zero\n");
        return 1;
    }

    if (inc > 0) {
        for (long i = first; i <= last; i += inc)
            kprintf("%ld\n", i);
    } else {
        for (long i = first; i >= last; i += inc)
            kprintf("%ld\n", i);
    }
    return 0;
}
