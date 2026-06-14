/* more.c — Simple pager: print 24 lines, wait for key */
#include "unistd.h"
#include "stdio.h"

#define LINES_PER_PAGE 24

int main(void) {
    char buf[512];
    int line = 0;
    int n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            write(1, buf + i, 1);
            if (buf[i] == '\n') {
                line++;
                if (line >= LINES_PER_PAGE) {
                    printf("--More--");
                    char ch;
                    read(0, &ch, 1);
                    printf("\r        \r");
                    line = 0;
                }
            }
        }
    }
    return 0;
}
