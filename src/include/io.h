#ifndef IO_H
#define IO_H

#include "types.h"

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline void cli(void) {
    __asm__ volatile("cli");
}

static inline void sti(void) {
    __asm__ volatile("sti");
}

static inline void hlt(void) {
    __asm__ volatile("hlt");
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}


/* MMIO access */
static inline uint32_t readl(const volatile void *addr) { return *(const volatile uint32_t *)addr; }
static inline void writel(uint32_t v, volatile void *addr) { *(volatile uint32_t *)addr = v; }
static inline uint16_t readw(const volatile void *addr) { return *(const volatile uint16_t *)addr; }
static inline void writew(uint16_t v, volatile void *addr) { *(volatile uint16_t *)addr = v; }
static inline uint8_t readb(const volatile void *addr) { return *(const volatile uint8_t *)addr; }
static inline void writeb(uint8_t v, volatile void *addr) { *(volatile uint8_t *)addr = v; }
/* String I/O */
static inline void insl(uint16_t port, void *addr, int count) {
    __asm__ volatile("rep insl" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}
static inline void outsl(uint16_t port, const void *addr, int count) {
    __asm__ volatile("rep outsl" : "+S"(addr), "+c"(count) : "d"(port) : "memory");
}
#endif
