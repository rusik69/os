/*
 * kernel printf implementation — format string engine
 * ====================================================
 *
 * Architecture overview:
 *   This file implements the kernel's formatted output subsystem. It provides
 *   three output families:
 *
 *   (A) Direct console output via kprintf() / vkprintf() — sends formatted
 *       text to VGA display, serial port, and the dmesg ring buffer.
 *   (B) printf-like log-level output via kprintf_level() — adds priority
 *       filtering against console_loglevel.
 *   (C) Buffer-based formatting via vsnprintf() / snprintf() / sprintf() —
 *       writes into caller-provided character buffers (used by userspace
 *       fallbacks and module code).
 *
 * Format specifiers supported (common to all paths):
 *   %s  — null-terminated string (shows "(null)" for NULL pointer)
 *   %d, %i — signed 64-bit decimal integer
 *   %u  — unsigned 64-bit decimal integer
 *   %x  — unsigned 64-bit hexadecimal (lowercase a-f)
 *   %p  — pointer (always 0x-prefixed, 16 hex digits on LP64)
 *   %c  — single character
 *   %%  — literal percent sign
 *   %f, %F — double in fixed-point notation (%f only in buffer path)
 *   %e, %E — double in scientific notation (buffer path only)
 *   %g, %G — double in shortest representation (buffer path only)
 *   %pK — kernel pointer with access restriction (hides from unprivileged)
 *   %pX — hex dump of N bytes (takes two args: addr, len)
 *   %pV — va_format passthrough for structured log messages
 *   %pf — function address (prints as hex pointer)
 *
 * Flags and modifiers:
 *   -  : left-align the output within the field width
 *   0  : zero-pad numeric fields (ignored when left-align is active)
 *   <digits> : minimum field width (right-pad with spaces, or zero-pad with '0')
 *   l, ll, z : length modifiers — all behave identically on LP64 (64-bit),
 *              consumed but ignored.
 *
 * Output architecture:
 *   Every formatted character eventually flows through kputchar() which:
 *     1. Writes it into the dmesg ring buffer (circular, 64 KB static)
 *     2. Dispatches to output_hook if set, otherwise to vga_putchar() + serial
 *   The ring buffer supports dmesg readback, resize, and flush-to-serial.
 */

#include "printf.h"
#include "vga.h"
#include "serial.h"
#include "string.h"
#include "timer.h"
#include "heap.h"
#include "kptr_restrict.h"
#include "export.h"

/* struct va_format — provided by printf.h */

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

/* Kernel log level configuration */
int console_loglevel = 7;       /* default: print everything */
int default_message_loglevel = 4; /* default: KERN_WARNING */

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

/* dmesg ring buffer: static 64 KB */
static int dmesg_ring_size = 65536;
static char dmesg_ring_buffer[65536];
static char *dmesg_buf = dmesg_ring_buffer;
static int dmesg_pos = 0;          /* next write position (wraps) */
static int dmesg_full = 0;         /* 1 once the buffer has wrapped */
static int dmesg_initialized = 1;

/* Initialize dmesg buffer on first use */
static void dmesg_init(void) {
    if (!dmesg_initialized) {
        if (!dmesg_buf) {
            dmesg_buf = (char *)kmalloc((size_t)dmesg_ring_size);
            if (!dmesg_buf) {
                /* Fallback to minimal static buffer */
                dmesg_ring_size = 4096;
                dmesg_buf = (char *)kmalloc((size_t)dmesg_ring_size);
                if (!dmesg_buf) return;
            }
        }
        memset(dmesg_buf, 0, (size_t)dmesg_ring_size);
        dmesg_pos = 0;
        dmesg_full = 0;
        dmesg_initialized = 1;
    }
}

int dmesg_get_size(void) {
    return dmesg_ring_size;
}

void dmesg_resize(int new_size) {
    if (new_size < 1024) new_size = 1024;
    if (new_size > 1024 * 1024) new_size = 1024 * 1024;

    char *new_buf = (char *)kmalloc((size_t)new_size);
    if (!new_buf) return;

    memset(new_buf, 0, (size_t)new_size);

    /* Copy old data */
    if (dmesg_buf && dmesg_initialized) {
        int copy_size = dmesg_ring_size < new_size ? dmesg_ring_size : new_size;
        if (dmesg_full) {
            int first_part = dmesg_ring_size - dmesg_pos;
            if (first_part > copy_size) first_part = copy_size;
            memcpy(new_buf, dmesg_buf + dmesg_pos, (size_t)first_part);
            if (first_part < copy_size) {
                memcpy(new_buf + first_part, dmesg_buf, (size_t)(copy_size - first_part));
            }
        } else {
            int copy = dmesg_pos < copy_size ? dmesg_pos : copy_size;
            memcpy(new_buf, dmesg_buf, (size_t)copy);
        }
    }

    if (dmesg_buf && dmesg_buf != dmesg_ring_buffer) kfree(dmesg_buf);
    dmesg_buf = new_buf;
    dmesg_ring_size = new_size;
    dmesg_pos = 0;
    dmesg_full = 0;
    dmesg_initialized = 1;
}

