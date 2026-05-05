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

void users_init(void);

/* User management */
int  user_add(const char *username, uint32_t uid, const char *password);
int  user_delete(const char *username);
int  user_find(const char *username, struct user_entry *out);
int  user_passwd(const char *username, const char *new_pass);
struct user_entry *users_get_table(void);
int  users_count(void);

/* Session management */
int  session_login(const char *username, const char *password);
void session_logout(void);
struct user_session *session_get(void);
int  session_is_root(void);

#endif
