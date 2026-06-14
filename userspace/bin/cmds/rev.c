/* rev.c — Reverse lines from stdin */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(void) {
    char buf[1024];
    int off = 0;
    int n;
    while ((n = read(0, buf + off, (int)sizeof(buf) - 1 - off)) > 0) {
        off += n;
        buf[off] = 0;
        char *line = buf;
        while (1) {
            char *nl = strchr(line, '\n');
            if (!nl) {
                int remaining = buf + off - line;
                if (remaining > 0 && line != buf) {
                    memcpy(buf, line, remaining);
                    off = remaining;
                }
                break;
            }
            *nl = 0;
            /* Reverse the line */
            int len = strlen(line);
            for (int i = len - 1; i >= 0; i--)
                write(1, line + i, 1);
            write(1, "\n", 1);
            line = nl + 1;
        }
    }
    return 0;
}
