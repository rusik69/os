/* users.c — list user accounts from the kernel user table.
 *
 * Uses the value-returning SYS_USERS_GET_BY_INDEX syscall so it works from
 * ring-3 (the kernel user table pointer is not directly accessible).
 */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include <stdint.h>

#ifndef SYS_USERS_GET_BY_INDEX
#define SYS_USERS_GET_BY_INDEX 146
#endif
#ifndef USER_MAX_NAME
#define USER_MAX_NAME 32
#endif

struct users_user_entry {
    uint32_t uid;
    uint32_t gid;
    char username[USER_MAX_NAME];
    char home[128];
    uint8_t active;
    uint8_t reserved[3];
};

/* Raw syscall (SYS_USERS_GET_BY_INDEX, idx, &entry) -> 0 ok, -1 end */
static long sys_users_get_by_index(int idx, struct users_user_entry *e) {
    long ret;
    (void)e; /* referenced in asm below */
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_USERS_GET_BY_INDEX),
          "D"((long)idx),
          "S"((long)(unsigned long)e)
        : "rcx", "r11", "memory");
    return ret;
}

int main(void) {
    printf("UID   GID   USERNAME         HOME\n");

    int found = 0;
    for (int i = 0; i < 64; i++) {
        struct users_user_entry e;
        memset(&e, 0, sizeof(e));
        long rc = sys_users_get_by_index(i, &e);
        if (rc < 0)
            continue; /* no such index */
        if (!e.active || e.username[0] == '\0')
            continue;
        printf("%-5u %-5u %-16s %s\n", e.uid, e.gid, e.username, e.home);
        found++;
    }
    (void)found;
    return 0;
}
