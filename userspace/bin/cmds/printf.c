#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: printf <format> [arg...]\n"); return 1; }
    const char *fmt = argv[1];
    char out[4096];
    char buf[64];
    int oi = 0;
    for (int i = 0; fmt[i] && oi < (int)sizeof(out) - 1; i++) {
        if (fmt[i] == '%' && fmt[i+1] == 's') {
            int ai = (argc > 2) ? 2 : 1;
            if (ai < argc) {
                const char *s = argv[ai];
                while (*s && oi < (int)sizeof(out) - 1) out[oi++] = *s++;
                if (ai > 1) argv[ai] = "";
            }
            i++;
        } else if (fmt[i] == '%' && fmt[i+1] == 'd') {
            int ai = (argc > 2) ? 2 : 1;
            if (ai < argc) {
                int v = atoi(argv[ai]);
                int len = snprintf(buf, sizeof(buf), "%d", v);
                for (int j = 0; j < len && oi < (int)sizeof(out) - 1; j++)
                    out[oi++] = buf[j];
                if (ai > 1) argv[ai] = "";
            }
            i++;
        } else if (fmt[i] == '\\' && fmt[i+1] == 'n') {
            out[oi++] = '\n';
            i++;
        } else if (fmt[i] == '\\' && fmt[i+1] == 't') {
            out[oi++] = '\t';
            i++;
        } else {
            out[oi++] = fmt[i];
        }
    }
    out[oi] = '\0';
    write(1, out, oi);
    return 0;
}
