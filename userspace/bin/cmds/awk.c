/* awk.c — Simple AWK-like field processor */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    /* If no args, just print $1..$NF for each line */
    char buf[1024];
    int fd = 0;
    if (argc >= 2 && argv[1][0] != '{') {
        fd = open(argv[1], O_RDONLY, 0);
        if (fd < 0) {
            printf("awk: cannot open '%s'\n", argv[1]);
            return 1;
        }
    }
    int n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = 0;
        char *line = buf;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            /* Print fields, space-separated */
            char *fields[256];
            int nf = 0;
            char *p = line;
            int in_field = 0;
            while (*p) {
                if (*p == ' ' || *p == '\t') {
                    if (in_field) { in_field = 0; }
                } else {
                    if (!in_field) {
                        fields[nf++] = p;
                        in_field = 1;
                    }
                }
                p++;
            }
            for (int i = 0; i < nf; i++) {
                if (i > 0) write(1, " ", 1);
                /* Print field until space */
                char *f = fields[i];
                while (*f && *f != ' ' && *f != '\t') {
                    write(1, f, 1);
                    f++;
                }
            }
            write(1, "\n", 1);
            line = nl ? nl + 1 : 0;
        }
    }
    if (fd != 0) close(fd);
    return 0;
}
