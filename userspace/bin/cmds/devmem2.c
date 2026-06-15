/* devmem2.c — read/write physical memory (alternative interface) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: devmem2 <address> [type]\n");
        printf("  <address>  physical address in hex (e.g. 0xFEC00000)\n");
        printf("  [type]     access width: byte, word (16-bit), long (32-bit), or dword\n");
        printf("             default: long (32-bit)\n");
        return 1;
    }

    /* Parse address */
    unsigned long addr = 0;
    const char *h = argv[1];
    if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h += 2;
    while (*h) {
        addr = (addr << 4) + (*h >= 'a' ? *h - 'a' + 10 :
                            *h >= 'A' ? *h - 'A' + 10 : *h - '0');
        h++;
    }

    /* Parse type */
    const char *type_str = (argc > 2) ? argv[2] : "long";
    int width;
    if (strcmp(type_str, "byte") == 0) width = 8;
    else if (strcmp(type_str, "word") == 0) width = 16;
    else if (strcmp(type_str, "long") == 0) width = 32;
    else if (strcmp(type_str, "dword") == 0) width = 64;
    else {
        printf("devmem2: unknown type '%s' (use byte, word, long, dword)\n", type_str);
        return 1;
    }

    int fd = open("/dev/mem", O_RDWR, 0);
    if (fd < 0) {
        /* Try read-only */
        fd = open("/dev/mem", O_RDONLY, 0);
        if (fd < 0) {
            printf("devmem2: cannot open /dev/mem\n");
            printf("  (Kernel must have CONFIG_STRICT_DEVMEM disabled or\n");
            printf("   /dev/mem must exist and be accessible)\n");
            return 1;
        }
    }

    /* Seek to address */
    long ret = lseek(fd, addr, SEEK_SET);
    if (ret < 0) {
        printf("devmem2: failed to seek to 0x%lx\n", addr);
        close(fd);
        return 1;
    }

    /* Read value */
    unsigned char buf[8];
    int bytes = width / 8;
    if (bytes > 8) bytes = 8;

    int n = read(fd, buf, bytes);
    if (n < 0) {
        printf("devmem2: failed to read at 0x%lx\n", addr);
        close(fd);
        return 1;
    }

    /* Display value */
    printf("Value at 0x%lx (%d bits): ", addr, width);
    printf("0x");
    for (int i = 0; i < n; i++) {
        printf("%02x", (unsigned char)buf[i]);
    }
    printf("\n");

    close(fd);
    return 0;
}
