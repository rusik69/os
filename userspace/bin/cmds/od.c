#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static void print_oct_offset(unsigned long val, int digits) {
    char buf[16];
    int pos = digits;
    buf[pos] = '\0';
    while (pos > 0) {
        buf[--pos] = '0' + (val & 7);
        val >>= 3;
    }
    printf("%s", buf);
}

static void print_oct_byte(unsigned char b) {
    char buf[4];
    buf[3] = '\0';
    buf[2] = '0' + (b & 7); b >>= 3;
    buf[1] = '0' + (b & 7); b >>= 3;
    buf[0] = '0' + (b & 7);
    printf("%s", buf);
}

int main(int argc, char *argv[]) {
    const char *path = argc > 1 ? argv[1] : "/dev/stdin";
    int fd = open(path, 0, 0);
    if (fd < 0) { printf("od: %s: No such file\n", path); return 1; }
    unsigned char buf[16];
    unsigned long offset = 0;
    int n;
    while ((n = read(fd, buf, 16)) > 0) {
        print_oct_offset(offset, 7);
        for (int i = 0; i < n; i++) {
            printf(" ");
            print_oct_byte(buf[i]);
        }
        printf("\n");
        offset += n;
    }
    if (offset > 0) {
        print_oct_offset(offset, 7);
        printf("\n");
    }
    close(fd);
    return 0;
}
