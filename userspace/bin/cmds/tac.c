/* tac.c — reverse cat (read file, write lines in reverse order) */
#include "unistd.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    int fd = 0; /* stdin */
    if (argc >= 2) {
        fd = open(argv[1], O_RDONLY, 0);
        if (fd < 0) {
            const char *msg = "tac: cannot open ";
            write(1, msg, strlen(msg));
            write(1, argv[1], strlen(argv[1]));
            write(1, "\n", 1);
            return 1;
        }
    }
    char *data = 0;
    unsigned long total = 0;
    char buf[4096];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        char *newdata = malloc(total + n + 1);
        if (total > 0 && data) {
            memcpy(newdata, data, total);
            free(data);
        }
        memcpy(newdata + total, buf, n);
        data = newdata;
        total += n;
        data[total] = 0;
    }
    if (fd != 0) close(fd);
    if (!data) return 0;
    /* Print lines in reverse order */
    char *end = data + total;
    char *line_end = end;
    while (line_end > data) {
        /* Find previous newline */
        char *nl = 0;
        char *p = line_end - 1;
        while (p >= data) {
            if (*p == '\n') { nl = p; break; }
            p--;
        }
        if (nl) {
            write(1, nl + 1, line_end - nl - 1);
            write(1, "\n", 1);
            line_end = nl;
        } else {
            write(1, data, line_end - data);
            write(1, "\n", 1);
            break;
        }
    }
    free(data);
    return 0;
}
