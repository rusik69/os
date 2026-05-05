/* cmd_id.c — Print user/group identity */

#include "shell_cmds.h"
#include "printf.h"
#include "users.h"

void cmd_id(const char *args) {
    (void)args;
    struct user_session *s = session_get();
    struct user_entry ue;

    if (!s || !s->logged_in) {
        kprintf("uid=0(root) gid=0(root)\n");
        return;
    }

    if (user_find(s->username, &ue) == 0)
        kprintf("uid=%u(%s) gid=%u(%s)\n",
                (uint64_t)ue.uid, ue.username,
                (uint64_t)ue.gid, ue.username);
    else
        kprintf("uid=%u(%s) gid=%u\n",
                (uint64_t)s->uid, s->username,
                (uint64_t)s->gid);
}
