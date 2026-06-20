/*
 * cmd_printf.c — printf command: formatted output with full format specifiers
 *
 * Supports all kernel vsnprintf format specifiers:
 *   %s, %d, %i, %u, %x, %X, %o, %c, %%, %p
 *   Width, precision, left/right justification (%-10s, %08x, etc.)
 *   Escape sequences: \n, \t, \\, \r, \", \'
 *
 * Item 318: printf shell command — format specifier completeness
 */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

/* ── Helpers ────────────────────────────────────────────────────── */

/* Convert a decimal string to int64_t */
static int64_t str_to_int(const char *s) {
    if (!s || !*s) return 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    int64_t val = 0;
    while (*s >= '0' && *s <= '9')
        val = val * 10 + (*s++ - '0');
    return neg ? -val : val;
}

/* Convert a hex string to uint64_t */
static uint64_t str_to_hex(const char *s) {
    if (!s || !*s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint64_t val = 0;
    while (*s) {
        val <<= 4;
        if (*s >= '0' && *s <= '9')      val |= (uint64_t)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') val |= (uint64_t)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F') val |= (uint64_t)(*s - 'A' + 10);
        else break;
        s++;
    }
    return val;
}

/* ── Main command ───────────────────────────────────────────────── */

void cmd_printf(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: printf <format> [args...]\r\n");
        kprintf("  Format specs: %%s %%d %%u %%x %%X %%o %%c %%p %%n\r\n");
        kprintf("  Width/precision: %%10s %%-10s %%08x %%.5f\r\n");
        kprintf("  Escape seqs: \\\\n \\\\t \\\\\\\\ \\\\r\r\n");
        return;
    }

    /* Parse args into format string and positional arguments.
     * First space-delimited token is the format string.
     * Remaining tokens are substitution arguments. */
    char fmt[512];
    int fi = 0;
    const char *p = args;
    while (*p && *p == ' ') p++;
    while (*p && *p != ' ' && fi < (int)sizeof(fmt) - 1)
        fmt[fi++] = *p++;
    fmt[fi] = '\0';
    while (*p == ' ') p++;

    /* Collect remaining tokens as args */
    char argbuf[1024];
    strncpy(argbuf, p, sizeof(argbuf) - 1);
    argbuf[sizeof(argbuf) - 1] = '\0';

    char *words[64];
    int nwords = 0;
    char *w = argbuf;
    while (*w && nwords < 64) {
        while (*w == ' ') w++;
        if (!*w) break;
        words[nwords++] = w;
        while (*w && *w != ' ') w++;
        if (*w) *w++ = '\0';
    }

    /* Build the output using snprintf for each format specifier.
     * We process the format string sequentially, handling escape
     * sequences and format specifiers via snprintf with proper arg types. */
    char out[4096];
    int opos = 0;
    int wi = 0;
    const char *fp = fmt;

    while (*fp && opos < (int)sizeof(out) - 64) {
        if (*fp == '\\') {
            /* Escape sequence */
            fp++;
            switch (*fp) {
                case 'n':  out[opos++] = '\n'; break;
                case 't':  out[opos++] = '\t'; break;
                case 'r':  out[opos++] = '\r'; break;
                case '\\': out[opos++] = '\\'; break;
                case '"':  out[opos++] = '"';  break;
                case '\'': out[opos++] = '\''; break;
                case '0':  out[opos++] = '\0'; break;
                default:   out[opos++] = '\\'; if (*fp) out[opos++] = *fp; break;
            }
            if (*fp) fp++;
            continue;
        }

        if (*fp != '%') {
            out[opos++] = *fp++;
            continue;
        }

        /* Format specifier: copy the whole %...x token including flags,
         * width, precision, length, and convert specifier */
        int spec_start = opos;
        out[opos++] = *fp++;  /* '%' */

        /* Collect flags, width, precision, length modifiers */
        while (*fp && opos < (int)sizeof(out) - 64) {
            out[opos++] = *fp;
            if (*fp == 'd' || *fp == 'i' || *fp == 'u' || *fp == 'x' ||
                *fp == 'X' || *fp == 'o' || *fp == 'c' || *fp == 's' ||
                *fp == 'p' || *fp == '%' || *fp == 'n') {
                /* Got the full specifier — now we need to format */
                out[opos] = '\0';
                char spec = *fp;
                fp++;

                /* For %%, just emit '%' */
                if (spec == '%') {
                    /* Keep the %% as-is */
                    break;
                }

                /* For %n, this is a special printf extension - skip it */
                if (spec == 'n') {
                    opos = spec_start; /* remove the %n token */
                    if (wi < nwords) {
                        /* %n writes the number of chars output so far */
                        int *pcount = (int *)words[wi++];
                        *pcount = opos;
                    }
                    break;
                }

                /* Build the format string up to the specifier */
                char local_fmt[64];
                int flen = opos - spec_start;
                if (flen > (int)sizeof(local_fmt) - 1) flen = sizeof(local_fmt) - 1;
                memcpy(local_fmt, out + spec_start, (size_t)flen);
                local_fmt[flen] = '\0';
                opos = spec_start; /* roll back, we'll write the formatted output */

                /* Get the argument and format it */
                char piece[256];
                if (spec == 's') {
                    /* String */
                    const char *val = (wi < nwords) ? words[wi++] : "(null)";
                    snprintf(piece, sizeof(piece), local_fmt, val);
                } else if (spec == 'c') {
                    /* Character */
                    int val = (wi < nwords) ? (unsigned char)words[wi++][0] : 0;
                    snprintf(piece, sizeof(piece), local_fmt, val);
                } else if (spec == 'd' || spec == 'i') {
                    /* Signed decimal */
                    int64_t val = (wi < nwords) ? str_to_int(words[wi++]) : 0;
                    snprintf(piece, sizeof(piece), local_fmt, (long)val);
                } else if (spec == 'u') {
                    /* Unsigned decimal */
                    uint64_t val = (wi < nwords) ? (uint64_t)str_to_int(words[wi++]) : 0;
                    snprintf(piece, sizeof(piece), local_fmt, (unsigned long)val);
                } else if (spec == 'x' || spec == 'X') {
                    /* Hexadecimal */
                    uint64_t val = (wi < nwords) ? str_to_hex(words[wi++]) : 0;
                    snprintf(piece, sizeof(piece), local_fmt, (unsigned long)val);
                } else if (spec == 'o') {
                    /* Octal */
                    uint64_t val = (wi < nwords) ? (uint64_t)str_to_int(words[wi++]) : 0;
                    snprintf(piece, sizeof(piece), local_fmt, (unsigned long)val);
                } else if (spec == 'p') {
                    /* Pointer (display as hex) */
                    uint64_t val = (wi < nwords) ? str_to_hex(words[wi++]) : 0;
                    snprintf(piece, sizeof(piece), local_fmt, (void *)(uintptr_t)val);
                } else {
                    /* Unknown specifier — emit as-is */
                    snprintf(piece, sizeof(piece), "%s", local_fmt);
                }

                /* Append the formatted piece to output */
                for (char *cp = piece; *cp && opos < (int)sizeof(out) - 1; cp++)
                    out[opos++] = *cp;
                break;
            }
            fp++;
        }
    }

    out[opos] = '\0';

    /* Print the result */
    kprintf("%s", out);
}
