/* uuencode.c — encode file to uuencode format */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    const char *name = "data";
    int fd = 0; /* stdin */
    if (argc >= 2) {
        name = argv[1];
        fd = open(argv[1], O_RDONLY, 0);
        if (fd < 0) {
            printf("uuencode: cannot open '%s'\n", argv[1]);
            return 1;
        }
    }
    if (argc >= 3) name = argv[2];
    printf("begin 644 %s\n", name);
    char buf[45];
    int n;
    while ((n = read(fd, buf, 45)) > 0) {
        /* Encode line length char */
        int c = n;
        putchar((c & 0x3f) + 32);
        /* Encode data */
        for (int i = 0; i < n; i += 3) {
            int b1 = buf[i] & 0xff;
            int b2 = (i + 1 < n) ? buf[i + 1] & 0xff : 0;
            int b3 = (i + 2 < n) ? buf[i + 2] & 0xff : 0;
            putchar(((b1 >> 2) & 0x3f) + 32);
            putchar((((b1 << 4) | (b2 >> 4)) & 0x3f) + 32);
            if (i + 1 < n)
                putchar((((b2 << 2) | (b3 >> 6)) & 0x3f) + 32);
            else
                putchar(96); /* ` padding */
            if (i + 2 < n)
                putchar((b3 & 0x3f) + 32);
            else
                putchar(96);
        }
        putchar('\n');
    }
    if (fd != 0) close(fd);
    printf("end\n");
    return 0;
}
