#include "serial.h"
#include "io.h"
#include "idt.h"
#include "pic.h"
#include "apic.h"
#include "printf.h"
#include "string.h"

/* ── Per-port state ────────────────────────────────────────────────── */

struct serial_port_state {
    int      initialised;
    uint16_t base;
    uint8_t  irq_num;
    char     rx_buffer[256];
    volatile int rx_head;
    volatile int rx_tail;
    volatile int irq_enabled;
};

static struct serial_port_state g_ports[SERIAL_PORTS_MAX] = {
    { .base = SERIAL_COM1, .irq_num = 4 },
    { .base = SERIAL_COM2, .irq_num = 3 },
    { .base = SERIAL_COM3, .irq_num = 4 },
    { .base = SERIAL_COM4, .irq_num = 3 },
};

/* ── IRQ handler for serial ports ─────────────────────────────────── */

static void serial_irq_handler(struct interrupt_frame *frame, int port_idx) {
    (void)frame;

    struct serial_port_state *port = &g_ports[port_idx];

    while (inb(port->base + UART_LSR) & UART_LSR_DR) {
        char c = (char)inb(port->base + UART_RBR);
        int next = (port->rx_head + 1) % (int)sizeof(port->rx_buffer);
        if (next != port->rx_tail) {
            port->rx_buffer[port->rx_head] = c;
            port->rx_head = next;
        }
    }
}

/* Shared IRQ dispatchers for COM1/COM3 (IRQ 4) and COM2/COM4 (IRQ 3) */
static void irq4_handler(struct interrupt_frame *frame) {
    serial_irq_handler(frame, 0); /* COM1 */
    if (g_ports[2].irq_enabled) serial_irq_handler(frame, 2); /* COM3 */
    irq_ack(4);
}

static void irq3_handler(struct interrupt_frame *frame) {
    serial_irq_handler(frame, 1); /* COM2 */
    if (g_ports[3].irq_enabled) serial_irq_handler(frame, 3); /* COM4 */
    irq_ack(3);
}

/* ── Port initialisation ──────────────────────────────────────────── */

static void uart_init_port(uint16_t base) {
    outb(base + UART_IER, 0x00); /* Disable interrupts */
    outb(base + UART_LCR, UART_LCR_DLAB); /* Enable DLAB */
    outb(base + UART_DLL, 0x01); /* 115200 baud lo */
    outb(base + UART_DLM, 0x00); /* 115200 baud hi */
    outb(base + UART_LCR, UART_LCR_8BIT); /* 8 bits, no parity, 1 stop */
    outb(base + UART_FCR, UART_FCR_ENABLE | UART_FCR_RXCLR |
         UART_FCR_TXCLR | UART_FCR_TRIG1);
    outb(base + UART_MCR, 0x0B); /* IRQs enabled, RTS/DSR set */
}

int serial_port_init(int port_idx) {
    if (port_idx < 0 || port_idx >= SERIAL_PORTS_MAX) return -1;

    struct serial_port_state *port = &g_ports[port_idx];
    if (port->initialised) return 0;

    uart_init_port(port->base);
    port->rx_head = 0;
    port->rx_tail = 0;
    port->irq_enabled = 0;
    port->initialised = 1;

    return 0;
}

void __init serial_init(void) {
    serial_port_init(0); /* COM1 */
}

/* ── Port I/O ──────────────────────────────────────────────────────── */

void serial_port_write(int port_idx, char c) {
    if (port_idx < 0 || port_idx >= SERIAL_PORTS_MAX) return;
    if (!g_ports[port_idx].initialised) return;

    uint16_t base = g_ports[port_idx].base;
    outb(base, c);
}

void serial_port_write_str(int port_idx, const char *str) {
    while (str && *str) {
        if (*str == '\n') serial_port_write(port_idx, '\r');
        serial_port_write(port_idx, *str++);
    }
}

void serial_putchar(char c) {
    serial_port_write(0, c);
}

void serial_write(const char *str) {
    serial_port_write_str(0, str);
}

int serial_readable(void) {
    return inb(SERIAL_COM1 + UART_LSR) & UART_LSR_DR;
}