void dmesg_clear(void) {
    dmesg_pos = 0;
    dmesg_full = 0;
    if (dmesg_buf && dmesg_initialized)
        memset(dmesg_buf, 0, (size_t)dmesg_ring_size);
}

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
    if (!dmesg_initialized) return 0;
    int written = 0;
    if (dmesg_full) {
        /* Start from the character just after the current write pointer */
        for (int i = dmesg_pos; i < dmesg_ring_size && written < max - 1; i++)
            buf[written++] = dmesg_buf[i];
    }
    for (int i = 0; i < dmesg_pos && written < max - 1; i++)
        buf[written++] = dmesg_buf[i];
    buf[written] = '\0';
    return written;
}

void kprintf_dmesg_clear(void) {
    dmesg_pos = 0;
    dmesg_full = 0;
}

/* Return the number of bytes currently in use in the dmesg ring buffer */
int kprintf_dmesg_used(void) {
    if (!dmesg_initialized) return 0;
    if (dmesg_full) return dmesg_ring_size;
    return dmesg_pos;
}

/* ── Ratelimited printing ────────────────────────────────────────────── */

static uint64_t g_ratelimit_last_tick = 0;

__printf(1, 2)
int kprintf_ratelimited(const char *fmt, ...) {
    uint64_t now = timer_get_ticks();
    if (now - g_ratelimit_last_tick < TIMER_FREQ) {
        return 0; /* suppressed */
    }
    g_ratelimit_last_tick = now;

    va_list ap;
    va_start(ap, fmt);
    int ret = vkprintf(fmt, ap);
    va_end(ap);
    return ret;
}

/* Flush the dmesg ring buffer directly to serial in small chunks.
 * No intermediate buffer needed — avoids stack overflow from large
 * buffers in callers that previously allocated char[65536] on the stack. */
void kprintf_dmesg_flush_serial(void) {
    if (!dmesg_initialized) return;
    char chunk[513]; /* 512 + NUL */
    int chunk_idx;

    if (dmesg_full) {
        for (int i = dmesg_pos; i < dmesg_ring_size; ) {
            chunk_idx = 0;
            while (i < dmesg_ring_size && chunk_idx < 512)
                chunk[chunk_idx++] = dmesg_buf[i++];
            chunk[chunk_idx] = '\0';
            serial_write(chunk);
        }
    }
    {
        int i = 0;
        while (i < dmesg_pos) {
            chunk_idx = 0;
            while (i < dmesg_pos && chunk_idx < 512)
                chunk[chunk_idx++] = dmesg_buf[i++];
            chunk[chunk_idx] = '\0';
            serial_write(chunk);
        }
    }
}

void kputchar(char c) {
    /* Lazily initialize dmesg buffer */
    if (!dmesg_initialized) dmesg_init();
    if (!dmesg_buf) return; /* allocation failed, skip */

    /* Always record in ring buffer (even when hook is active) */
    dmesg_buf[dmesg_pos++] = c;
    if (dmesg_pos >= dmesg_ring_size) { dmesg_pos = 0; dmesg_full = 1; }

    if (output_hook) {
        output_hook(c, output_hook_ctx);
        return;
    }
    vga_putchar(c);
    if (c == '\n') serial_putchar('\r');
    serial_putchar(c);
}

/* ── Core integer-to-string conversion ──────────────────────────────── */
/*
 * print_uint — convert an unsigned 64-bit value to its text representation.
 * @val:     the value to print
 * @base:    numeric base (10 for decimal, 16 for hexadecimal)
 * @pad:     minimum field width (right-padded with padchar if wider than digits)
 * @padchar: padding character (' ' for space, '0' for zero)
 *
 * Returns the number of characters emitted via kputchar().
 */
