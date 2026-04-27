#include "pic.h"
#include "io.h"

void pic_init(void) {
    /* ICW1: begin initialization */
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();

    /* ICW2: vector offsets */
    outb(PIC1_DATA, IRQ_OFFSET);      io_wait(); /* IRQ 0-7  -> INT 32-39 */
    outb(PIC2_DATA, IRQ_OFFSET + 8);  io_wait(); /* IRQ 8-15 -> INT 40-47 */

    /* ICW3: cascading */
    outb(PIC1_DATA, 0x04); io_wait(); /* IRQ2 has slave */
    outb(PIC2_DATA, 0x02); io_wait(); /* slave cascade identity */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    /* Restore masks (mask all) */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

void pic_mask(uint8_t irq) {
    uint16_t port;
    if (irq < 8) { port = PIC1_DATA; }
    else { port = PIC2_DATA; irq -= 8; }
    outb(port, inb(port) | (1 << irq));
}

void pic_unmask(uint8_t irq) {
    uint16_t port;
    if (irq < 8) { port = PIC1_DATA; }
    else { port = PIC2_DATA; irq -= 8; }
    outb(port, inb(port) & ~(1 << irq));
}
