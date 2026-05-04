/* cmd_mouse.c — mouse command */
#include "shell_cmds.h"
#include "printf.h"
#include "mouse.h"

void cmd_mouse_status(void) {
    int x, y;
    mouse_get_pos(&x, &y);
    uint8_t btn = mouse_get_buttons();
    kprintf("Mouse: x=%d y=%d buttons=0x%x (L=%d M=%d R=%d)\n",
            (uint64_t)x, (uint64_t)y, (uint64_t)btn,
            (uint64_t)(btn & 1), (uint64_t)((btn >> 2) & 1), (uint64_t)((btn >> 1) & 1));
}
