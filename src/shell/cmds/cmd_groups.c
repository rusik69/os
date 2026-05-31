/* cmd_groups.c — print group memberships for current user */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"

void cmd_groups(const char *args) {
    (void)args;
    struct user_session *s = session_get();
    if (!s || !s->logged_in) {
        kprintf("users\n");
        return;
    }
    /* For the simple user model we just print the user's name and "users" group */
    kprintf("%s users\n", s->username);
}
