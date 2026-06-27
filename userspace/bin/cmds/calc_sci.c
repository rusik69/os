/* calc_sci.c — simple expression calculator */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int next_int(const char *s, int *i) {
    int val = 0, neg = 0;
    while (s[*i] == ' ') (*i)++;
    if (s[*i] == '-') { neg = 1; (*i)++; }
    while (s[*i] >= '0' && s[*i] <= '9') { val = val * 10 + (s[*i] - '0'); (*i)++; }
    return neg ? -val : val;
}

int main(void) {
    char buf[256];
    printf("Calculator (+ - * / %%)\nUsage: 5 + 3\nType 'quit' to exit\n");
    while (1) {
        printf("> ");
        int n = read(0, buf, 255);
        if (n <= 0) break;
        buf[n - 1] = 0;
        if (strcmp(buf, "quit") == 0 || strlen(buf) == 0) break;
        int i = 0, a = next_int(buf, &i);
        while (buf[i] == ' ') i++;
        if (!buf[i]) { printf("Need operator\n"); continue; }
        char op = buf[i]; i++;
        int b = next_int(buf, &i);
        switch (op) {
            case '+': printf("= %d\n", a + b); break;
            case '-': printf("= %d\n", a - b); break;
            case '*': printf("= %d\n", a * b); break;
            case '/': if (b) printf("= %d\n", a / b); else printf("div 0\n"); break;
            case '%': if (b) printf("= %d\n", a % b); else printf("div 0\n"); break;
            default: printf("unknown op '%c'\n", op);
        }
    }
    return 0;
}
