/* dc.c — desk calculator: evaluate postfix expressions (simple integer) */

#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define STACK_MAX 256

static long stack[STACK_MAX];
static int sp = 0;

static void push(long v) {
    if (sp < STACK_MAX) stack[sp++] = v;
}

static long pop(void) {
    if (sp > 0) return stack[--sp];
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: dc <expression>...\n");
        printf("example: dc 3 4 + p\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        const char *tok = argv[i];
        if (strcmp(tok, "p") == 0) {
            if (sp > 0) printf("%ld\n", stack[sp - 1]);
        } else if (strcmp(tok, "f") == 0) {
            for (int j = sp - 1; j >= 0; j--)
                printf("%ld\n", stack[j]);
        } else if (strcmp(tok, "+") == 0) {
            long b = pop(), a = pop();
            push(a + b);
        } else if (strcmp(tok, "-") == 0) {
            long b = pop(), a = pop();
            push(a - b);
        } else if (strcmp(tok, "*") == 0) {
            long b = pop(), a = pop();
            push(a * b);
        } else if (strcmp(tok, "/") == 0) {
            long b = pop(), a = pop();
            if (b == 0) { printf("divide by zero\n"); return 1; }
            push(a / b);
        } else if (strcmp(tok, "%") == 0) {
            long b = pop(), a = pop();
            if (b == 0) { printf("modulo by zero\n"); return 1; }
            push(a % b);
        } else if (strcmp(tok, "d") == 0) {
            if (sp > 0) push(stack[sp - 1]);
        } else if (strcmp(tok, "r") == 0) {
            if (sp >= 2) {
                long a = pop(), b = pop();
                push(a); push(b);
            }
        } else if (strcmp(tok, "c") == 0) {
            sp = 0;
        } else {
            long v = atoi(tok);
            push(v);
        }
    }
    return 0;
}
