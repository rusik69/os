#include "libc.h"
#include "printf.h"
#include "shell.h"
#include "shell_cmds.h"
#include "string.h"

void cmd_mouse_status(void) {
    struct libc_mouse_state st;
    if (libc_mouse_get_state(&st) == 0) {
        kprintf("Mouse: PS/2 on IRQ 12\n");
        kprintf("  x=%d y=%d buttons=%d\n", st.x, st.y, (int)st.buttons);
    } else {
        kprintf("Mouse: PS/2 on IRQ 12\n");
        kprintf("  x=0 y=0 buttons=0\n");
    }
}
