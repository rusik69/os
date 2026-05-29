/* cmd_bt.c — print kernel backtrace */

#include "fault.h"
#include "printf.h"

void cmd_bt(void) {
    kprintf("\n");
    arch_print_backtrace();
}
