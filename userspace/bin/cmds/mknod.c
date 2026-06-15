/* mknod.c — create block/char device node */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: mknod NAME TYPE MAJOR MINOR\n");
        printf("  TYPE: 'b' for block, 'c' for character\n");
        return 1;
    }

    const char *name = argv[1];
    const char *type_s = argv[2];
    int major = atoi(argv[3]);
    int minor = atoi(argv[4]);

    unsigned int mode;
    unsigned long dev;

    if (type_s[0] == 'b') {
        mode = 0060000 | 0644;  /* S_IFBLK | rw-r--r-- */
    } else if (type_s[0] == 'c') {
        mode = 0020000 | 0644;  /* S_IFCHR | rw-r--r-- */
    } else {
        printf("mknod: type must be 'b' (block) or 'c' (character)\n");
        return 1;
    }

    /* Encode major/minor into dev number */
    dev = ((unsigned long)major << 8) | (unsigned long)minor;

    if (mknod(name, mode, dev) < 0) {
        printf("mknod: cannot create '%s' (type %c, major %d, minor %d)\n",
               name, type_s[0], major, minor);
        return 1;
    }

    return 0;
}
