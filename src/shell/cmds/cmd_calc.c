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
    int64_t val = 0;
    int neg = 0;
    if (*pos == '-') { neg = 1; pos++; skip_spaces(); }

    int is_hex = 0;
    if (*pos == '0' && (*(pos+1) == 'x' || *(pos+1) == 'X')) {
        pos += 2; is_hex = 1;
    }

    if (is_hex) {
        while ((*pos >= '0' && *pos <= '9') ||
               (*pos >= 'a' && *pos <= 'f') ||
               (*pos >= 'A' && *pos <= 'F')) {
            int d;
            if (*pos >= '0' && *pos <= '9') d = *pos - '0';
            else if (*pos >= 'a' && *pos <= 'f') d = *pos - 'a' + 10;
            else d = *pos - 'A' + 10;
            val = val * 16 + d;
            pos++;
        }
    } else {
        while (*pos >= '0' && *pos <= '9') {
            val = val * 10 + (*pos - '0');
            pos++;
        }
    }
    return neg ? -val : val;
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
