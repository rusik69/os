#ifndef USERS_H
#define USERS_H

#include "types.h"

#define USER_MAX_NAME    32
#define USER_MAX_PASS    64   /* stored as simple hash */
#define USER_MAX_HOME    64
#define USER_MAX_ENTRIES 16

struct user_entry {
    char    username[USER_MAX_NAME];
    uint32_t uid;
    uint32_t gid;
    char    home[USER_MAX_HOME];
    uint32_t pw_hash;   /* djb2 hash of password */
    int     active;     /* 1 = valid entry */
};

/* Session: currently logged-in user */
struct user_session {
    int     logged_in;
    uint32_t uid;
    uint32_t gid;
    char    username[USER_MAX_NAME];
};

/* ── /etc/group support (Item U22) ──────────────────────────────── */

#define GROUP_MAX_NAME     32
#define GROUP_MAX_ENTRIES  32
#define GROUP_MAX_MEMBERS  32   /* max users per group */

struct group_entry {
    char    name[GROUP_MAX_NAME];   /* group name */
    uint32_t gid;                   /* numeric group ID */
    char    password;               /* usually 'x' or '\0' */
    uint32_t members[GROUP_MAX_MEMBERS];  /* uid list of members */
    int     member_count;
    int     active;                 /* 1 = valid entry */
};

/* Initialize user and group subsystems (called at boot). */
void users_init(void);

/* User management */
int  user_add(const char *username, uint32_t uid, const char *password);
int  user_delete(const char *username);
int  user_find(const char *username, struct user_entry *out);
int  user_passwd(const char *username, const char *new_pass);
struct user_entry *users_get_table(void);
int  users_count(void);

/* Group management — /etc/group backed */
int  groups_init(void);                         /* parse /etc/group at boot */
int  group_add(const char *name, uint32_t gid); /* add a new group */
int  group_del(const char *name);               /* remove a group */
int  group_add_user(const char *group, uint32_t uid);  /* add user to group */
int  group_del_user(const char *group, uint32_t uid);  /* remove user from group */
struct group_entry *group_find(const char *name);
struct group_entry *group_find_by_gid(uint32_t gid);
struct group_entry *groups_get_table(void);
int  groups_count(void);
int  groups_of_user(uint32_t uid, struct group_entry **out, int max); /* get all groups for a user */

/* Check if user has access — returns 1 if user belongs to target_gid
 * (either as primary gid or as a supplementary group member). */
int  user_in_group(uint32_t uid, uint32_t gid);

/* Session management */
int  session_login(const char *username, const char *password);
void session_logout(void);
struct user_session *session_get(void);
int  session_is_root(void);

#endif
