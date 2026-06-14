/* Minimal printf for userspace — writes to stdout via write() */

#include "stdio.h"
#include "unistd.h"
#include "string.h"
#include "stdlib.h"
#include "stdarg.h"

static void print_str(const char *s) {
    if (!s) s = "(null)";
    unsigned long len = strlen(s);
    write(STDOUT_FILENO, s, len);
}

static void print_dec(long long val, int width) {
    char buf[24];
    int neg = 0;
    int pos = 0;
    if (val < 0) { neg = 1; val = -val; }
    if (val == 0) buf[pos++] = '0';
    else {
        while (val > 0 && pos < 23) {
            buf[pos++] = '0' + (val % 10);
            val /= 10;
        }
    }
    int pad = width > pos ? width - pos : 0;
    if (neg) pad--; /* sign takes one space */
    while (pad > 0) {
        buf[pos++] = ' ';
        pad--;
    }
    if (neg) buf[pos++] = '-';
    /* Reverse */
    for (int i = 0; i < pos / 2; i++) {
        char t = buf[i];
        buf[i] = buf[pos - 1 - i];
        buf[pos - 1 - i] = t;
    }
    write(STDOUT_FILENO, buf, pos);
}

static void print_hex(unsigned long long val) {
    char buf[19];
    int pos = 0;
    buf[pos++] = '0';
    buf[pos++] = 'x';
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        int nib = (val >> i) & 0xF;
        if (nib || started || i == 0) {
            started = 1;
            buf[pos++] = nib < 10 ? '0' + nib : 'a' + nib - 10;
        }
    }
    write(STDOUT_FILENO, buf, pos);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int count = 0;

    while (*fmt) {
        if (*fmt != '%') {
            write(STDOUT_FILENO, fmt, 1);
            count++;
            fmt++;
            continue;
        }
        fmt++; /* skip '%' */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        if (*fmt == 'd' || *fmt == 'i') {
            int val = va_arg(ap, int);
            print_dec(val, width);
        } else if (*fmt == 'u') {
            unsigned int val = va_arg(ap, unsigned int);
            print_dec(val, width);
        } else if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') fmt++; /* skip second l for long long */
            if (*fmt == 'd' || *fmt == 'i') {
                long long val = va_arg(ap, long long);
                print_dec(val, width);
            } else if (*fmt == 'u') {
                unsigned long long val = va_arg(ap, unsigned long long);
                print_dec((long long)val, width);
            } else if (*fmt == 'x' || *fmt == 'X') {
                unsigned long long val = va_arg(ap, unsigned long long);
                print_hex(val);
            } else {
                /* Unknown, emit raw */
                write(STDOUT_FILENO, "%l", 2);
                write(STDOUT_FILENO, fmt, 1);
            }
        } else if (*fmt == 'x' || *fmt == 'X') {
            unsigned int val = va_arg(ap, unsigned int);
            print_hex(val);
        } else if (*fmt == 's') {
            const char *s = va_arg(ap, const char *);
            print_str(s);
        } else if (*fmt == 'c') {
            char c = (char)va_arg(ap, int);
            write(STDOUT_FILENO, &c, 1);
        } else if (*fmt == 'p') {
            void *p = va_arg(ap, void *);
            print_hex((unsigned long long)p);
        } else if (*fmt == '%') {
            write(STDOUT_FILENO, "%", 1);
        } else {
            write(STDOUT_FILENO, "%", 1);
            write(STDOUT_FILENO, fmt, 1);
        }
        count++;
        fmt++;
    }

    va_end(ap);
    return count;
}

int putchar(int c) {
    char ch = (char)c;
    if (write(STDOUT_FILENO, &ch, 1) < 0)
        return EOF;
    return (unsigned char)ch;
}

int puts(const char *s) {
    print_str(s);
    write(STDOUT_FILENO, "\n", 1);
    return 1;
}
