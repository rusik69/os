/* cmd_mouse.c — mouse command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_mouse_status(void) {
    struct mouse_state state = {0};
    mouse_get_state(&state);
    uint8_t btn = state.buttons;
    kprintf("Mouse: x=%d y=%d buttons=0x%x (L=%d M=%d R=%d)\n",
            (int)state.x, (int)state.y, (unsigned int)btn,
            (int)(btn & 1), (int)((btn >> 2) & 1), (int)((btn >> 1) & 1));
}
