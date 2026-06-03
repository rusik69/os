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

void serial_init(void) {
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
