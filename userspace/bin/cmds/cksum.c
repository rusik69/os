/* cksum.c — simplified POSIX cksum: 32-bit CRC */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* Simple CRC-32 implementation */
static unsigned int crc32_table[256];
static int table_init = 0;

static void init_crc32(void) {
    for (unsigned int i = 0; i < 256; i++) {
        unsigned int crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
        crc32_table[i] = crc;
    }
    table_init = 1;
}

static unsigned int compute_crc(int fd, unsigned long *size) {
    if (!table_init) init_crc32();
    unsigned int crc = 0xFFFFFFFF;
    *size = 0;
    char buf[4096];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            crc = crc32_table[(crc ^ (unsigned char)buf[i]) & 0xFF] ^ (crc >> 8);
            (*size)++;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        unsigned long size;
        unsigned int crc = compute_crc(STDIN_FILENO, &size);
        printf("%u %lu\n", crc, size);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) { printf("cksum: %s: No such file\n", argv[i]); continue; }
        unsigned long size;
        unsigned int crc = compute_crc(fd, &size);
        close(fd);
        printf("%u %lu %s\n", crc, size, argv[i]);
    }
    return 0;
}
