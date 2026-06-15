/* zforce.c — Force .gz extension on gzipped files */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: zforce <file...>\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        const char *path = argv[i];

        /* Check if already has .gz extension */
        unsigned long len = strlen(path);
        if (len >= 3 && strcmp(path + len - 3, ".gz") == 0)
            continue;

        /* Check gzip magic */
        int fd = open(path, O_RDONLY, 0);
        if (fd < 0) {
            printf("zforce: cannot open '%s'\n", path);
            continue;
        }
        unsigned char magic[2];
        int n = read(fd, magic, 2);
        close(fd);

        if (n == 2 && magic[0] == 0x1f && magic[1] == 0x8b) {
            /* It's gzipped — rename to add .gz */
            char newname[512];
            snprintf(newname, sizeof(newname), "%s.gz", path);
            if (rename(path, newname) == 0) {
                printf("zforce: renamed '%s' -> '%s'\n", path, newname);
            } else {
                printf("zforce: cannot rename '%s'\n", path);
            }
        }
    }

    return 0;
}
