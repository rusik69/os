/* colrm.c — remove columns from stdin */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    int start = 0, end = 0;
    if (argc > 1) start = atoi(argv[1]);
    if (argc > 2) end = atoi(argv[2]);
    if (start < 1) start = 1;
    if (end < start) end = 0;

    char buf[4096];
    long n;
    while ((n = read(0, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        /* Process line by line */
        char line[4096];
        int li = 0;
        for (long i = 0; i <= n; i++) {
            if (buf[i] == '\n' || i == n) {
                line[li] = '\0';
                /* Remove columns if end > 0 */
                if (end > 0) {
                    unsigned long len = strlen(line);
                    char out[4096];
                    int oi = 0;
                    for (unsigned long j = 0; j < len && j < (unsigned long)start - 1; j++)
                        out[oi++] = line[j];
                    for (unsigned long j = (unsigned long)end; j < len; j++)
                        out[oi++] = line[j];
                    out[oi] = '\0';
                    write(1, out, oi);
                } else {
                    write(1, line, li);
                }
                write(1, "\n", 1);
                li = 0;
            } else {
                line[li++] = buf[i];
            }
        }
    }
    return 0;
}