char serial_getchar(void) {
    int timeout = 10000000;
    while (!serial_readable() && --timeout > 0)
        __asm__ volatile("pause");
    return timeout > 0 ? (char)inb(SERIAL_COM1) : 0;
}

void serial_read_line(char *buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c = serial_getchar();
        if (c == '\r' || c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = 0;
}

/* ── IRQ mode ─────────────────────────────────────────────────────── */

int serial_set_irq_mode(int port_idx, int enable) {
    if (port_idx < 0 || port_idx >= SERIAL_PORTS_MAX) return -1;
    if (!g_ports[port_idx].initialised) return -1;

    struct serial_port_state *port = &g_ports[port_idx];

    if (enable) {
        /* Register IRQ handlers on first enable */
        static int irq3_registered = 0, irq4_registered = 0;

        if (port->irq_num == 4 && !irq4_registered) {
            idt_register_handler_named(36, irq4_handler, "serial_com1");  /* IRQ4 = vector 36 */
            if (apic_is_init_complete()) {
                ioapic_unmask_irq(4);
            }
            pic_unmask(4);
            irq4_registered = 1;
        }
        if (port->irq_num == 3 && !irq3_registered) {
            idt_register_handler_named(35, irq3_handler, "serial_com2");  /* IRQ3 = vector 35 */
            if (apic_is_init_complete()) {
                ioapic_unmask_irq(3);
            }
            pic_unmask(3);
            irq3_registered = 1;
        }

        /* Enable received data available interrupt */
        outb(port->base + UART_IER, UART_IER_RX);
        port->irq_enabled = 1;
    } else {
        outb(port->base + UART_IER, 0x00);
        port->irq_enabled = 0;
    }

    return 0;
}

char serial_read_irq(int port_idx) {
    if (port_idx < 0 || port_idx >= SERIAL_PORTS_MAX) return 0;
    struct serial_port_state *port = &g_ports[port_idx];

    if (port->rx_head == port->rx_tail) return 0;

    char c = port->rx_buffer[port->rx_tail];
    port->rx_tail = (port->rx_tail + 1) % (int)sizeof(port->rx_buffer);
    return c;
}

int serial_has_irq(int port_idx) {
    if (port_idx < 0 || port_idx >= SERIAL_PORTS_MAX) return 0;
    return g_ports[port_idx].rx_head != g_ports[port_idx].rx_tail;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Early serial console — works before full serial driver init
 *
 *  These functions use hardcoded COM1 (0x3F8) port I/O and require no
 *  kernel state (no struct serial_port_state, no spinlocks, no heap).
 *  They are safe to call from the very first instruction in kernel_main,
 *  before VGA init, before PMM, before anything.
 *
 *  This is essential for debugging early boot crashes that would otherwise
 *  be completely silent.
 * ═══════════════════════════════════════════════════════════════════════ */

/* Wait for the transmitter holding register to be empty (THRE bit).
 * Returns when the UART is ready to accept a new character. */
static inline void early_tx_wait(void)
{
    for (volatile int timeout = 0; timeout < 100000; timeout++) {
        if (inb(SERIAL_COM1 + UART_LSR) & UART_LSR_THRE)
            return;
        __asm__ volatile("pause");
    }
}

/* Minimal UART initialisation — 115200 baud, 8N1, no FIFO.
 * Unlike serial_port_init(), this doesn't touch any kernel data structures
 * and can be called before BSS is cleared (as long as we use stack locals). */
void early_serial_init(void)
{
    /* Set baud rate: divisor = 115200 / 115200 = 1 */
    outb(SERIAL_COM1 + UART_LCR, UART_LCR_DLAB);  /* enable DLAB */
    outb(SERIAL_COM1 + UART_DLL, 0x01);             /* 115200 baud lo */
    outb(SERIAL_COM1 + UART_DLM, 0x00);             /* 115200 baud hi */

    /* Line control: 8 bits, no parity, 1 stop bit */
    outb(SERIAL_COM1 + UART_LCR, UART_LCR_8BIT);

    /* FIFO: disable (keep it simple for early boot) */
    outb(SERIAL_COM1 + UART_FCR, 0x00);

    /* Modem control: set DTR/RTS (required for QEMU serial to work) */
    outb(SERIAL_COM1 + UART_MCR, 0x03);

    /* Flush any stale byte from the receiver */
    (void)inb(SERIAL_COM1 + UART_RBR);
}

/* Write a single character to the early serial port.
 * Translates '\n' to '\r\n' for proper line endings. */
void early_putchar(char c)
{
    if (c == '\n')
        early_putchar('\r');

    early_tx_wait();
    outb(SERIAL_COM1 + UART_THR, (uint8_t)c);
}

/* Write a null-terminated ASCII string to the early serial port. */
void early_printascii(const char *s)
{
    if (!s) return;
    while (*s)
        early_putchar(*s++);
}

/* Write a 64-bit unsigned integer as "0x" + 16 hex digits.
 * Example: early_printhex(0xDEAD) → prints "0x000000000000DEAD" */
void early_printhex(uint64_t val)
{
    const char hex_chars[] = "0123456789ABCDEF";
    char buf[19]; /* "0x" + 16 digits + NUL */
    int i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++) {
        buf[2 + i] = hex_chars[(val >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\0';

    early_printascii(buf);
}

/* Write a 64-bit unsigned integer as decimal digits. */
void early_printdec(uint64_t val)
{
    char buf[21]; /* max 20 digits for 2^64-1 + NUL */
    int pos = 20;

    buf[20] = '\0';

    if (val == 0) {
        early_putchar('0');
        return;
    }

    while (val > 0 && pos > 0) {
        pos--;
        buf[pos] = (char)('0' + (val % 10));
        val /= 10;
    }

    early_printascii(&buf[pos]);
}

/* ── Open a serial port ──────────────────────────────── */
static int serial_open(int port)
{
    if (port < 0 || port >= SERIAL_PORTS_MAX)
        return -EINVAL;
    return serial_port_init(port);
}

/* ── Close a serial port ────────────────────────────── */
static int serial_close(int port)
{
    (void)port;
    return 0;
}

/* ── Set baud rate using DLAB divisor ───────────────── */
static int serial_set_baud(int port, int baud)
{
    if (port < 0 || port >= SERIAL_PORTS_MAX)
        return -EINVAL;
    if (baud <= 0)
        return -EINVAL;

    uint16_t base = g_ports[port].base;
    /* Divisor = 115200 / baud */
    uint16_t divisor = (uint16_t)(115200 / (uint32_t)baud);
    if (divisor == 0) divisor = 1;

    outb(base + UART_LCR, UART_LCR_DLAB);  /* enable DLAB */
    outb(base + UART_DLL, (uint8_t)(divisor & 0xFF));
    outb(base + UART_DLM, (uint8_t)(divisor >> 8));
    /* Restore line control to 8N1 */
    outb(base + UART_LCR, UART_LCR_8BIT);

    return 0;
}

/* ── Set line parameters (bits, parity, stop) ───────── */
static int serial_set_params(int port, int bits, int parity, int stop)
{
    if (port < 0 || port >= SERIAL_PORTS_MAX)
        return -EINVAL;

    uint8_t lcr = 0;

    /* Data bits */
    if (bits == 5) lcr |= 0;
    else if (bits == 6) lcr |= 1;
    else if (bits == 7) lcr |= 2;
    else if (bits == 8) lcr |= 3;
    else return -EINVAL;

    /* Parity: 0=none, 1=odd, 2=even, 3=mark, 4=space */
    switch (parity) {
    case 0: break; /* none */
    case 1: lcr |= 0x08; break; /* odd */
    case 2: lcr |= 0x18; break; /* even */
    case 3: lcr |= 0x28; break; /* mark */
    case 4: lcr |= 0x38; break; /* space */
    default: return -EINVAL;
    }

    /* Stop bits: 1 or 2 */
    if (stop == 2) lcr |= 0x04;
    else if (stop != 1) return -EINVAL;

    outb(g_ports[port].base + UART_LCR, lcr);
    return 0;
}
