#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static const char *identify(const char *path) {
    int fd = open(path, 0, 0);
    if (fd < 0) return "cannot open";
    unsigned char buf[64];
    int n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n < 4) return "data";
    /* ELF */
    if (buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F')
        return "ELF";
    /* gzip */
    if (buf[0] == 0x1f && buf[1] == 0x8b) return "gzip compressed data";
    /* new-style cpio */
    if (buf[0] == '0' && buf[1] == '7' && buf[2] == '0' && buf[3] == '7')
        return "cpio archive";
    /* old-style cpio */
    if (buf[0] == 0x71 && buf[1] == 0xc7) return "cpio archive (old)";
    /* bzip2 */
    if (buf[0] == 'B' && buf[1] == 'Z' && buf[2] == 'h') return "bzip2 compressed data";
    /* PNG */
    if (buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G')
        return "PNG image data";
    /* JPEG */
    if (buf[0] == 0xff && buf[1] == 0xd8 && buf[2] == 0xff) return "JPEG image data";
    /* shebang script */
    if (buf[0] == '#' && buf[1] == '!') return "script text executable";
    /* ASCII text */
    int ascii = 1;
    for (int i = 0; i < n; i++) {
        if (buf[i] > 127 && buf[i] != '\n' && buf[i] != '\r' && buf[i] != '\t') {
            ascii = 0;
            break;
        }
    }
    if (ascii) return "ASCII text";
    return "data";
}

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: file <file> [file...]\n"); return 1; }
    for (int i = 1; i < argc; i++)
        printf("%s: %s\n", argv[i], identify(argv[i]));
    return 0;
}
