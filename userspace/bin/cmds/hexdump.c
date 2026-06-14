#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define BUF_SIZE 4096

static void print_hex_offset(unsigned long val, int digits) {
    char buf[17];
    for (int i = digits - 1; i >= 0; i--) {
        int nib = val & 0xF;
        buf[i] = nib < 10 ? '0' + nib : 'a' + nib - 10;
        val >>= 4;
    }
    buf[digits] = '\0';
    printf("%s", buf);
}

static void print_hex_byte(unsigned char b) {
    char buf[3];
    int hi = (b >> 4) & 0xF;
    int lo = b & 0xF;
    buf[0] = hi < 10 ? '0' + hi : 'a' + hi - 10;
    buf[1] = lo < 10 ? '0' + lo : 'a' + lo - 10;
    buf[2] = '\0';
    printf("%s", buf);
}

void hexdump(const char *path) {
    int fd = open(path, 0, 0);
    if (fd < 0) { printf("hexdump: cannot open %s\n", path); return; }
    unsigned char buf[BUF_SIZE];
    unsigned long offset = 0;
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i += 16) {
            print_hex_offset(offset + i, 8);
            printf("  ");
            for (int j = 0; j < 16; j++) {
                if (i + j < n) {
                    print_hex_byte(buf[i + j]);
                    printf(" ");
                } else {
                    printf("   ");
                }
                if (j == 7) printf(" ");
            }
            printf(" |");
            for (int j = 0; j < 16 && i + j < n; j++)
                printf("%c", buf[i+j] >= 32 && buf[i+j] < 127 ? buf[i+j] : '.');
            printf("|\n");
        }
        offset += n;
    }
    close(fd);
}

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: hexdump <file>\n"); return 1; }
    for (int i = 1; i < argc; i++) hexdump(argv[i]);
    return 0;
}
