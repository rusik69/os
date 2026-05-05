#include "serial.h"
#include "io.h"

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00); /* Disable interrupts */
    outb(COM1 + 3, 0x80); /* Enable DLAB */
    outb(COM1 + 0, 0x03); /* 38400 baud lo */
    outb(COM1 + 1, 0x00); /* 38400 baud hi */
    outb(COM1 + 3, 0x03); /* 8 bits, no parity, 1 stop */
    outb(COM1 + 2, 0xC7); /* Enable FIFO */
    outb(COM1 + 4, 0x0B); /* IRQs enabled, RTS/DSR set */
}

void serial_putchar(char c) {
    while (!(inb(COM1 + 5) & 0x20));
    outb(COM1, c);
}

void serial_write(const char *str) {
    while (*str) {
        if (*str == '\n') serial_putchar('\r');
        serial_putchar(*str++);
    }
}

int serial_readable(void) {
    return inb(COM1 + 5) & 0x01;
}

char serial_getchar(void) {
    while (!serial_readable());
    return (char)inb(COM1);
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
