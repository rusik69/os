#ifndef EARLY_SERIAL_H
#define EARLY_SERIAL_H

/*
 * early_serial.h — Early serial console (pre-MMU, pre-interrupts)
 *
 * These functions work before the MMU, before the kernel memory
 * allocator, and before interrupts are enabled.  They use only
 * I/O port instructions (in/out) and require no kernel state.
 *
 * Call early_serial_init() as the very first thing in kernel_main,
 * before any other subsystem initialisation.
 *
 * If src/drivers/serial.c already provides early serial support
 * (early_serial_init, early_putchar, early_printascii), this header
 * provides the alternate API with early_printf support.
 *
 * Functions:
 *   early_serial_init(port)  — Initialise UART at given port (0x3F8, 0x2F8, etc.)
 *   early_putc(c)            — Write a single character
 *   early_puts(s)            — Write a null-terminated string
 *   early_printf(fmt, ...)   — Minimal printf (%s, %d, %x, %c, %% support)
 */

#include "types.h"

/*
 * early_serial_init — Minimal UART 16550 initialisation.
 *
 * @port: I/O port base address for the UART (e.g. 0x3F8 for COM1).
 *        Configures 115200 baud, 8 data bits, no parity, 1 stop bit.
 *        No FIFO, no interrupts.  Safe to call before BSS is clear.
 *
 * NOTE: If src/drivers/serial.c is also linked, its early_serial_init(void)
 * variant (hardcoded COM1) may conflict.  Use early_serial_init_port()
 * from this module for explicit port selection, or call the serial.c
 * version for the default COM1 init.
 */
void early_serial_init_port(uint16_t port);

/* Convenience wrapper: init COM1 (0x3F8) using the early serial module */
static inline void early_serial_init(void) {
    early_serial_init_port(0x3F8);
}

/*
 * early_putc — Write a single character to the early serial port.
 *
 * Translates '\n' to "\r\n" for proper line endings.
 * Busy-waits for the transmitter to be ready (THRE bit).
 */
void early_putc(char c);

/*
 * early_puts — Write a null-terminated string.
 *
 * Calls early_putc() for each character.  Safe to call with NULL.
 */
void early_puts(const char *s);

/*
 * early_printf — Minimal printf for early serial console.
 *
 * Supports the following format specifiers:
 *   %s  — null-terminated string
 *   %d  — signed 64-bit integer printed as decimal
 *   %x  — unsigned 64-bit integer printed as hex (lowercase)
 *   %c  — single character
 *   %%  — literal percent sign
 *
 * No field widths, no precision, no flags.  Designed for debugging
 * early boot without requiring any kernel infrastructure.
 */
void early_printf(const char *fmt, ...);

#endif /* EARLY_SERIAL_H */
