/* cmd_mouse.c — mouse command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_mouse_status(void) {
    struct mouse_state state = {0};
    mouse_get_state(&state);
    uint8_t btn = state.buttons;
    kprintf("Mouse: x=%d y=%d buttons=0x%x (L=%d M=%d R=%d)\n",
            (uint64_t)state.x, (uint64_t)state.y, (uint64_t)btn,
            (uint64_t)(btn & 1), (uint64_t)((btn >> 2) & 1), (uint64_t)((btn >> 1) & 1));
}
