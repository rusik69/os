/* xxd.c — hex dump with offset */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    int fd = 0; /* stdin */
    if (argc >= 2) {
        fd = open(argv[1], O_RDONLY, 0);
        if (fd < 0) {
            printf("xxd: cannot open '%s'\n", argv[1]);
            return 1;
        }
    }
    unsigned long offset = 0;
    char buf[16];
    int n;
    while ((n = read(fd, buf, 16)) > 0) {
        printf("%08lx: ", offset);
        for (int i = 0; i < 16; i++) {
            if (i < n) printf("%02x ", (unsigned char)buf[i]);
            else printf("   ");
            if (i == 7) printf(" ");
        }
        printf(" ");
        for (int i = 0; i < n; i++) {
            if (buf[i] >= 32 && buf[i] < 127) putchar(buf[i]);
            else putchar('.');
        }
        printf("\n");
        offset += n;
    }
    if (fd != 0) close(fd);
    return 0;
}
