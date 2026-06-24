#define KERNEL_INTERNAL
#include "early_serial.h"
#include "io.h"
#include "string.h"

/*
 * early_serial.c — Early serial console for pre-MMU debug output
 *
 * This implementation uses only I/O port instructions (in/out) and
 * requires no kernel state whatsoever — no MMU, no heap, no spinlocks,
 * no interrupts.  It can be called from the very first line of
 * kernel_main() before anything else is initialised.
 *
 * If src/drivers/serial.c already configures COM1 at 115200 8N1, this
 * file provides the same capability as a standalone module with the
 * exact API requested (early_putc, early_puts, early_printf).
 *
 * To avoid linker conflicts with serial.c's early_serial_init(void),
 * the port-taking initialiser is named early_serial_init_port().
 * Use the macro early_serial_init() from the header for default COM1.
 */

/* UART 16550 register offsets (from port base) */
#define UART_RBR  0   /* Receive Buffer Register (DLAB=0, read) */
#define UART_THR  0   /* Transmit Holding Register (DLAB=0, write) */
#define UART_IER  1   /* Interrupt Enable Register */
#define UART_FCR  2   /* FIFO Control Register */
#define UART_LCR  3   /* Line Control Register */
#define UART_MCR  4   /* Modem Control Register */
#define UART_LSR  5   /* Line Status Register */
#define UART_DLL  0   /* Divisor Latch Low (DLAB=1) */
#define UART_DLM  1   /* Divisor Latch High (DLAB=1) */

/* UART flags */
#define UART_LSR_THRE   (1U << 5)  /* Transmitter Holding Register Empty */
#define UART_LSR_DR     (1U << 0)  /* Data Ready */
#define UART_LCR_8BIT   (3 << 0)  /* 8 data bits, no parity, 1 stop */
#define UART_LCR_DLAB   (1U << 7)  /* Divisor Latch Access Bit */

/* Saved port base (default COM1 = 0x3F8) */
static uint16_t g_early_port = 0x3F8;

/* ── Wait for transmitter to be ready ─────────────────────────────── */

static inline void early_tx_wait(void)
{
    for (volatile int timeout = 0; timeout < 100000; timeout++) {
        if (inb(g_early_port + UART_LSR) & UART_LSR_THRE)
            return;
        __asm__ volatile("pause");
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

void early_serial_init_port(uint16_t port)
{
    g_early_port = port;

    /* Set baud rate: divisor = 115200 / 115200 = 1 */
    outb(g_early_port + UART_LCR, UART_LCR_DLAB);  /* enable DLAB */
    outb(g_early_port + UART_DLL, 0x01);            /* 115200 baud lo */
    outb(g_early_port + UART_DLM, 0x00);            /* 115200 baud hi */

    /* Line control: 8 bits, no parity, 1 stop bit */
    outb(g_early_port + UART_LCR, UART_LCR_8BIT);

    /* FIFO: disable (keep it simple for early boot) */
    outb(g_early_port + UART_FCR, 0x00);

    /* Modem control: set DTR/RTS (required for QEMU serial to work) */
    outb(g_early_port + UART_MCR, 0x03);

    /* Flush any stale byte from the receiver */
    (void)inb(g_early_port + UART_RBR);
}

void early_putc(char c)
{
    if (c == '\n')
        early_putc('\r');

    early_tx_wait();
    outb(g_early_port + UART_THR, (uint8_t)c);
}

void early_puts(const char *s)
{
    if (!s) return;
    while (*s)
        early_putc(*s++);
}

void early_printf(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    char c;
    while ((c = *fmt++) != '\0') {
        if (c != '%') {
            early_putc(c);
            continue;
        }

        /* Handle format specifier */
        c = *fmt++;
        switch (c) {
        case 's': {
            const char *str = __builtin_va_arg(ap, const char *);
            if (!str) str = "(null)";
            early_puts(str);
            break;
        }
        case 'd': {
            int64_t val = __builtin_va_arg(ap, int64_t);
            if (val < 0) {
                early_putc('-');
                val = -val;
            }
            /* Convert to decimal string (reverse order) */
            char buf[21];
            int pos = 20;
            buf[20] = '\0';
            if (val == 0) {
                early_putc('0');
            } else {
                while (val > 0 && pos > 0) {
                    pos--;
                    buf[pos] = (char)('0' + (val % 10));
                    val /= 10;
                }
                early_puts(&buf[pos]);
            }
            break;
        }
        case 'x': {
            uint64_t val = __builtin_va_arg(ap, uint64_t);
            const char hex_chars[] = "0123456789abcdef";
            char buf[19]; /* "0x" + 16 digits + NUL */
            int i;
            buf[0] = '0';
            buf[1] = 'x';
            for (i = 0; i < 16; i++) {
                buf[2 + i] = hex_chars[(val >> ((15 - i) * 4)) & 0xF];
            }
            buf[18] = '\0';
            /* Strip leading zeros */
            {
                char *p = buf + 2;
                while (*p == '0' && *(p + 1) != '\0') p++;
                early_puts(p - 2); /* include "0x" */
            }
            break;
        }
        case 'c': {
            int ch = __builtin_va_arg(ap, int);
            early_putc((char)ch);
            break;
        }
        case '%':
            early_putc('%');
            break;
        default:
            /* Unknown specifier — print literal */
            early_putc('%');
            if (c) early_putc(c);
            break;
        }
    }

    __builtin_va_end(ap);
}


/* ── Stub: early_serial_putc ─────────────────────────────── */
int early_serial_putc(char c)
{
    (void)c;
    kprintf("[early_serial] early_serial_putc: not yet implemented\n");
    return 0;
}
/* ── Stub: early_serial_getc ─────────────────────────────── */
int early_serial_getc(char *c)
{
    (void)c;
    kprintf("[early_serial] early_serial_getc: not yet implemented\n");
    return 0;
}
/* ── Stub: early_serial_write ─────────────────────────────── */
int early_serial_write(const char *buf, size_t len)
{
    (void)buf;
    (void)len;
    kprintf("[early_serial] early_serial_write: not yet implemented\n");
    return 0;
}
