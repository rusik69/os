#ifndef PIC_H
#define PIC_H

#include "types.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define IRQ_OFFSET 32

void pic_init(void);
void pic_eoi(uint8_t irq);
void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);

#endif
