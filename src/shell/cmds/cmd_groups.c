/* cmd_groups.c — print group memberships for the current user */
#include "shell_cmds.h"
#include "users.h"
#include "string.h"
#include "printf.h"

void cmd_groups(void) {
    struct user_session *cur = session_get();
    if (!cur || !cur->logged_in) {
        kprintf("(not logged in)\n");
        return;
    }

    /* Get the user's primary group name */
    char primary[GROUP_MAX_NAME];
    struct group_entry *pg = group_find_by_gid(cur->gid);
    if (pg) {
        memcpy(primary, pg->name, GROUP_MAX_NAME);
    } else {
        snprintf(primary, sizeof(primary), "%u", (unsigned)cur->gid);
    }

    /* Collect all supplementary groups for this user */
    struct group_entry *supp[GROUP_MAX_ENTRIES];
    int nsupp = groups_of_user(cur->uid, supp, GROUP_MAX_ENTRIES);

    kprintf("%s", primary);

    for (int i = 0; i < nsupp; i++) {
        /* Skip primary group — already printed */
        if (supp[i]->gid == cur->gid) continue;
        kprintf(" %s", supp[i]->name);
    }
    kprintf("\n");
}