static int print_uint(uint64_t val, int base, int pad, char padchar) {
    char buf[64];
    int i = 0, chars = 0;
    const char *digits = "0123456789abcdef";

    if (val == 0) { buf[i++] = '0'; }
    else {
        while (val > 0) {
            buf[i++] = digits[val % base];
            val /= base;
        }
    }
    while (i < pad) buf[i++] = padchar;
    while (i > 0) { kputchar(buf[--i]); chars++; }
    return chars;
}

static int print_int(int64_t val, int pad, char padchar) {
    int chars = 0;
    if (val < 0) {
        kputchar('-');
        chars++;
        /* Avoid UB on INT64_MIN: negate as unsigned */
        val = -(val + 1);
        return chars + print_uint((uint64_t)val + 1, 10, pad, padchar);
    }
    return chars + print_uint((uint64_t)val, 10, pad, padchar);
}

/* ── Variadic kernel printf (format engine core) ───────────────────── */
/*
 * vkprintf — kernel printf core accepting a va_list.
 * @fmt: format string with %-specifiers
 * @ap:  variable argument list
 *
 * Parses and processes format specifiers, dispatching each character
 * through kputchar(). This is a simplified engine compared to kprintf():
 * it does NOT handle left-align or floating-point specifiers, but
 * supports the core integer, string, pointer, and character formats.
 * Used internally by kprintf_level() and as a fallback for %pV.
 */
__printf(1, 0)
int vkprintf(const char *fmt, va_list ap) {
    /* Re-use kprintf's format engine by simulating variadic dispatch.
     * kprintf uses va_arg internally, so we provide the caller's va_list
     * by copying its state into a new va_list. */
    /* NOTE: This is a simplified version that only supports basic formats.
     * For full format support, use kprintf directly. */
    int count = 0;
    while (*fmt) {
        if (*fmt != '%') { kputchar(*fmt++); count++; continue; }
        fmt++;
        int pad = 0; char padchar = ' ';
        if (*fmt == '0') { padchar = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { pad = pad * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == 'l') { fmt++; if (*fmt == 'l') fmt++; }
        else if (*fmt == 'z') fmt++;
        switch (*fmt) {
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; while (*s) { kputchar(*s++); count++; } break; }
        case 'd': case 'i': count += print_int(va_arg(ap, int64_t), pad, padchar); break;
        case 'u': count += print_uint(va_arg(ap, uint64_t), 10, pad, padchar); break;
        case 'x': count += print_uint(va_arg(ap, uint64_t), 16, pad, padchar); break;
        case 'p': {
            /* Check for %pK (kernel pointer restriction) */
            if (*(fmt + 1) == 'K') {
                fmt++;
                if (kptr_restrict_check()) {
                    /* Restricted: hide kernel pointer — output 0x0000000000000000 */
                    kputchar('0'); kputchar('x'); count += 2;
                    for (int i = 0; i < 16; i++) { kputchar('0'); count++; }
                } else {
                    uint64_t v = va_arg(ap, uint64_t);
                    kputchar('0'); kputchar('x'); count += 2;
                    count += print_uint(v, 16, 16, '0');
                }
                break;
            }
            /* Regular %p */
            uint64_t v = va_arg(ap, uint64_t);
            kputchar('0'); kputchar('x'); count += 2;
            count += print_uint(v, 16, 16, '0');
            break;
        }
        case 'c': { char c = (char)va_arg(ap, int); kputchar(c); count++; break; }
        case '%': kputchar('%'); count++; break;
        default: kputchar('%'); kputchar(*fmt); count += 2; break;
        }
        fmt++;
    }
    return count;
}

/* ── Main kernel printf (full format engine) ───────────────────────── */
/*
 * kprintf — kernel printf with full format support.
 * @fmt: format string with %-specifiers
 *
 * This is the primary print entry-point for kernel code. It supports the
 * full set of format specifiers documented at the top of this file,
 * including left-alignment ('-'), zero-padding ('0'), minimum field width,
 * and the %p extended pointer specifiers (%pK, %pX, %pV, %pf).
 *
 * Format parsing state machine:
 *   1. Copy literal characters verbatim -> kputchar()
 *   2. On '%', enter format-specifier parsing:
 *      a. Optional '-' flag -> left_align = 1
 *      b. Optional '0' flag -> padchar = '0'
 *      c. Optional digit sequence -> pad (field width)
 *      d. Optional length modifier (l/ll/z) -> consumed, ignored on LP64
 *      e. Single-char conversion specifier -> dispatch
 *   3. Unknown specifiers -> emit '%' + the unknown char literally
 */
