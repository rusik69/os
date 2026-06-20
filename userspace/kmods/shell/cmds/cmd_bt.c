#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
void cmd_bt(void) {
    extern void print_stack_trace(void);
    print_stack_trace();
}
