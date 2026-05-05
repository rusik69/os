#include "shell_cmds.h"
#include "fs.h"
#include "users.h"
#include "string.h"
#include "printf.h"

/* chown <user>[:<group>] <path>
 * user/group are either numeric UIDs or usernames.
 */
void cmd_chown(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: chown <uid>[:<gid>] <path>\n");
        kprintf("  uid and gid are numeric or usernames\n");
        return;
    }

    const char *p = args;
    while (*p == ' ') p++;
    if (!*p) { kprintf("Usage: chown <uid>[:<gid>] <path>\n"); return; }

    /* Parse user[:group] */
    char owner[USER_MAX_NAME];
    int oi = 0;
    while (*p && *p != ' ' && *p != ':') owner[oi++] = *p++;
    owner[oi] = '\0';

    char grp[USER_MAX_NAME];
    int gi = 0;
    if (*p == ':') {
        p++;
        while (*p && *p != ' ') grp[gi++] = *p++;
    }
    grp[gi] = '\0';

    while (*p == ' ') p++;
    if (!*p) { kprintf("chown: missing path\n"); return; }

    char path[64];
    if (*p != '/') { path[0] = '/'; strcpy(path + 1, p); }
    else strcpy(path, p);

    /* Resolve uid: numeric or username */
    uint16_t uid_val = 0, gid_val = 0;
    if (owner[0] >= '0' && owner[0] <= '9') {
        for (int i = 0; owner[i]; i++)
            uid_val = (uint16_t)(uid_val * 10 + (owner[i] - '0'));
    } else {
        struct user_entry ue;
        if (user_find(owner, &ue) < 0) {
            kprintf("chown: unknown user '%s'\n", owner);
            return;
        }
        uid_val = (uint16_t)ue.uid;
        gid_val = (uint16_t)ue.gid;
    }

    if (gi > 0) {
        if (grp[0] >= '0' && grp[0] <= '9') {
            gid_val = 0;
            for (int i = 0; grp[i]; i++)
                gid_val = (uint16_t)(gid_val * 10 + (grp[i] - '0'));
        } else {
            struct user_entry ue;
            if (user_find(grp, &ue) < 0) {
                kprintf("chown: unknown group '%s'\n", grp);
                return;
            }
            gid_val = (uint16_t)ue.gid;
        }
    }

    int rc = fs_chown(path, uid_val, gid_val);
    if (rc == -1) kprintf("chown: not found: %s\n", path);
    else if (rc == -2) kprintf("chown: permission denied (root only)\n");
    else kprintf("owner changed: %s -> uid=%u gid=%u\n",
                 path, (uint64_t)uid_val, (uint64_t)gid_val);
}
