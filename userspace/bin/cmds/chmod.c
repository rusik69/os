/* chmod.c — change file permissions */

#include "unistd.h"
#include "stdio.h"
#include "string.h"

static unsigned int parse_mode(const char *s) {
    unsigned int m = 0;
    unsigned long len = strlen(s);
    for (unsigned long i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '7') return 0;
        m = (m << 3) | (s[i] - '0');
    }
    return m;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: chmod <mode> <file>\n");
        return 1;
    }
    unsigned int mode = parse_mode(argv[1]);
    if (mode == 0) {
        printf("chmod: invalid mode '%s'\n", argv[1]);
        return 1;
    }
    if (chmod(argv[2], mode) < 0) {
        printf("chmod: failed to change mode of '%s'\n", argv[2]);
        return 1;
    }
    return 0;
}
