/* cmd_bc.c — Basic calculator (bc-style) with variables and functions
 *
 * Implements a simple desktop calculator supporting:
 *   - Integer arithmetic: +, -, *, /, %, ^ (exponentiation)
 *   - Bitwise: &, |, ^, ~, <<, >>
 *   - Variable assignment: x = 5
 *   - Interactive or expression mode
 *   - Hex output with 0x prefix on large results
 *
 * Usage:
 *   bc <expression>          — evaluate expression and print result
 *   bc (interactive mode)    — read-eval-print loop (future)
 */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"

/* Recursive descent parser state */
static const char *bc_pos;
static int64_t bc_vars[26]; /* variables a-z */
static int bc_var_init[26]; /* whether each variable has been assigned */

static void bc_skip_spaces(void) {
    while (*bc_pos == ' ' || *bc_pos == '\t') bc_pos++;
}

/* Forward declarations */
static int64_t bc_parse_expr(void);

/* Parse a number or variable reference */
static int64_t bc_parse_primary(void) {
    bc_skip_spaces();
    if (*bc_pos == '(') {
        bc_pos++;
        int64_t val = bc_parse_expr();
        bc_skip_spaces();
        if (*bc_pos == ')') bc_pos++;
        return val;
    }
    if (*bc_pos == '-') {
        bc_pos++;
        return -bc_parse_primary();
    }
    if (*bc_pos == '+') {
        bc_pos++;
        return bc_parse_primary();
    }
    if (*bc_pos == '~') {
        bc_pos++;
        return ~bc_parse_primary();
    }
    /* Variable reference or assignment */
    if (*bc_pos >= 'a' && *bc_pos <= 'z') {
        int idx = *bc_pos - 'a';
        bc_pos++;

        /* Check for assignment: var = expr */
        bc_skip_spaces();
        if (*bc_pos == '=') {
            bc_pos++;
            int64_t val = bc_parse_expr();
            bc_vars[idx] = val;
            bc_var_init[idx] = 1;
            return val;
        }

        if (!bc_var_init[idx]) {
            kprintf("bc: variable '%c' is uninitialized\n", 'a' + idx);
            return 0;
        }
        return bc_vars[idx];
    }
    /* Number: strtol handles 0x prefix, decimal, octal */
    char *end;
    int64_t val = (int64_t)strtol(bc_pos, &end, 0);
    bc_pos = end;
    return val;
}

/* Parse exponentiation (right-associative) */
static int64_t bc_parse_power(void) {
    int64_t left = bc_parse_primary();
    bc_skip_spaces();
    if (*bc_pos == '^') {
        bc_pos++;
        int64_t exp = bc_parse_power(); /* right-associative */
        int64_t result = 1;
        if (exp < 0) {
            /* Negative exponent: compute reciprocal as floating-point result.
             * For integer mode, 1/(base^|exp|) gives 0 for |base|>1 or base=0.
             * Special cases: 1^N = 1, (-1)^N = ±1. */
            uint64_t abs_exp = (uint64_t)(-exp);
            uint64_t power = 1;
            int overflow = 0;
            for (uint64_t i = 0; i < abs_exp; i++) {
                uint64_t prev = power;
                power *= (left >= 0 ? (uint64_t)left : (uint64_t)(-left));
                if (power / (left >= 0 ? (uint64_t)left : (uint64_t)(-left)) != prev && i > 0) {
                    overflow = 1;
                    break;
                }
            }
            if (overflow || power > 1) {
                result = 0;  /* 1/N for N>1 is 0 in integer arithmetic */
            } else if (power == 1) {
                /* 1^N or (-1)^N */
                result = (left == 1) ? 1 : ((abs_exp % 2 == 0) ? 1 : -1);
            } else {
                result = 0;
            }
        } else {
            for (int64_t i = 0; i < exp; i++) {
                result *= left;
            }
        }
        return result;
    }
    return left;
}

/* Parse unary +, - (already handled in primary, this handles +expr, -expr at expr level) */
static int64_t bc_parse_unary(void) {
    bc_skip_spaces();
    if (*bc_pos == '-') {
        bc_pos++;
        return -bc_parse_power();
    }
    if (*bc_pos == '+') {
        bc_pos++;
        return bc_parse_power();
    }
    return bc_parse_power();
}

