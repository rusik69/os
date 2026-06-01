/* cmd_whoami.c — print effective user name */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "types.h"

void cmd_whoami(void) {
    struct libc_user_session *s = libc_session_get();
    if (!s || !s->logged_in) {
        kprintf("root\n");
    } else {
        kprintf("%s\n", s->username);
    }
}
