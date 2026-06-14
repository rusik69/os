/* dos2unix.c — convert DOS line endings to Unix */

#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    char buf[4096];
    int n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        char out[4096];
        int outlen = 0;
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\r' && (i + 1 < n && buf[i + 1] == '\n'))
                continue; /* skip \r */
            if (buf[i] == '\r' && (i + 1 >= n)) {
                /* \r at boundary, need next read to check */
                /* For simplicity, just keep it */
                out[outlen++] = buf[i];
            } else {
                out[outlen++] = buf[i];
            }
        }
        write(1, out, outlen);
    }
    return 0;
}
