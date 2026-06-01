/* page_poison.c — Dedicated page poisoning subsystem */

#include "page_poison.h"
#include "printf.h"

static uint8_t poison_freed_val = 0xDC;
static int poison_active = 1;

void page_poison_init(void) {
    /* This complements the poisoning already in pmm.c.
     * Provides an independent API for managing poison state. */
    poison_active = 1;
    poison_freed_val = 0xDC;
    kprintf("[mem] Page poisoning active (freed pages filled with 0x%x)\n",
            (unsigned int)poison_freed_val);
}

void page_poison_set_freed_value(uint8_t val) {
    poison_freed_val = val;
}

uint8_t page_poison_get_freed_value(void) {
    return poison_freed_val;
}

int page_poison_is_active(void) {
    return poison_active;
}

void poison_region(void *addr, size_t size) {
    if (!poison_active || !addr) return;
    uint8_t *bytes = (uint8_t *)addr;
    for (size_t i = 0; i < size; i++)
        bytes[i] = poison_freed_val;
}

int poison_check_region(const void *addr, size_t size, uint8_t poison_val) {
    if (!addr) return -1;
    const uint8_t *bytes = (const uint8_t *)addr;
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] != poison_val)
            return 1; /* corruption detected */
    }
    return 0; /* clean */
}
