/*
 * Multiuser subsystem
 *
 * Provides a simple in-memory user database with password hashing (djb2),
 * session tracking, permission helpers, and /etc/group support.
 *
 * Default users created at boot:
 *   root   (uid=0, gid=0)
 *   guest  (uid=1000, gid=1000)
 *
 * Default groups created at boot:
 *   root   (gid=0)
 *   wheel  (gid=10) — administrative group
 *   users  (gid=100) — regular users
 *   guest  (gid=1000)
 */
#include "users.h"
#include "fs.h"
#include "string.h"
#include "printf.h"

#define DEFAULT_ROOT_PASSWORD  "root"
#define DEFAULT_GUEST_PASSWORD "guest"

#define GROUP_FILE  "/etc/group"
#define GROUP_LINE_MAX  256

static struct user_entry  user_table[USER_MAX_ENTRIES];
static int                user_count = 0;

static struct group_entry group_table[GROUP_MAX_ENTRIES];
static int                group_count = 0;

static struct user_session current_session;

/* ── Password hashing ─────────────────────────────────────────────────────── */
static uint32_t djb2_hash(const char *s) {
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) ^ (uint8_t)*s++;
    return h;
}

static int ensure_home_owned(const char *path, uint16_t uid, uint16_t gid, uint16_t mode) {
    uint8_t type = 0;
    if (fs_stat(path, (uint32_t *)0, &type) < 0) {
        if (fs_create(path, FS_TYPE_DIR) < 0) return -1;
    } else if (type != FS_TYPE_DIR) {
        return -1;
    }

    if (fs_chown(path, uid, gid) < 0) return -1;
    if (fs_chmod(path, mode) < 0) return -1;
    return 0;
}

/* ── /etc/group file I/O helpers ──────────────────────────────────────────── */

/* Write the entire group table to /etc/group.
 * Format: group_name:password:GID:user_list (one per line).
 * Returns 0 on success, -1 on failure. */
