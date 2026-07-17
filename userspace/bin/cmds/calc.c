/* calc.c — simple integer calculator */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("usage: calc <a> <op> <b>\n");
        printf("ops: + - * / %%\n");
        return 1;
    }
    int a = atoi(argv[1]);
    int b = atoi(argv[3]);
    char op = argv[2][0];
    int result = 0;
    switch (op) {
        case '+': result = a + b; break;
        case '-': result = a - b; break;
        case '*': result = a * b; break;
        case '/':
            if (b == 0) { printf("division by zero\n"); return 1; }
            result = a / b;
            break;
        case '%':
            if (b == 0) { printf("division by zero\n"); return 1; }
            result = a % b;
            break;
        default:
            printf("unknown operator: %c\n", op);
            return 1;
    }
    printf("%d\n", result);
    return 0;
}
