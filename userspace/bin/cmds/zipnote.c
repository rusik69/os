/* zipnote.c — read/write zip central directory comment */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

/* End-of-central-directory signature in little-endian: PK\x05\x06 */
static const unsigned char EOCD_SIG[4] = {0x50, 0x4b, 0x05, 0x06};

int main(int argc, char *argv[]) {
    int write_mode = 0;
    const char *filename;
    const char *new_comment = NULL;

    if (argc == 2) {
        write_mode = 0;
        filename = argv[1];
    } else if (argc == 4 && strcmp(argv[1], "-w") == 0) {
        write_mode = 1;
        filename = argv[2];
        new_comment = argv[3];
    } else {
        printf("Usage: zipnote <zipfile>              (read comment)\n");
        printf("       zipnote -w <zipfile> <comment> (write comment)\n");
        return 1;
    }

    int fd = open(filename, write_mode ? O_RDWR : O_RDONLY, 0);
    if (fd < 0) {
        printf("zipnote: cannot open '%s'\n", filename);
        return 1;
    }

    long size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    if (size < 22) {
        printf("zipnote: file too small to contain EOCD\n");
        close(fd);
        return 1;
    }

    /* Allocate extra space in case new comment is larger */
    char *buf = malloc(size + 65536);
    if (!buf) {
        printf("zipnote: out of memory\n");
        close(fd);
        return 1;
    }

    if (read(fd, buf, size) != size) {
        printf("zipnote: read error\n");
        free(buf);
        close(fd);
        return 1;
    }

    /* Find EOCD record by scanning from the end of file backwards.
       Minimum EOCD size is 22 bytes (no comment). */
    long eocd_off = -1;

    for (long i = size - 22; i >= 0; i--) {
        if (memcmp(buf + i, EOCD_SIG, 4) == 0) {
            eocd_off = i;
            break;
        }
    }

    if (eocd_off < 0) {
        printf("zipnote: cannot find EOCD record\n");
        free(buf);
        close(fd);
        return 1;
    }

    if (!write_mode) {
        /* Read mode: output the comment */
        unsigned short comment_len;
        memcpy(&comment_len, buf + eocd_off + 20, 2);

        if (comment_len > 0) {
            write(1, buf + eocd_off + 22, comment_len);
            write(1, "\n", 1);
        }
    } else {
        /* Write mode: replace the comment */
        unsigned short old_comment_len;
        memcpy(&old_comment_len, buf + eocd_off + 20, 2);

        unsigned long new_len = strlen(new_comment);
        if (new_len > 65535) {
            printf("zipnote: comment too long (max 65535 bytes)\n");
            free(buf);
            close(fd);
            return 1;
        }
        unsigned short new_comment_len = (unsigned short)new_len;

        /* Update comment length in EOCD */
        memcpy(buf + eocd_off + 20, &new_comment_len, 2);
        /* Write the new comment bytes */
        memcpy(buf + eocd_off + 22, new_comment, new_len);

        /* New total file size = EOCD offset + 22 fixed bytes + comment */
        long new_total = eocd_off + 22 + new_len;

        lseek(fd, 0, SEEK_SET);
        write(fd, buf, new_total);
        ftruncate(fd, new_total);
    }

    free(buf);
    close(fd);
    return 0;
}
