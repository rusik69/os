/* cmd_seq.c -- Print a sequence of numbers */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_seq(int argc, char **argv) {
    long first = 1, inc = 1, last;

    /* The shell wrapper passes args without a synthetic argv[0], so argv[0]
     * is the FIRST operand here. */
    if (argc < 1) {
        kprintf("Usage: seq [FIRST [INCREMENT]] LAST\n");
        return 1;
    }

    if (argc >= 1)
        last = atol(argv[0]);
    if (argc >= 2) {
        first = atol(argv[0]);
        last = atol(argv[1]);
    }
    if (argc >= 3) {
        first = atol(argv[0]);
        inc = atol(argv[1]);
        last  = atol(argv[2]);
    }
    if (argc > 3) {
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
