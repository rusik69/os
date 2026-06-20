/* cmd_factor.c — factor integers into prime factors */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_factor(const char *args) {
    if (!args || !*args) { kprintf("Usage: factor <number> [number...]\n"); return; }

    char buf[128];
    strncpy(buf, args, 127);
    buf[127] = '\0';

    char *p = buf;
    char numstr[16];
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        int i = 0;
        while (*p && *p != ' ' && i < 15) numstr[i++] = *p++;
        numstr[i] = '\0';

        unsigned long n = strtoul(numstr, 0, 10);
        kprintf("%lu:", n);

        if (n <= 1) {
            kprintf(" %lu\n", n);
            continue;
        }

        unsigned long temp = n;
        int first = 1;
        unsigned long d = 2;
        while (d * d <= temp) {
            while (temp % d == 0) {
                if (!first) kprintf(" ");
                kprintf("%lu", d);
                temp /= d;
                first = 0;
            }
            d = (d == 2) ? 3 : d + 2;
        }
        if (temp > 1) {
            if (!first) kprintf(" ");
            kprintf("%lu", temp);
        }
        kprintf("\n");
    }
}
