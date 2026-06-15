/* zipcloak.c — toggle encryption bit in zip local file headers */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

/* Local file header signature in little-endian: PK\x03\x04 */
static const unsigned char LFH_SIG[4] = {0x50, 0x4b, 0x03, 0x04};

int main(int argc, char *argv[]) {
    int decrypt = 0;
    const char *filename;

    if (argc == 2) {
        decrypt = 0;          /* encrypt: set bit 0 */
        filename = argv[1];
    } else if (argc == 3 && strcmp(argv[1], "-d") == 0) {
        decrypt = 1;          /* decrypt: clear bit 0 */
        filename = argv[2];
    } else {
        printf("Usage: zipcloak <zipfile>   (encrypt)\n");
        printf("       zipcloak -d <zipfile> (decrypt)\n");
        return 1;
    }

    int fd = open(filename, O_RDWR, 0);
    if (fd < 0) {
        printf("zipcloak: cannot open '%s'\n", filename);
        return 1;
    }

    long size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    if (size < 30) {
        printf("zipcloak: file too small\n");
        close(fd);
        return 1;
    }

    char *buf = malloc(size);
    if (!buf) {
        printf("zipcloak: out of memory\n");
        close(fd);
        return 1;
    }

    if (read(fd, buf, size) != size) {
        printf("zipcloak: read error\n");
        free(buf);
        close(fd);
        return 1;
    }

    int found = 0;
    long i = 0;

    while (i <= size - 30) {
        if (memcmp(buf + i, LFH_SIG, 4) == 0) {
            /* General purpose bit flag is at offset 6 from the header start */
            unsigned short flags;
            memcpy(&flags, buf + i + 6, 2);

            if (decrypt)
                flags &= ~0x0001;   /* clear encryption bit */
            else
                flags |= 0x0001;    /* set encryption bit */

            memcpy(buf + i + 6, &flags, 2);

            /* Skip past filename and extra field to find next header */
            unsigned short name_len, extra_len;
            memcpy(&name_len, buf + i + 26, 2);
            memcpy(&extra_len, buf + i + 28, 2);
            i += 30 + name_len + extra_len;
            found = 1;
        } else {
            i++;
        }
    }

    if (!found) {
        printf("zipcloak: no local file headers found\n");
        free(buf);
        close(fd);
        return 1;
    }

    lseek(fd, 0, SEEK_SET);
    write(fd, buf, size);

    free(buf);
    close(fd);

    printf("zipcloak: %s '%s'\n", decrypt ? "decrypted" : "encrypted", filename);
    return 0;
}
