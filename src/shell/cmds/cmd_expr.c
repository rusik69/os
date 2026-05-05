/* cmd_expr.c — Evaluate simple integer expressions */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static long parse_num(const char **p) {
    long sign = 1;
    if (**p == '-') { sign = -1; (*p)++; }
    long v = 0;
    while (is_digit(**p)) { v = v * 10 + (**p - '0'); (*p)++; }
    return sign * v;
}

void cmd_expr(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: expr <num> <op> <num>\n");
        return;
    }

    const char *p = args;
    while (*p == ' ') p++;

    /* Parse: num op num */
    long a = parse_num(&p);
    while (*p == ' ') p++;

    if (!*p) { kprintf("%d\n", (uint64_t)a); return; }

    char op = *p++;
    /* Handle multi-char ops: !=, >=, <= */
    char op2 = 0;
    if (*p == '=' || *p == '>') { op2 = *p++; }

    while (*p == ' ') p++;
    long b = parse_num(&p);

    long result = 0;
    if (op == '+') result = a + b;
    else if (op == '-') result = a - b;
    else if (op == '*') result = a * b;
    else if (op == '/' && b != 0) result = a / b;
    else if (op == '%' && b != 0) result = a % b;
    else if (op == '<' && op2 == '=') result = (a <= b);
    else if (op == '>' && op2 == '=') result = (a >= b);
    else if (op == '!' && op2 == '=') result = (a != b);
    else if (op == '<') result = (a < b);
    else if (op == '>') result = (a > b);
    else if (op == '=') result = (a == b);
    else if (op == '/' && b == 0) {
        kprintf("expr: division by zero\n");
        return;
    } else if (op == '%' && b == 0) {
        kprintf("expr: division by zero\n");
        return;
    } else {
        kprintf("expr: unknown operator '%c'\n", (uint64_t)(uint8_t)op);
        return;
    }

    kprintf("%d\n", (uint64_t)result);
}
