/* cmd_dc.c — RPN desktop calculator */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

#define STACK_SIZE 64

static int64_t stack[STACK_SIZE];
static int sp = 0;

static int push(int64_t v) {
    if (sp >= STACK_SIZE) { kprintf("dc: stack full\n"); return -1; }
    stack[sp++] = v;
    return 0;
}

static int64_t pop(void) {
    if (sp <= 0) { kprintf("dc: stack empty\n"); return 0; }
    return stack[--sp];
}

int cmd_dc(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: dc <expression> [expression...]\n");
        return 1;
    }

    sp = 0;
    for (int i = 1; i < argc; i++) {
        const char *tok = argv[i];
        if (isdigit(tok[0]) || (tok[0] == '-' && tok[1] != '\0')) {
            push(atol(tok));
        } else if (strcmp(tok, "+") == 0) {
            int64_t b = pop(), a = pop();
            push(a + b);
        } else if (strcmp(tok, "-") == 0) {
            int64_t b = pop(), a = pop();
            push(a - b);
        } else if (strcmp(tok, "*") == 0) {
            int64_t b = pop(), a = pop();
            push(a * b);
        } else if (strcmp(tok, "/") == 0) {
            int64_t b = pop(), a = pop();
            if (b == 0) { kprintf("dc: division by zero\n"); return 1; }
            push(a / b);
        } else if (strcmp(tok, "%") == 0) {
            int64_t b = pop(), a = pop();
            if (b == 0) { kprintf("dc: modulo by zero\n"); return 1; }
            push(a % b);
        } else if (strcmp(tok, "~") == 0) {
            int64_t b = pop(), a = pop();
            if (b == 0) { kprintf("dc: division by zero\n"); return 1; }
            push(a / b);
            push(a % b);
        } else if (strcmp(tok, "^") == 0) {
            int64_t b = pop(), a = pop();
            int64_t r = 1;
            for (int64_t j = 0; j < b; j++) r *= a;
            push(r);
        } else if (strcmp(tok, "v") == 0) {
            int64_t a = pop();
            if (a < 0) { kprintf("dc: sqrt of negative\n"); return 1; }
            int64_t r = 0;
            while (r * r <= a) r++;
            push(r - 1);
        } else if (strcmp(tok, "p") == 0) {
            kprintf("%lld\n", stack[sp - 1]);
        } else if (strcmp(tok, "P") == 0) {
            if (sp > 0) kprintf("%lld\n", pop());
        } else if (strcmp(tok, "f") == 0) {
            for (int j = sp - 1; j >= 0; j--)
                kprintf("%lld\n", stack[j]);
        } else if (strcmp(tok, "c") == 0) {
            sp = 0;
        } else if (strcmp(tok, "d") == 0) {
            if (sp > 0) push(stack[sp - 1]);
        } else if (strcmp(tok, "r") == 0) {
            if (sp >= 2) {
                int64_t tmp = stack[sp - 1];
                stack[sp - 1] = stack[sp - 2];
                stack[sp - 2] = tmp;
            }
        } else {
            kprintf("dc: unknown operation '%s'\n", tok);
            return 1;
        }
    }
    return 0;
}
