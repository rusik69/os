/* cmd_expr.c — Evaluate simple integer expressions */

#include "printf.h"
#include "shell_cmds.h"
#include "string.h"

static long parse_num(const char **p) {
    char *end = NULL;
    long v = strtol(*p, &end, 10);
    *p = end;
    return v;
}

void cmd_expr(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: expr <num> <op> <num>\n");
        return;
    }

    const char *p = args;
    while (*p == ' ')
        p++;

    /* Parse: num op num */
    long a = parse_num(&p);
    while (*p == ' ')
        p++;

    if (!*p) {
        kprintf("%d\n", (int)a);
        return;
    }

    char op = *p++;
    /* Handle multi-char ops: !=, >=, <= */
    char op2 = 0;
    if (*p == '=' || *p == '>') {
        op2 = *p++;
    }

    while (*p == ' ')
        p++;
    long b = parse_num(&p);

    long result = 0;
    if (op == '+')
        result = a + b;
    else if (op == '-')
        result = a - b;
    else if (op == '*')
        result = a * b;
    else if (op == '/' && b == 0) {
        kprintf("expr: division by zero\n");
        return;
    } else if (op == '%' && b == 0) {
        kprintf("expr: division by zero\n");
        return;
    } else if (op == '/')
        result = a / b;
    else if (op == '%')
        result = a % b;
    else if (op == '<' && op2 == '=')
        result = (a <= b);
    else if (op == '>' && op2 == '=')
        result = (a >= b);
    else if (op == '!' && op2 == '=')
        result = (a != b);
    else if (op == '<')
        result = (a < b);
    else if (op == '>')
        result = (a > b);
    else if (op == '=')
        result = (a == b);
    else {
        kprintf("expr: unknown operator '%c'\n", (int)(uint8_t)op);
        return;
    }

    kprintf("%d\n", (int)result);
}
