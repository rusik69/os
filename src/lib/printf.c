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

/* Flush hook: called by kprintf_flush() to push buffered output */
static void (*flush_hook)(void *ctx) = 0;
static void *flush_hook_ctx = 0;

void kprintf_set_flush(void (*fn)(void *), void *ctx) {
    flush_hook = fn;
    flush_hook_ctx = ctx;
}
void kprintf_flush(void) {
    if (flush_hook) flush_hook(flush_hook_ctx);
}

/* dmesg ring buffer: 16 KB, always captures every character */
#define DMESG_BUF_SIZE 16384
static char dmesg_buf[DMESG_BUF_SIZE];
static int  dmesg_pos = 0;          /* next write position (wraps) */
static int  dmesg_full = 0;         /* 1 once the buffer has wrapped */

void kprintf_set_hook(void (*hook)(char, void *), void *ctx) {
    output_hook = hook;
    output_hook_ctx = ctx;
}
void kprintf_get_hook(void (**hook)(char,void*), void **ctx) {
    if (hook) *hook = output_hook;
    if (ctx)  *ctx  = output_hook_ctx;
}

/* Fill buf with the dmesg contents (oldest first). Returns bytes written. */
int kprintf_dmesg(char *buf, int max) {
    int written = 0;
    if (dmesg_full) {
        /* Start from the character just after the current write pointer */
        for (int i = dmesg_pos; i < DMESG_BUF_SIZE && written < max - 1; i++)
            buf[written++] = dmesg_buf[i];
    }
    for (int i = 0; i < dmesg_pos && written < max - 1; i++)
        buf[written++] = dmesg_buf[i];
    buf[written] = '\0';
    return written;
}

static void kputchar(char c) {
    /* Always record in ring buffer (even when hook is active) */
    dmesg_buf[dmesg_pos++] = c;
    if (dmesg_pos >= DMESG_BUF_SIZE) { dmesg_pos = 0; dmesg_full = 1; }

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

/* --- vsnprintf / snprintf / sprintf ---------------------------------- */

typedef struct { char *buf; size_t pos; size_t max; } snbuf_t;

static void sn_write(snbuf_t *b, char c) {
    if (b->pos < b->max - 1) b->buf[b->pos++] = c;
}

static int sn_uint(snbuf_t *b, uint64_t val, int base, int pad,
                   char padchar, int left_align) {
    char tmp[64]; int i = 0;
    const char *digits = "0123456789abcdef";
    if (val == 0) tmp[i++] = '0';
    else while (val) { tmp[i++] = digits[val % base]; val /= base; }
    int len = i, written = 0;
    if (!left_align) for (int j = len; j < pad; j++) { sn_write(b, padchar); written++; }
    for (int j = len - 1; j >= 0; j--) { sn_write(b, tmp[j]); written++; }
    if (left_align)  for (int j = len; j < pad; j++) { sn_write(b, ' ');    written++; }
    return written;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    if (!buf || n == 0) return 0;
    snbuf_t b = { buf, 0, n };
    int count = 0;
    while (*fmt) {
        if (*fmt != '%') { sn_write(&b, *fmt++); count++; continue; }
        fmt++;
        int left_align = 0, pad = 0; char padchar = ' ';
        if (*fmt == '-') { left_align = 1; fmt++; }
        if (*fmt == '0' && !left_align) { padchar = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { pad = pad * 10 + (*fmt - '0'); fmt++; }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = 0; const char *t = s; while (*t) { t++; len++; }
            if (!left_align) for (int i = len; i < pad; i++) { sn_write(&b, ' '); count++; }
            while (*s) { sn_write(&b, *s++); count++; }
            if (left_align)  for (int i = len; i < pad; i++) { sn_write(&b, ' '); count++; }
            break;
        }
        case 'd': case 'i': {
            int64_t val = va_arg(ap, int64_t);
            if (left_align) {
                char tmp[32]; int ti = 0;
                int64_t v = val;
                if (v < 0) { tmp[ti++] = '-'; v = -v; }
                char d2[32]; int di = 0;
                if (v == 0) d2[di++] = '0';
                else while (v > 0) { d2[di++] = '0' + (int)(v % 10); v /= 10; }
                while (di > 0) tmp[ti++] = d2[--di];
                for (int i = 0; i < ti; i++) { sn_write(&b, tmp[i]); count++; }
                for (int i = ti; i < pad; i++) { sn_write(&b, ' '); count++; }
            } else {
                if (val < 0) { sn_write(&b, '-'); count++; val = -val; }
                count += sn_uint(&b, (uint64_t)val, 10, pad, padchar, 0);
            }
            break;
        }
        case 'u': {
            uint64_t val = va_arg(ap, uint64_t);
            count += sn_uint(&b, val, 10, pad, padchar, left_align);
            break;
        }
        case 'x': {
            uint64_t val = va_arg(ap, uint64_t);
            count += sn_uint(&b, val, 16, pad, padchar, left_align);
            break;
        }
        case 'p': {
            uint64_t val = va_arg(ap, uint64_t);
            sn_write(&b, '0'); sn_write(&b, 'x');
            count += sn_uint(&b, val, 16, 16, '0', 0);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            sn_write(&b, c); count++;
            break;
        }
        case '%': sn_write(&b, '%'); count++; break;
        default:  sn_write(&b, '%'); sn_write(&b, *fmt); count += 2; break;
        }
        fmt++;
    }
    b.buf[b.pos] = '\0';
    return count;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap); return r;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap); return r;
}
