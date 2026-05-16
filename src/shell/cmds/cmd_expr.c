/* cmd_expr.c — Evaluate simple integer expressions */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

static long parse_num(const char **p) {
    return strtol(*p, (char **)p, 10);
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

    if (!*p) { kprintf("%ld\n", (uint64_t)a); return; }

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
    else if (op == '/' && b == 0) {
        kprintf("expr: division by zero\n");
        return;
    } else if (op == '%' && b == 0) {
        kprintf("expr: division by zero\n");
        return;
    } else if (op == '/') result = a / b;
    else if (op == '%') result = a % b;
    else if (op == '<' && op2 == '=') result = (a <= b);
    else if (op == '>' && op2 == '=') result = (a >= b);
    else if (op == '!' && op2 == '=') result = (a != b);
    else if (op == '<') result = (a < b);
    else if (op == '>') result = (a > b);
    else if (op == '=') result = (a == b);
    else {
        kprintf("expr: unknown operator '%c'\n", (uint64_t)(uint8_t)op);
        return;
    }

    kprintf("%ld\n", (uint64_t)result);
}
