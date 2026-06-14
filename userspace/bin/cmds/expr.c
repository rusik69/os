/* expr.c — evaluate expression */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("usage: expr <a> <op> <b>\n");
        printf("supported ops: + - * / %%\n");
        return 1;
    }
    int a = atoi(argv[1]);
    int b = atoi(argv[3]);
    const char *op = argv[2];
    int result = 0;
    if (strcmp(op, "+") == 0) result = a + b;
    else if (strcmp(op, "-") == 0) result = a - b;
    else if (strcmp(op, "*") == 0) result = a * b;
    else if (strcmp(op, "/") == 0) {
        if (b == 0) { printf("division by zero\n"); return 1; }
        result = a / b;
    } else if (strcmp(op, "%") == 0) {
        if (b == 0) { printf("division by zero\n"); return 1; }
        result = a % b;
    } else {
        printf("expr: unknown operator %s\n", op);
        return 1;
    }
    printf("%d\n", result);
    return 0;
}
