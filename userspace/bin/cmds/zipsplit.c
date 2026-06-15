/* zipsplit.c — split zip file into chunks */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc != 4 || strcmp(argv[1], "-n") != 0) {
        printf("Usage: zipsplit -n <size> <zipfile>\n");
        return 1;
    }

    int chunk_size = atoi(argv[2]);
    if (chunk_size <= 0) {
        printf("zipsplit: invalid chunk size\n");
        return 1;
    }

    const char *filename = argv[3];

    int fd = open(filename, O_RDONLY, 0);
    if (fd < 0) {
        printf("zipsplit: cannot open '%s'\n", filename);
        return 1;
    }

    long total = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    if (total == 0) {
        printf("zipsplit: empty file\n");
        close(fd);
        return 1;
    }

    char *buf = malloc(total);
    if (!buf) {
        printf("zipsplit: out of memory\n");
        close(fd);
        return 1;
    }

    if (read(fd, buf, total) != total) {
        printf("zipsplit: read error\n");
        free(buf);
        close(fd);
        return 1;
    }
    close(fd);

    /* Build base name by stripping .zip extension if present */
    char base[256];
    int flen = strlen(filename);
    if (flen >= (int)sizeof(base)) flen = sizeof(base) - 1;
    memcpy(base, filename, flen);
    base[flen] = '\0';

    if (flen > 4 && strcmp(base + flen - 4, ".zip") == 0)
        base[flen - 4] = '\0';

    long offset = 0;
    int part = 1;

    while (offset < total) {
        long this_chunk = total - offset;
        if (this_chunk > chunk_size)
            this_chunk = chunk_size;

        char outname[300];
        int len = snprintf(outname, sizeof(outname), "%s.%03d", base, part);
        if (len < 0 || len >= (int)sizeof(outname)) {
            printf("zipsplit: output filename too long\n");
            free(buf);
            return 1;
        }

        int outfd = open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (outfd < 0) {
            printf("zipsplit: cannot create '%s'\n", outname);
            free(buf);
            return 1;
        }

        if (write(outfd, buf + offset, this_chunk) != this_chunk) {
            printf("zipsplit: write error on '%s'\n", outname);
            close(outfd);
            free(buf);
            return 1;
        }
        close(outfd);

        offset += this_chunk;
        part++;
    }

    free(buf);
    printf("zipsplit: created %d parts\n", part - 1);
    return 0;
}
