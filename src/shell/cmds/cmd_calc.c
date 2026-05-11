/* cmd_calc.c — Simple arithmetic calculator */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

/*
 * Recursive descent parser for expressions:
 *   expr   = term (('+' | '-') term)*
 *   term   = factor (('*' | '/' | '%') factor)*
 *   factor = '-' factor | '(' expr ')' | number
 */

static const char *pos;

static void skip_spaces(void) { while (*pos == ' ') pos++; }

static int64_t parse_expr(void);

static int64_t parse_number(void) {
    skip_spaces();
    char *end;
    int64_t val = (int64_t)strtol(pos, &end, 0);
    pos = end;
    return val;
}

static int64_t parse_factor(void) {
    skip_spaces();
    if (*pos == '(') {
        pos++;
        int64_t val = parse_expr();
        skip_spaces();
        if (*pos == ')') pos++;
        return val;
    }
    if (*pos == '-') {
        pos++;
        return -parse_factor();
    }
    if (*pos == '~') {
        pos++;
        return ~parse_factor();
    }
    return parse_number();
}

static int64_t parse_term(void) {
    int64_t left = parse_factor();
    for (;;) {
        skip_spaces();
        if (*pos == '*') { pos++; left *= parse_factor(); }
        else if (*pos == '/') {
            pos++;
            int64_t r = parse_factor();
            if (r == 0) { kprintf("calc: division by zero\n"); return 0; }
            left /= r;
        }
        else if (*pos == '%') {
            pos++;
            int64_t r = parse_factor();
            if (r == 0) { kprintf("calc: modulo by zero\n"); return 0; }
            left %= r;
        }
        else break;
    }
    return left;
}

static int64_t parse_bitshift(void) {
    int64_t left = parse_term();
    for (;;) {
        skip_spaces();
        if (*pos == '<' && *(pos+1) == '<') { pos += 2; left <<= parse_term(); }
        else if (*pos == '>' && *(pos+1) == '>') { pos += 2; left >>= parse_term(); }
        else break;
    }
    return left;
}

static int64_t parse_bitand(void) {
    int64_t left = parse_bitshift();
    for (;;) {
        skip_spaces();
        if (*pos == '&') { pos++; left &= parse_bitshift(); }
        else break;
    }
    return left;
}

static int64_t parse_bitxor(void) {
    int64_t left = parse_bitand();
    for (;;) {
        skip_spaces();
        if (*pos == '^') { pos++; left ^= parse_bitand(); }
        else break;
    }
    return left;
}

static int64_t parse_bitor(void) {
    int64_t left = parse_bitxor();
    for (;;) {
        skip_spaces();
        if (*pos == '|') { pos++; left |= parse_bitxor(); }
        else break;
    }
    return left;
}

static int64_t parse_expr(void) {
    int64_t left = parse_bitor();
    for (;;) {
        skip_spaces();
        if (*pos == '+') { pos++; left += parse_bitor(); }
        else if (*pos == '-') { pos++; left -= parse_bitor(); }
        else break;
    }
    return left;
}

void cmd_calc(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: calc <expression>\n");
        kprintf("  Supports: + - * / %% & | ^ ~ << >> ()\n");
        kprintf("  Hex: 0xFF  Example: calc (10 + 5) * 3\n");
        return;
    }
    pos = args;
    int64_t result = parse_expr();
    kprintf("%d\n", (uint64_t)result);
    /* Also show hex if large */
    if (result > 255 || result < -255)
        kprintf("  = 0x%x\n", (uint64_t)result);
}