/* Parse multiplicative operators: *, /, % */
static int64_t bc_parse_term(void) {
    int64_t left = bc_parse_unary();
    for (;;) {
        bc_skip_spaces();
        if (*bc_pos == '*') { bc_pos++; left *= bc_parse_unary(); }
        else if (*bc_pos == '/') {
            bc_pos++;
            int64_t r = bc_parse_unary();
            if (r == 0) { kprintf("bc: division by zero\n"); return 0; }
            left /= r;
        }
        else if (*bc_pos == '%') {
            bc_pos++;
            int64_t r = bc_parse_unary();
            if (r == 0) { kprintf("bc: modulo by zero\n"); return 0; }
            left %= r;
        }
        else break;
    }
    return left;
}

/* Parse shift operators: <<, >> */
static int64_t bc_parse_shift(void) {
    int64_t left = bc_parse_term();
    for (;;) {
        bc_skip_spaces();
        if (*bc_pos == '<' && *(bc_pos+1) == '<') { bc_pos += 2; left <<= bc_parse_term(); }
        else if (*bc_pos == '>' && *(bc_pos+1) == '>') { bc_pos += 2; left >>= bc_parse_term(); }
        else break;
    }
    return left;
}

/* Parse bitwise AND: & */
static int64_t bc_parse_band(void) {
    int64_t left = bc_parse_shift();
    for (;;) {
        bc_skip_spaces();
        if (*bc_pos == '&') { bc_pos++; left &= bc_parse_shift(); }
        else break;
    }
    return left;
}

/* Parse bitwise XOR: ^ (handled; note ^ at power level was checked earlier) */
static int64_t bc_parse_bxor(void) {
    int64_t left = bc_parse_band();
    for (;;) {
        bc_skip_spaces();
        if (*bc_pos == '^') {
            /* Check if next char is also special (could be power if preceded by whitespace).
             * At this level, we only handle XOR ^ when it wasn't caught by power parser. */
            if (*(bc_pos-1) == '(' || *(bc_pos-1) == ' ' || *(bc_pos-1) == '\t') {
                /* Power operator — skip; already handled in bc_parse_power */
                break;
            }
            bc_pos++;
            left ^= bc_parse_band();
        }
        else break;
    }
    return left;
}

/* Parse bitwise OR: | */
static int64_t bc_parse_bor(void) {
    int64_t left = bc_parse_bxor();
    for (;;) {
        bc_skip_spaces();
        if (*bc_pos == '|') { bc_pos++; left |= bc_parse_bxor(); }
        else break;
    }
    return left;
}

/* Parse addition/subtraction: +, - */
static int64_t bc_parse_add(void) {
    int64_t left = bc_parse_bor();
    for (;;) {
        bc_skip_spaces();
        if (*bc_pos == '+') { bc_pos++; left += bc_parse_bor(); }
        else if (*bc_pos == '-') { bc_pos++; left -= bc_parse_bor(); }
        else break;
    }
    return left;
}

/* Top-level expression parser */
static int64_t bc_parse_expr(void) {
    return bc_parse_add();
}

/*
 * Main entry point for the bc command.
 * Usage: "bc <expression>" or interactive "bc" (enter expressions line by line)
 */
void cmd_bc(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: bc <expression>\n");
        kprintf("  Simple integer calculator with variables a-z.\n");
        kprintf("  Operators: + - * / %% ^ & | ~ << >> ( )\n");
        kprintf("  Assignment: x = 5 + 3\n");
        kprintf("  Examples:\n");
        kprintf("    bc 2 + 2\n");
        kprintf("    bc x = 5\n");
        kprintf("    bc x * 3 + 1\n");
        kprintf("    bc 0xFF + 0x1\n");
        return;
    }

    bc_pos = args;
    int64_t result = bc_parse_expr();

    /* Check for trailing garbage (partial parse) */
    bc_skip_spaces();
    if (*bc_pos != '\0') {
        /* Print partial result but indicate parse warning */
        kprintf("%lld\n", (long long)result);
        kprintf("  (warning: trailing characters after expression)\n");
        return;
    }

    kprintf("%lld\n", (long long)result);

    /* Show hex for values that don't fit in a small decimal display */
    if (result > 1024 || result < -1024)
        kprintf("  = 0x%llx\n", (unsigned long long)result);
}
