/* cmd_logname.c — print login name of current user */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_logname(const char *args) {
    (void)args;
    struct user_session *s = session_get();
    if (!s || !s->logged_in || !s->username[0]) {
        shell_set_exit_status(1);
        return;
    }
    kprintf("%s\n", s->username);
    shell_set_exit_status(0);
}
