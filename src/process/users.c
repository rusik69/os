/*
 * Multiuser subsystem
 *
 * Provides a simple in-memory user database with password hashing (djb2),
 * session tracking, and permission helpers.
 *
 * Default users created at boot:
 *   root   (uid=0, gid=0)
 *   guest  (uid=1000, gid=1000)
 */
#include "users.h"
#include "fs.h"
#include "string.h"
#include "printf.h"

#define DEFAULT_ROOT_PASSWORD  "root"
#define DEFAULT_GUEST_PASSWORD "guest"

static struct user_entry user_table[USER_MAX_ENTRIES];
static int               user_count = 0;

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
