/* bc.c — Simple integer calculator */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static int eval(const char **p) {
    int val = 0;
    int sign = 1;
    /* Parse optional leading sign */
    if (**p == '-') { sign = -1; (*p)++; }
    else if (**p == '+') { (*p)++; }
    /* Parse number */
    while (**p >= '0' && **p <= '9') {
        val = val * 10 + (**p - '0');
        (*p)++;
    }
    val *= sign;
    /* Handle operators */
    while (**p) {
        while (**p == ' ') (*p)++;
        char op = **p;
        if (op != '+' && op != '-' && op != '*' && op != '/' && op != '%') break;
        (*p)++;
        while (**p == ' ') (*p)++;
        int rhs_sign = 1;
        if (**p == '-') { rhs_sign = -1; (*p)++; }
        else if (**p == '+') { (*p)++; }
        int rhs = 0;
        while (**p >= '0' && **p <= '9') {
            rhs = rhs * 10 + (**p - '0');
            (*p)++;
        }
        rhs *= rhs_sign;
        if (op == '+') val += rhs;
        else if (op == '-') val -= rhs;
        else if (op == '*') val *= rhs;
        else if (op == '/') { if (rhs != 0) val /= rhs; else val = 0; }
        else if (op == '%') { if (rhs != 0) val %= rhs; else val = 0; }
    }
    return val;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: bc <expression>\n");
        return 1;
    }
    const char *p = argv[1];
    int result = eval(&p);
    printf("%d\n", result);
    return 0;
}
