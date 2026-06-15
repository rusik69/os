/* chown.c — change file owner and group */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: chown OWNER[:GROUP] FILE...\n");
        return 1;
    }

    const char *spec = argv[1];
    unsigned int uid;
    unsigned int gid;
    int has_group = 0;

    /* Parse "owner[:group]" */
    const char *colon = strchr(spec, ':');
    if (colon) {
        /* Has both owner and group */
        char owner[32];
        unsigned long own_len = (unsigned long)(colon - spec);
        if (own_len > sizeof(owner) - 1) own_len = sizeof(owner) - 1;
        memcpy(owner, spec, own_len);
        owner[own_len] = '\0';
        uid = (unsigned int)atoi(owner);
        gid = (unsigned int)atoi(colon + 1);
        has_group = 1;
    } else {
        /* Just owner */
        uid = (unsigned int)atoi(spec);
        gid = 0;  /* placeholder, will be overwritten by stat */
    }

    int ret = 0;

    for (int i = 2; i < argc; i++) {
        unsigned int gid_use;

        if (has_group) {
            gid_use = gid;
        } else {
            /* Get current group from file stat */
            struct stat st;
            if (stat(argv[i], &st) < 0) {
                printf("chown: cannot stat '%s'\n", argv[i]);
                ret = 1;
                continue;
            }
            gid_use = st.st_gid;
        }

        if (fs_chown(argv[i], uid, gid_use) < 0) {
            printf("chown: failed to change owner of '%s' to %u", argv[i], uid);
            if (has_group) printf(":%u", gid_use);
            printf("\n");
            ret = 1;
        }
    }

    return ret;
}
