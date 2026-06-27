/* units.c — simple unit converter */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    char buf[256];
    printf("Unit Converter\n  c2f <C> | f2c <F> | m2f <m> | f2m <ft>\nType 'quit' to exit\n");
    while (1) {
        printf("> ");
        int n = read(0, buf, 255);
        if (n <= 0) break;
        buf[n - 1] = 0;
        if (strcmp(buf, "quit") == 0 || strlen(buf) == 0) break;
        char cmd[16];
        int i = 0;
        while (buf[i] && buf[i] != ' ') { cmd[i] = buf[i]; i++; }
        cmd[i] = 0;
        while (buf[i] == ' ') i++;
        int val = 0, neg = 0;
        if (buf[i] == '-') { neg = 1; i++; }
        while (buf[i] >= '0' && buf[i] <= '9') { val = val * 10 + (buf[i] - '0'); i++; }
        if (neg) val = -val;
        if (strcmp(cmd, "c2f") == 0) printf("%dC = %dF\n", val, val * 9 / 5 + 32);
        else if (strcmp(cmd, "f2c") == 0) printf("%dF = %dC\n", val, (val - 32) * 5 / 9);
        else if (strcmp(cmd, "m2f") == 0) printf("%dm = %dft\n", val, val * 328 / 100);
        else if (strcmp(cmd, "f2m") == 0) printf("%dft = %dm\n", val, val * 100 / 328);
        else printf("unknown cmd '%s'\n", cmd);
    }
    return 0;
}
