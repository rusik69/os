#ifndef GDT_H
#define GDT_H

#include "types.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_entry_high {
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void gdt_init(void);
void tss_set_rsp0(uint64_t rsp0);

extern void gdt_load(struct gdt_pointer *ptr, uint16_t code_seg, uint16_t data_seg);
extern void tss_load(uint16_t selector);

#endif
