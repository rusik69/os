#ifndef FAULT_H
#define FAULT_H

/* Register the page fault handler (ISR 14). Call once after idt_init(). */
void fault_init(void);

#endif