__printf(1, 2)
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
        /* Skip length modifiers (l, ll, z — all are same on LP64) */
        if (*fmt == 'l') { fmt++; if (*fmt == 'l') fmt++; }
        else if (*fmt == 'z') fmt++;

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
                char buf[64]; int bi = 0;
                int64_t v = val;
                if (v < 0) { buf[bi++] = '-'; v = -(v + 1); }
                char dbuf[32]; int di = 0;
                if (v == 0 && val < 0) { dbuf[di++] = '1'; v = 0; }
                uint64_t uv = val < 0 ? (uint64_t)v + 1 : (uint64_t)v;
                if (uv == 0) dbuf[di++] = '0';
                else while (uv > 0) { dbuf[di++] = (char)('0' + uv % 10); uv /= 10; }
                while (di > 0) buf[bi++] = dbuf[--di];
                for (int i = 0; i < bi; i++) { kputchar(buf[i]); count++; }
                for (int i = bi; i < pad; i++) { kputchar(' '); count++; }
            } else {
                count += print_int(val, pad, padchar);
            }
            break;
        }
        case 'u': {
            uint64_t val = va_arg(ap, uint64_t);
            if (left_align) {
                char buf[32]; int bi = 0;
                if (val == 0) buf[bi++] = '0';
                else { char t[32]; int ti = 0; while (val > 0) { t[ti++] = (char)('0' + val % 10); val /= 10; } while (ti > 0) buf[bi++] = t[--ti]; }
                for (int i = 0; i < bi; i++) { kputchar(buf[i]); count++; }
                for (int i = bi; i < pad; i++) { kputchar(' '); count++; }
            } else {
                count += print_uint(val, 10, pad, padchar);
            }
            break;
        }
        case 'x': {
            uint64_t val = va_arg(ap, uint64_t);
            count += print_uint(val, 16, pad, padchar);
            break;
        }
        case 'p': {
            /* Check for %pK (kernel pointer restriction) */
            if (*(fmt + 1) == 'K') {
                fmt++;
                if (kptr_restrict_check()) {
                    /* Restricted: hide kernel pointer — output 0x0000000000000000 */
                    kputchar('0'); kputchar('x'); count += 2;
                    for (int i = 0; i < 16; i++) { kputchar('0'); count++; }
                } else {
                    uint64_t val = va_arg(ap, uint64_t);
                    kputchar('0'); kputchar('x'); count += 2;
                    count += print_uint(val, 16, 16, '0');
                }
                break;
            }
            /* Check for %pX (hex dump) */
            if (*(fmt + 1) == 'X') {
                fmt++;
                const void *addr = va_arg(ap, const void *);
                int len = (int)va_arg(ap, int64_t);
                const uint8_t *p = (const uint8_t *)addr;
                for (int i = 0; i < len; i++) {
                    char hex[3];
                    hex[0] = "0123456789abcdef"[(p[i] >> 4) & 0xF];
                    hex[1] = "0123456789abcdef"[p[i] & 0xF];
                    hex[2] = '\0';
                    for (int j = 0; j < 2; j++) { kputchar(hex[j]); count++; }
                    if ((i & 15) == 15 && i + 1 < len) { kputchar('\n'); count++; }
                }
                break;
            }
            /* Check for %pV (va_format passthrough) */
            if (*(fmt + 1) == 'V') {
                fmt++;
                struct va_format *vaf = va_arg(ap, struct va_format *);
                if (!vaf || !vaf->fmt) { const char *ns = "(null)"; while (*ns) { kputchar(*ns++); count++; } break; }
                if (vaf->va) {
                    count += vkprintf(vaf->fmt, *vaf->va);
                } else {
                    const char *s = vaf->fmt;
                    while (*s) { kputchar(*s++); count++; }
                }
                break;
            }
            /* Check for %pf (function name) */
            if (*(fmt + 1) == 'f') {
                fmt++;
                void *addr = va_arg(ap, void *);
                kputchar('0'); kputchar('x'); count += 2;
                count += print_uint((uint64_t)(uintptr_t)addr, 16, 16, '0');
                break;
            }
            /* Regular %p — always show pointer */
            uint64_t val = va_arg(ap, uint64_t);
            kputchar('0'); kputchar('x'); count += 2;
            count += print_uint(val, 16, 16, '0');
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

__printf(2, 3)
int kprintf_level(int level, const char *fmt, ...) {
    /* Filter by console_loglevel: only print if level <= console_loglevel */
    if (level > console_loglevel) return 0;

    va_list ap;
    va_start(ap, fmt);
    int count = vkprintf(fmt, ap);
    va_end(ap);
    return count;
}

/* ── snprintf buffer-writer and helpers ─────────────────────────────── */

/* Buffer context passed through snprintf format expansion. */
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

/* ── Floating-point formatting (buffer-only: vsnprintf path) ────────── */
/*
 * This family implements IEEE 754 double-precision conversion for the
 * three standard printf floating-point specifiers:
 *
 *   %f / %F — fixed-point ("123.456000")
 *   %e / %E — scientific notation ("1.234560e+02")
 *   %g / %G — shortest representation (fixed or sci, whichever is shorter)
 *
 * All three convert via double → integer part + fractional part with the
 * specified precision (default 6). Clamping guards against NaN and values
 * exceeding UINT64_MAX. These are only available in the vsnprintf() path
 * (buffer output), not in the direct-console kprintf() path.
 */

/* Format a double in fixed-point notation (%f).
 * Writes the integer part, a decimal point, and {precision} fractional digits. */
static int sn_double_fixed(snbuf_t *b, double val, int precision) {
    int written = 0;
    if (val < 0) { sn_write(b, '-'); written++; val = -val; }
    /* Extract integer part — guard against NaN / overflow for safe conversion */
    uint64_t int_part = 0;
    if (val >= 0.0 && val <= (double)UINT64_MAX) {
        int_part = (uint64_t)val;
    } else if (val > (double)UINT64_MAX) {
        int_part = UINT64_MAX;
    }
    written += sn_uint(b, int_part, 10, 0, ' ', 0);
    if (precision > 0) {
        sn_write(b, '.'); written++;
        double frac = val - (double)int_part;
        for (int i = 0; i < precision; i++) {
            frac *= 10.0;
            int digit = (int)frac;
            if (digit > 9) digit = 9;
            sn_write(b, '0' + (char)digit);
            written++;
            frac -= (double)digit;
        }
    }
    return written;
}

/* Format a double in scientific notation (%e). */
static int sn_double_sci(snbuf_t *b, double val, int precision) {
    int written = 0;
    if (val < 0) { sn_write(b, '-'); written++; val = -val; }
    int exp = 0;
    if (val != 0.0) {
        while (val >= 10.0) { val /= 10.0; exp++; }
        while (val < 1.0) { val *= 10.0; exp--; }
    }
    /* Write the mantissa */
    written += sn_double_fixed(b, val, precision);
    sn_write(b, 'e'); written++;
    if (exp >= 0) { sn_write(b, '+'); written++; }
    else { sn_write(b, '-'); written++; exp = -exp; }
    written += sn_uint(b, (uint64_t)exp, 10, 2, '0', 0);
    return written;
}

/* Format a double in shortest representation (%g).
 * Uses fixed-point if exponent is in [-4, precision), scientific otherwise. */
static int sn_double_shortest(snbuf_t *b, double val, int precision) {
    if (precision < 1) precision = 1;
    if (val == 0.0) {
        sn_write(b, '0'); return 1;
    }
    /* Estimate exponent */
    double v = val < 0 ? -val : val;
    int exp = 0;
    while (v >= 10.0) { v /= 10.0; exp++; }
    while (v < 1.0) { v *= 10.0; exp--; }
    if (exp >= -4 && exp < precision) {
        return sn_double_fixed(b, val, precision - exp - 1);
    } else {
        return sn_double_sci(b, val, precision - 1);
    }
}

/* ── vsnprintf — formatted output into a fixed-size buffer ──────────── */
/*
 * vsnprintf — format a string into a caller-provided buffer.
 * @buf: destination buffer (NULL-safe: returns 0 if NULL)
 * @n:   size of destination buffer (0-safe: returns 0 if 0)
 * @fmt: printf-style format string
 * @ap:  variable argument list
 *
 * This is the buffer-based counterpart of kprintf(). It supports all
 * format specifiers that kprintf() does PLUS floating-point (%f, %e, %g).
 * The output is always NUL-terminated (writes at most n-1 characters
 * plus the NUL terminator). Returns the number of characters that would
 * have been written had the buffer been large enough (excluding NUL),
 * consistent with C99 snprintf semantics for the buffer path.
 */
__printf(3, 0)
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
        /* Skip length modifiers */
        if (*fmt == 'l') { fmt++; if (*fmt == 'l') fmt++; }
        else if (*fmt == 'z') fmt++;
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
                else while (v > 0) { d2[di++] = (char)('0' + (int)(v % 10)); v /= 10; }
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
        /* ── Floating-point format specifiers ────────────────────────── */
        case 'f': case 'F': {
            double val = va_arg(ap, double);
            int precision = 6;
            count += sn_double_fixed(&b, val, precision);
            break;
        }
        case 'e': case 'E': {
            double val = va_arg(ap, double);
            int precision = 6;
            count += sn_double_sci(&b, val, precision);
            break;
        }
        case 'g': case 'G': {
            double val = va_arg(ap, double);
            int precision = 6;
            count += sn_double_shortest(&b, val, precision);
            break;
        }
        case 'p': {
            /* %pX — hex dump */
            if (*(fmt + 1) == 'X') {
                fmt++;
                const void *addr = va_arg(ap, const void *);
                int len = (int)va_arg(ap, int64_t);
                const uint8_t *p = (const uint8_t *)addr;
                for (int i = 0; i < len; i++) {
                    sn_write(&b, "0123456789abcdef"[(p[i] >> 4) & 0xF]);
                    sn_write(&b, "0123456789abcdef"[p[i] & 0xF]);
                    count += 2;
                    if ((i & 15) == 15 && i + 1 < len) { sn_write(&b, '\n'); count++; }
                }
                break;
            }
            /* %pV — va_format passthrough */
            if (*(fmt + 1) == 'V') {
                fmt++;
                struct va_format *vaf = va_arg(ap, struct va_format *);
                if (!vaf || !vaf->fmt) { const char *ns = "(null)"; while (*ns) { sn_write(&b, *ns++); count++; } break; }
                if (vaf->va) {
                    count += vsnprintf(b.buf + b.pos, b.max - b.pos, vaf->fmt, *vaf->va);
                    b.pos += count;
                    if (b.pos >= b.max - 1) b.pos = b.max - 1;
                    b.buf[b.pos] = '\0';
                } else {
                    const char *s = vaf->fmt;
                    while (*s) { sn_write(&b, *s++); count++; }
                }
                break;
            }
            /* %pf — function name (prints address) */
            if (*(fmt + 1) == 'f') {
                fmt++;
                void *addr = va_arg(ap, void *);
                count += sn_uint(&b, (uint64_t)(uintptr_t)addr, 16, 16, '0', 0);
                break;
            }
            /* %pK — kernel pointer restriction */
            if (*(fmt + 1) == 'K') {
                fmt++;
                if (kptr_restrict_check()) {
                    sn_write(&b, '0'); sn_write(&b, 'x');
                    count += 2;
                    for (int i = 0; i < 16; i++) { sn_write(&b, '0'); count++; }
                } else {
                    uint64_t val = va_arg(ap, uint64_t);
                    sn_write(&b, '0'); sn_write(&b, 'x');
                    count += sn_uint(&b, val, 16, 16, '0', 0);
                }
                break;
            }
            /* Regular %p — always show pointer */
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

/* ── snprintf — vsnprintf wrapper with va_args ───────────────────────── */
/*
 * snprintf — convenience wrapper around vsnprintf for variadic callers.
 * @buf: destination buffer
 * @n:   buffer size
 * @fmt: printf-style format string
 */
__printf(3, 4)
int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap); return r;
}

/* ── sprintf — unbounded variant (use snprintf instead) ─────────────── */
/*
 * sprintf — format into an unbounded buffer.
 * @buf: destination buffer (assumed large enough)
 * @fmt: printf-style format string
 *
 * WARNING: This function does not take a size parameter and will
 * overrun the buffer if the output exceeds its capacity. Prefer
 * snprintf() in all new code.
 */
__printf(2, 3)
int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap); return r;
}

/* ── Exported symbols for module loading ──────────────────────────── */
/* kprintf is exported so loadable kernel modules can use formatted output. */
EXPORT_SYMBOL(kprintf);

/* ── vsprintf — unbounded buffer variant (static, internal only) ──── */
/*
 * vsprintf — format into an unbounded buffer (internal helper).
 * This is a static function used only within this file as the va_list
 * counterpart of sprintf(). No overflow protection — callers must
 * ensure the buffer is large enough.
 */
__printf(2, 0)
static int vsprintf(char *buf, const char *fmt, va_list args)
{
    return vsnprintf(buf, (size_t)-1, fmt, args);
}
