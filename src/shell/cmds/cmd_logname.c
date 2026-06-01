/* cmd_logname.c — print user's login name */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "types.h"

void cmd_logname(void) {
    struct libc_user_session *s = libc_session_get();
    if (!s || !s->logged_in) {
        kprintf("root\n");
    } else {
        kprintf("%s\n", s->username);
    }
}
