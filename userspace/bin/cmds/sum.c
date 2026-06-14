/* sum.c — compute BSD sum (16-bit sum of bytes with rotate) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static unsigned long bsd_sum(int fd, unsigned long *bytes) {
    unsigned short sum = 0;
    *bytes = 0;
    char buf[4096];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            /* Rotate right by 1 bit, then add byte */
            unsigned short carry = sum & 1;
            sum = (sum >> 1) | (carry << 15);
            sum += (unsigned char)buf[i];
            (*bytes)++;
        }
    }
    return sum;
}

int main(int argc, char *argv[]) {
    unsigned long total_bytes = 0;
    unsigned short total_sum = 0;
    int multiple = 0;
    if (argc < 2) {
        unsigned long bytes;
        unsigned short s = bsd_sum(STDIN_FILENO, &bytes);
        printf("%u %lu\n", (unsigned)s, bytes);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) { printf("sum: %s: No such file\n", argv[i]); continue; }
        unsigned long bytes;
        unsigned short s = bsd_sum(fd, &bytes);
        close(fd);
        printf("%u %lu %s\n", (unsigned)s, bytes, argv[i]);
        total_bytes += bytes;
        total_sum += s;
        multiple++;
    }
    if (multiple > 1)
        printf("%u %lu total\n", (unsigned)total_sum, total_bytes);
    return 0;
}