static int group_file_write(void) {
    char buf[2048];
    int  off = 0;

    for (int i = 0; i < GROUP_MAX_ENTRIES; i++) {
        if (!group_table[i].active) continue;

        /* group_name:password:GID:user_list */
        int n = snprintf(buf + off, (int)sizeof(buf) - off, "%s:%c:%u:",
                         group_table[i].name,
                         group_table[i].password ? group_table[i].password : 'x',
                         (unsigned)group_table[i].gid);
        if (n < 0 || off + n >= (int)sizeof(buf) - 2) break;
        off += n;

        /* Append comma-separated member UIDs */
        for (int j = 0; j < group_table[i].member_count; j++) {
            /* Resolve UID to username for readability */
            char username[USER_MAX_NAME];
            int  found = 0;
            for (int k = 0; k < USER_MAX_ENTRIES; k++) {
                if (user_table[k].active && user_table[k].uid == group_table[i].members[j]) {
                    memcpy(username, user_table[k].username, USER_MAX_NAME);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                snprintf(username, sizeof(username), "%u", (unsigned)group_table[i].members[j]);
            }
            int remaining = (int)sizeof(buf) - off;
            int written = snprintf(buf + off, remaining, "%s%s",
                                   j > 0 ? "," : "", username);
            if (written < 0 || off + written >= (int)sizeof(buf) - 2) break;
            off += written;
        }

        /* Terminate line */
        if (off < (int)sizeof(buf) - 2) {
            buf[off++] = '\n';
        }
    }

    buf[off] = '\0';

    /* Write the file (truncate + rewrite) */
    if (fs_write_file(GROUP_FILE, buf, (uint32_t)off) < 0) {
        /* File may not exist yet — create it */
        if (fs_create(GROUP_FILE, FS_TYPE_FILE) < 0)
            return -1;
        if (fs_write_file(GROUP_FILE, buf, (uint32_t)off) < 0)
            return -1;
    }
    return 0;
}

/* Parse a single /etc/group line into a group_entry.
 * Line format: group_name:password:GID:user_list
 * (user_list is comma-separated usernames).
 * Returns 0 on success, -1 on parse error. */
static int parse_group_line(const char *line, struct group_entry *ge) {
    if (!line || !*line || line[0] == '#') return -1;

    memset(ge, 0, sizeof(*ge));
    ge->password = 'x';

    /* Parse: group_name : password : GID : user_list */
    const char *p = line;

    /* Group name */
    int i = 0;
    while (*p && *p != ':' && i < GROUP_MAX_NAME - 1)
        ge->name[i++] = *p++;
    ge->name[i] = '\0';
    if (i == 0) return -1;
    if (*p != ':') return -1;
    p++; /* skip ':' */

    /* Password field */
    if (*p && *p != ':')
        ge->password = *p;
    while (*p && *p != ':') p++;
    if (*p != ':') return -1;
    p++;

    /* GID */
    uint32_t gid = 0;
    while (*p && *p != ':') {
        if (*p >= '0' && *p <= '9')
            gid = gid * 10 + (uint32_t)(*p - '0');
        else
            return -1;  /* non-numeric GID */
        p++;
    }
    ge->gid = gid;
    if (*p != ':') return -1;
    p++;

    /* User list (comma-separated usernames, resolve to UIDs) */
    while (*p && *p != '\n' && *p != '\r') {
        /* Skip leading commas/spaces */
        while (*p == ',' || *p == ' ') p++;
        if (!*p || *p == '\n' || *p == '\r') break;

        char uname[USER_MAX_NAME];
        int  ui = 0;
        while (*p && *p != ',' && *p != '\n' && *p != '\r' && ui < USER_MAX_NAME - 1)
            uname[ui++] = *p++;
        uname[ui] = '\0';

        if (ui > 0) {
            /* Resolve username to UID */
            uint32_t uid = 0;
            int found = 0;
            for (int k = 0; k < USER_MAX_ENTRIES; k++) {
                if (user_table[k].active && strcmp(user_table[k].username, uname) == 0) {
                    uid = user_table[k].uid;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                /* Try parsing as numeric UID */
                uid = 0;
                const char *up = uname;
                while (*up >= '0' && *up <= '9')
                    uid = uid * 10 + (uint32_t)(*up++ - '0');
            }

            if (ge->member_count < GROUP_MAX_MEMBERS) {
                /* Check for duplicate */
                int dup = 0;
                for (int m = 0; m < ge->member_count; m++) {
                    if (ge->members[m] == uid) { dup = 1; break; }
                }
                if (!dup)
                    ge->members[ge->member_count++] = uid;
            }
        }
    }

    ge->active = 1;
    return 0;
}

/* ── Group initialization at boot ──────────────────────────────────────────── */

int groups_init(void) {
    memset(group_table, 0, sizeof(group_table));
    group_count = 0;

    /* Try to read /etc/group */
    char buf[2048];
    uint32_t size = 0;
    if (fs_read_file(GROUP_FILE, buf, (uint32_t)sizeof(buf) - 1, &size) == 0 && size > 0) {
        buf[size] = '\0';

        /* Parse each line */
        char *line = buf;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';

            struct group_entry ge;
            if (parse_group_line(line, &ge) == 0 && group_count < GROUP_MAX_ENTRIES) {
                /* Check for duplicate GID */
                int dup = 0;
                for (int i = 0; i < group_count; i++) {
                    if (group_table[i].active && group_table[i].gid == ge.gid) {
                        dup = 1; break;
                    }
                }
                if (!dup) {
                    memcpy(&group_table[group_count], &ge, sizeof(ge));
                    group_count++;
                }
            }

            if (nl) {
                *nl = '\n';
                line = nl + 1;
            } else {
                break;
            }
        }
    }

    /* If no groups loaded (first boot), create defaults */
    if (group_count == 0) {
        /* Create group entries */
        int idx = 0;
        memset(&group_table[idx], 0, sizeof(group_table[idx]));
        memcpy(group_table[idx].name, "root", 5);
        group_table[idx].gid = 0;
        group_table[idx].password = 'x';
        group_table[idx].members[group_table[idx].member_count++] = 0;
        group_table[idx].active = 1;
        idx++;

        memset(&group_table[idx], 0, sizeof(group_table[idx]));
        memcpy(group_table[idx].name, "wheel", 6);
        group_table[idx].gid = 10;
        group_table[idx].password = 'x';
        group_table[idx].members[group_table[idx].member_count++] = 0;
        group_table[idx].active = 1;
        idx++;

        memset(&group_table[idx], 0, sizeof(group_table[idx]));
        memcpy(group_table[idx].name, "users", 6);
        group_table[idx].gid = 100;
        group_table[idx].password = 'x';
        group_table[idx].active = 1;
        idx++;

        memset(&group_table[idx], 0, sizeof(group_table[idx]));
        memcpy(group_table[idx].name, "guest", 6);
        group_table[idx].gid = 1000;
        group_table[idx].password = 'x';
        group_table[idx].members[group_table[idx].member_count++] = 1000;
        group_table[idx].active = 1;
        idx++;

        group_count = idx;
        group_file_write();
    }

    return 0;
}

/* ── Group management functions ───────────────────────────────────────────── */

int group_add(const char *name, uint32_t gid) {
    if (!name || !*name) return -1;
    if (group_count >= GROUP_MAX_ENTRIES) return -1;

    /* Check for duplicate name or GID */
    for (int i = 0; i < GROUP_MAX_ENTRIES; i++) {
        if (!group_table[i].active) continue;
        if (strcmp(group_table[i].name, name) == 0) return -2;  /* already exists */
        if (group_table[i].gid == gid) return -3;               /* GID taken */
    }

    /* Find free slot */
    for (int i = 0; i < GROUP_MAX_ENTRIES; i++) {
        if (!group_table[i].active) {
            memset(&group_table[i], 0, sizeof(group_table[i]));
            strncpy(group_table[i].name, name, GROUP_MAX_NAME - 1);
            group_table[i].name[GROUP_MAX_NAME - 1] = '\0';
            group_table[i].gid = gid;
            group_table[i].password = 'x';
            group_table[i].active = 1;
            group_count++;
            group_file_write();
            return 0;
        }
    }
    return -1;
}

int group_del(const char *name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < GROUP_MAX_ENTRIES; i++) {
        if (!group_table[i].active) continue;
        if (strcmp(group_table[i].name, name) == 0) {
            group_table[i].active = 0;
            group_count--;
            group_file_write();
            return 0;
        }
    }
    return -2;  /* not found */
}

