#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_yes(int argc, char **argv) {
    /* The shell wrapper passes args without a synthetic argv[0], so argv[0]
     * is the FIRST operand here. */
    const char *str = "y";
    if (argc >= 1)
        str = argv[0];

    /* Bound output so a bare `yes` cannot wedge the shell: an unbounded loop
     * would never return a prompt, and a very large burst can overflow the
     * telnet/serial output buffer and drop the session. A modest number of
     * lines is plenty to demonstrate repetition and stays safe. */
    enum { YES_MAX_LINES = 16 };
    for (int i = 0; i < YES_MAX_LINES; i++) {
        kprintf("%s\n", str);
    }
    return 0;
}
