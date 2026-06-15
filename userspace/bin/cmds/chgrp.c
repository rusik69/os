/* chgrp.c — change group ownership of file */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: chgrp GROUP FILE...\n");
        return 1;
    }

    unsigned int gid = (unsigned int)atoi(argv[1]);
    int ret = 0;

    for (int i = 2; i < argc; i++) {
        /* Stat the file to get current uid */
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            printf("chgrp: cannot stat '%s'\n", argv[i]);
            ret = 1;
            continue;
        }

        /* Use fs_chown to change group (uid unchanged) */
        if (fs_chown(argv[i], st.st_uid, gid) < 0) {
            printf("chgrp: failed to change group of '%s' to %u\n", argv[i], gid);
            ret = 1;
        }
    }

    return ret;
}