int group_add_user(const char *group, uint32_t uid) {
    if (!group || !*group) return -1;
    for (int i = 0; i < GROUP_MAX_ENTRIES; i++) {
        if (!group_table[i].active) continue;
        if (strcmp(group_table[i].name, group) == 0) {
            /* Check for duplicates */
            for (int j = 0; j < group_table[i].member_count; j++) {
                if (group_table[i].members[j] == uid) return 0;  /* already a member */
            }
            if (group_table[i].member_count >= GROUP_MAX_MEMBERS) return -3;
            group_table[i].members[group_table[i].member_count++] = uid;
            group_file_write();
            return 0;
        }
    }
    return -2;  /* group not found */
}

int group_del_user(const char *group, uint32_t uid) {
    if (!group || !*group) return -1;
    for (int i = 0; i < GROUP_MAX_ENTRIES; i++) {
        if (!group_table[i].active) continue;
        if (strcmp(group_table[i].name, group) == 0) {
            for (int j = 0; j < group_table[i].member_count; j++) {
                if (group_table[i].members[j] == uid) {
                    /* Remove by shifting remaining members down */
                    for (int k = j; k < group_table[i].member_count - 1; k++)
                        group_table[i].members[k] = group_table[i].members[k + 1];
                    group_table[i].member_count--;
                    group_file_write();
                    return 0;
                }
            }
            return -3;  /* user not in group */
        }
    }
    return -2;  /* group not found */
}

struct group_entry *group_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < GROUP_MAX_ENTRIES; i++) {
        if (group_table[i].active && strcmp(group_table[i].name, name) == 0)
            return &group_table[i];
    }
    return NULL;
}

struct group_entry *group_find_by_gid(uint32_t gid) {
    for (int i = 0; i < GROUP_MAX_ENTRIES; i++) {
        if (group_table[i].active && group_table[i].gid == gid)
            return &group_table[i];
    }
    return NULL;
}

struct group_entry *groups_get_table(void) { return group_table; }
int  groups_count(void) { return group_count; }

/* Return all groups a user (identified by uid) belongs to.
 * Returns number of groups found, or -1 on error. */
int groups_of_user(uint32_t uid, struct group_entry **out, int max) {
    if (!out || max <= 0) return -1;
    int count = 0;

    for (int i = 0; i < GROUP_MAX_ENTRIES && count < max; i++) {
        if (!group_table[i].active) continue;

        /* Check if user is a member of this group */
        for (int j = 0; j < group_table[i].member_count; j++) {
            if (group_table[i].members[j] == uid) {
                out[count++] = &group_table[i];
                break;
            }
        }
    }
    return count;
}

/* Check if a user (by uid) has access to the given gid.
 * Returns 1 if the user's primary gid matches or if the user is a
 * member of a supplementary group with that gid. */
int user_in_group(uint32_t uid, uint32_t gid) {
    /* Check primary gid: walk user table */
    for (int i = 0; i < USER_MAX_ENTRIES; i++) {
        if (user_table[i].active && user_table[i].uid == uid) {
            if (user_table[i].gid == gid) return 1;
            break;
        }
    }

    /* Check supplementary groups */
    for (int i = 0; i < GROUP_MAX_ENTRIES; i++) {
        if (!group_table[i].active || group_table[i].gid != gid) continue;
        for (int j = 0; j < group_table[i].member_count; j++) {
            if (group_table[i].members[j] == uid) return 1;
        }
    }
    return 0;
}

