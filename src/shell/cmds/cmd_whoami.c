/* cmd_whoami.c — whoami command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_whoami(void) {
    struct user_session *s = session_get();
    if (!s || !s->logged_in) {
        kprintf("unknown\n");
        return;
    }
    kprintf("%s\n", s->username);
}
