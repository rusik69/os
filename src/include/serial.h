#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

/* Standard COM port base addresses */
#define SERIAL_COM1 0x3F8
#define SERIAL_COM2 0x2F8
#define SERIAL_COM3 0x3E8
#define SERIAL_COM4 0x2E8

#define SERIAL_PORTS_MAX 4

/* UART registers (offset from port base) */
#define UART_RBR  0   /* Receive Buffer Register (DLAB=0, read) */
#define UART_THR  0   /* Transmit Holding Register (DLAB=0, write) */
#define UART_IER  1   /* Interrupt Enable Register */
#define UART_IIR  2   /* Interrupt Identification Register */
#define UART_FCR  2   /* FIFO Control Register */
#define UART_LCR  3   /* Line Control Register */
#define UART_MCR  4   /* Modem Control Register */
#define UART_LSR  5   /* Line Status Register */
#define UART_MSR  6   /* Modem Status Register */
#define UART_DLL  0   /* Divisor Latch Low (DLAB=1) */
#define UART_DLM  1   /* Divisor Latch High (DLAB=1) */

/* UART flags */
#define UART_IER_RX     (1U << 0)  /* Enable Received Data Available Interrupt */
#define UART_IER_TX     (1U << 1)  /* Enable Transmitter Holding Register Empty Interrupt */
#define UART_IER_LINE   (1U << 2)  /* Enable Receiver Line Status Interrupt */
#define UART_IER_MS     (1U << 3)  /* Enable Modem Status Interrupt */
#define UART_LSR_DR     (1U << 0)  /* Data Ready */
#define UART_LSR_THRE   (1U << 5)  /* Transmitter Holding Register Empty */
#define UART_FCR_ENABLE (1U << 0)  /* FIFO Enable */
#define UART_FCR_RXCLR  (1U << 1)  /* Clear Receive FIFO */
#define UART_FCR_TXCLR  (1U << 2)  /* Clear Transmit FIFO */
#define UART_FCR_TRIG1  (0 << 6)  /* RX FIFO trigger level: 1 byte */
#define UART_FCR_TRIG4  (1U << 6)  /* RX FIFO trigger level: 4 bytes */
#define UART_FCR_TRIG8  (2 << 6)  /* RX FIFO trigger level: 8 bytes */
#define UART_FCR_TRIG14 (3 << 6)  /* RX FIFO trigger level: 14 bytes */
#define UART_LCR_8BIT   (3 << 0)  /* 8 data bits */
#define UART_LCR_DLAB   (1U << 7)  /* Divisor Latch Access Bit */

/* Initialise COM1 (original behaviour). */
void serial_init(void);

/* Initialise a specific serial port (port_idx 0..3 for COM1..COM4).
   Must be called before using that port. */
int  serial_port_init(int port_idx);

/* Write a single character to a specific serial port. */
void serial_port_write(int port_idx, char c);

/* Write a string to a specific serial port. */
void serial_port_write_str(int port_idx, const char *str);

/* Legacy: write 'c' to COM1. */
void serial_putchar(char c);

/* Legacy: write 'str' to COM1. */
void serial_write(const char *str);

/* Poll for data on COM1. */
int  serial_readable(void);

/* Read one char from COM1 (polling). */
int  serial_getchar(void);

/* Read a line from COM1. */
void serial_read_line(char *buf, int max);

/* ── Early serial (before full driver init) ──────────────────────────────
 *
 * These functions work at the very earliest boot stage, before the normal
 * serial_port_init() / serial_init() have been called.  They use direct
 * port I/O with hardcoded COM1 base address and require no allocated state.
 *
 * early_serial_init()   — minimal COM1 UART init (115200 8N1, no FIFO)
 * early_putchar(c)      — write one character to COM1 (busy-wait)
 * early_printascii(s)   — write null-terminated string
 * early_printhex(val)   — write 64-bit value as "0x" + 16 hex digits
 * early_printdec(val)   — write 64-bit value as decimal
 */

void early_serial_init(void);
void early_putchar(char c);
void early_printascii(const char *s);
void early_printhex(uint64_t val);
void early_printdec(uint64_t val);

/* ── IRQ mode ──────────────────────────────────────────────────────── */

/* Enable (1) or disable (0) interrupt-driven receive on a serial port.
   When enabled, the port will generate IRQ 4 (COM1/3) or IRQ 3 (COM2/4)
   on received data. */
int  serial_set_irq_mode(int port_idx, int enable);

/* Read a character from the IRQ-driven receive buffer for a port.
   Returns the byte (0-255) on success, or -1 if no data available. */
int  serial_read_irq(int port_idx);

/* Check if IRQ data is available on a port (returns non-zero if yes). */
int  serial_has_irq(int port_idx);

#endif /* SERIAL_H */
