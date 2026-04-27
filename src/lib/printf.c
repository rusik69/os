#include "printf.h"
#include "vga.h"
#include "serial.h"
#include "string.h"

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

/* Output redirect hook: if set, kputchar sends to hook instead of VGA/serial */
static void (*output_hook)(char c, void *ctx) = 0;
static void *output_hook_ctx = 0;

void kprintf_set_hook(void (*hook)(char, void *), void *ctx) {
    output_hook = hook;
    output_hook_ctx = ctx;
}

static void kputchar(char c) {
    if (output_hook) {
        output_hook(c, output_hook_ctx);
        return;
    }
    vga_putchar(c);
    if (c == '\n') serial_putchar('\r');
    serial_putchar(c);
}

static void print_uint(uint64_t val, int base, int pad, char padchar) {
    char buf[64];
    int i = 0;
    const char *digits = "0123456789abcdef";

    if (val == 0) { buf[i++] = '0'; }
    else {
        while (val > 0) {
            buf[i++] = digits[val % base];
            val /= base;
        }
    }
    while (i < pad) buf[i++] = padchar;
    while (i > 0) kputchar(buf[--i]);
}

static void print_int(int64_t val, int pad, char padchar) {
    if (val < 0) {
        kputchar('-');
        val = -val;
    }
    print_uint((uint64_t)val, 10, pad, padchar);
}

int kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int count = 0;

    while (*fmt) {
        if (*fmt != '%') {
            kputchar(*fmt++);
            count++;
            continue;
        }
        fmt++;

        int left_align = 0;
        int pad = 0;
        char padchar = ' ';

        if (*fmt == '-') { left_align = 1; fmt++; }
        if (*fmt == '0' && !left_align) { padchar = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            pad = pad * 10 + (*fmt - '0');
            fmt++;
        }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = 0;
            const char *t = s;
            while (*t) { t++; len++; }
            if (!left_align) { for (int i = len; i < pad; i++) { kputchar(' '); count++; } }
            while (*s) { kputchar(*s++); count++; }
            if (left_align) { for (int i = len; i < pad; i++) { kputchar(' '); count++; } }
            break;
        }
        case 'd': case 'i': {
            int64_t val = va_arg(ap, int64_t);
            if (left_align) {
                /* Print to temp, then pad */
                char buf[64]; int bi = 0;
                int64_t v = val;
                if (v < 0) { buf[bi++] = '-'; v = -v; }
                char dbuf[32]; int di = 0;
                if (v == 0) dbuf[di++] = '0';
                else while (v > 0) { dbuf[di++] = '0' + v % 10; v /= 10; }
                while (di > 0) buf[bi++] = dbuf[--di];
                for (int i = 0; i < bi; i++) { kputchar(buf[i]); count++; }
                for (int i = bi; i < pad; i++) { kputchar(' '); count++; }
            } else {
                print_int(val, pad, padchar);
                count++;
            }
            break;
        }
        case 'u': {
            uint64_t val = va_arg(ap, uint64_t);
            if (left_align) {
                char buf[32]; int bi = 0;
                if (val == 0) buf[bi++] = '0';
                else { char t[32]; int ti = 0; while (val > 0) { t[ti++] = '0' + val % 10; val /= 10; } while (ti > 0) buf[bi++] = t[--ti]; }
                for (int i = 0; i < bi; i++) { kputchar(buf[i]); count++; }
                for (int i = bi; i < pad; i++) { kputchar(' '); count++; }
            } else {
                print_uint(val, 10, pad, padchar);
                count++;
            }
            break;
        }
        case 'x': {
            uint64_t val = va_arg(ap, uint64_t);
            print_uint(val, 16, pad, padchar);
            count++;
            break;
        }
        case 'p': {
            uint64_t val = va_arg(ap, uint64_t);
            kputchar('0'); kputchar('x');
            print_uint(val, 16, 16, '0');
            count++;
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            kputchar(c);
            count++;
            break;
        }
        case '%':
            kputchar('%');
            count++;
            break;
        default:
            kputchar('%');
            kputchar(*fmt);
            count += 2;
            break;
        }
        fmt++;
    }

    va_end(ap);
    return count;
}
