/* uudecode.c — decode uuencoded file */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    int fd = 0; /* stdin */
    if (argc >= 2) {
        fd = open(argv[1], O_RDONLY, 0);
        if (fd < 0) {
            printf("uudecode: cannot open '%s'\n", argv[1]);
            return 1;
        }
    }
    char *data = 0;
    unsigned long total = 0;
    char buf[512];
    int n;
    int found_begin = 0;
    char outname[256] = {0};
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = 0;
        char *p = buf;
        while (*p) {
            /* Look for 'begin' line */
            if (!found_begin) {
                if (strncmp(p, "begin ", 6) == 0) {
                    found_begin = 1;
                    /* Skip mode */
                    char *sp = p + 6;
                    while (*sp && *sp != ' ') sp++;
                    if (*sp == ' ') {
                        char *namep = sp + 1;
                        int i = 0;
                        while (*namep && *namep != '\n' && i < 255)
                            outname[i++] = *namep++;
                        outname[i] = 0;
                    }
                    /* Skip rest of line */
                    while (*p && *p != '\n') p++;
                    if (*p == '\n') p++;
                    continue;
                }
                /* Skip line */
                while (*p && *p != '\n') p++;
                if (*p == '\n') p++;
                continue;
            }
            /* Check for 'end' */
            if (strncmp(p, "end", 3) == 0) break;
            /* Decode line */
            int len = (*p) - 32;
            p++;
            char dec[60];
            int di = 0;
            while (*p && *p != '\n' && *p != '\r') {
                char chars[4];
                int got = 0;
                while (got < 4 && *p && *p != '\n' && *p != '\r') {
                    chars[got++] = *p++;
                }
                if (got < 4) break;
                for (int i = 0; i < got; i++) chars[i] = (chars[i] - 32) & 0x3f;
                dec[di++] = (chars[0] << 2) | (chars[1] >> 4);
                if (di < len) dec[di++] = (chars[1] << 4) | (chars[2] >> 2);
                if (di < len) dec[di++] = (chars[2] << 6) | chars[3];
            }
            if (*p == '\n') p++;
            /* Append decoded data */
            char *newdata = malloc(total + di);
            if (!newdata) { free(data); if (fd != 0) close(fd); return 1; }
            if (total > 0 && data) {
                memcpy(newdata, data, total);
                free(data);
            }
            memcpy(newdata + total, dec, di);
            data = newdata;
            total += di;
        }
        if (strncmp(p, "end", 3) == 0) break;
    }
    if (fd != 0) close(fd);
    if (found_begin && total > 0) {
        /* Write output */
        if (outname[0]) {
            int outfd = open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (outfd >= 0) {
                write(outfd, data, total);
                close(outfd);
            } else {
                write(1, data, total);
            }
        } else {
            write(1, data, total);
        }
    }
    if (data) free(data);
    return 0;
}