/* ── User management ──────────────────────────────────────────────────────── */
void users_init(void) {
    memset(user_table, 0, sizeof(user_table));
    user_count = 0;
    memset(&current_session, 0, sizeof(current_session));

    current_session.logged_in = 1;
    current_session.uid = 0;
    current_session.gid = 0;
    memcpy(current_session.username, "root", 5);

    if (fs_stat("/home", (uint32_t *)0, (uint8_t *)0) < 0)
        fs_create("/home", FS_TYPE_DIR);

    /* Create default users with non-empty passwords */
    user_add("root",  0,    DEFAULT_ROOT_PASSWORD);
    user_add("guest", 1000, DEFAULT_GUEST_PASSWORD);

    /* Initialize /etc/group */
    groups_init();
}

int user_add(const char *username, uint32_t uid, const char *password) {
    if (!username || !*username) return -4;
    if (!password || !*password) return -4;
    if (user_count >= USER_MAX_ENTRIES) return -1;
    /* Check duplicate */
    for (int i = 0; i < USER_MAX_ENTRIES; i++) {
        if (!user_table[i].active) continue;
        if (strcmp(user_table[i].username, username) == 0) return -2;
    }
    /* Find a free slot */
    for (int i = 0; i < USER_MAX_ENTRIES; i++) {
        if (user_table[i].active) continue;
        memset(&user_table[i], 0, sizeof(user_table[i]));
        strncpy(user_table[i].username, username, USER_MAX_NAME - 1);
        user_table[i].uid     = uid;
        user_table[i].gid     = uid;  /* default gid = uid */
        user_table[i].pw_hash = djb2_hash(password);
        /* Home directory */
        if (uid == 0)
            memcpy(user_table[i].home, "/root", 6);
        else {
            memcpy(user_table[i].home, "/home/", 6);
            strncpy(user_table[i].home + 6, username, USER_MAX_HOME - 7);
            user_table[i].home[USER_MAX_HOME - 1] = '\0';
        }
        user_table[i].active = 1;
        uint16_t home_mode = (uid == 0) ? 0700 : 0750;
        if (ensure_home_owned(user_table[i].home, (uint16_t)uid, (uint16_t)uid, home_mode) < 0) {
            user_table[i].active = 0;
            return -5;
        }
        user_count++;
        return 0;
    }
    return -3;
}

int user_delete(const char *username) {
    if (strcmp(username, "root") == 0) return -1;  /* cannot delete root */
    for (int i = 0; i < USER_MAX_ENTRIES; i++) {
        if (user_table[i].active && strcmp(user_table[i].username, username) == 0) {
            user_table[i].active = 0;
            user_count--;
            return 0;
        }
    }
    return -2;
}

int user_find(const char *username, struct user_entry *out) {
    for (int i = 0; i < USER_MAX_ENTRIES; i++) {
        if (user_table[i].active && strcmp(user_table[i].username, username) == 0) {
            if (out) memcpy(out, &user_table[i], sizeof(*out));
            return 0;
        }
    }
    return -1;
}

int user_passwd(const char *username, const char *new_pass) {
    if (!new_pass || !*new_pass) return -2;
    for (int i = 0; i < USER_MAX_ENTRIES; i++) {
        if (user_table[i].active && strcmp(user_table[i].username, username) == 0) {
            user_table[i].pw_hash = djb2_hash(new_pass);
            return 0;
        }
    }
    return -1;
}

struct user_entry *users_get_table(void) { return user_table; }
int  users_count(void) { return user_count; }

/* ── Session management ───────────────────────────────────────────────────── */
int session_login(const char *username, const char *password) {
    struct user_entry ue;
    if (user_find(username, &ue) != 0) return -1;  /* user not found */

    /* Check password */
    uint32_t pw_hash = password && *password ? djb2_hash(password) : 0;
    if (ue.pw_hash != pw_hash) return -2;  /* wrong password */

    current_session.logged_in = 1;
    current_session.uid = ue.uid;
    current_session.gid = ue.gid;
    strncpy(current_session.username, username, USER_MAX_NAME - 1);
    return 0;
}

void session_logout(void) {
    memset(&current_session, 0, sizeof(current_session));
    /* After logout, fall back to guest */
    current_session.logged_in = 1;
    current_session.uid = 1000;
    current_session.gid = 1000;
    memcpy(current_session.username, "guest", 6);
}

struct user_session *session_get(void) { return &current_session; }

int session_is_root(void) { return current_session.uid == 0; }
