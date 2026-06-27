/* hexcalc.c — hex calculator */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static unsigned int parse_hex(const char *s, int *i) {
    unsigned int val = 0;
    while (s[*i] == ' ') (*i)++;
    while (hex_val(s[*i]) >= 0) { val = val * 16 + hex_val(s[*i]); (*i)++; }
    return val;
}

int main(void) {
    char buf[256];
    printf("Hex Calculator\nUsage: FF + 1\nOps: + - * / & | ^\n");
    while (1) {
        printf("> ");
        int n = read(0, buf, 255);
        if (n <= 0) break;
        buf[n - 1] = 0;
        if (strcmp(buf, "quit") == 0 || strlen(buf) == 0) break;
        int i = 0;
        unsigned int a = parse_hex(buf, &i);
        while (buf[i] == ' ') i++;
        if (!buf[i]) continue;
        char op = buf[i]; i++;
        unsigned int b = parse_hex(buf, &i);
        switch (op) {
            case '+': printf("= 0x%X\n", a + b); break;
            case '-': printf("= 0x%X\n", a - b); break;
            case '*': printf("= 0x%X\n", a * b); break;
            case '/': if (b) printf("= 0x%X\n", a / b); else printf("div 0\n"); break;
            case '&': printf("= 0x%X\n", a & b); break;
            case '|': printf("= 0x%X\n", a | b); break;
            case '^': printf("= 0x%X\n", a ^ b); break;
            default: printf("unknown op\n");
        }
    }
    return 0;
}
