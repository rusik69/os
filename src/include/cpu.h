#ifndef CPU_H
#define CPU_H

#include "types.h"

/* CR4 bits */
#define CR4_PAE      (1ULL << 5)
#define CR4_PSE      (1ULL << 4)
#define CR4_SMEP     (1ULL << 20)  /* Supervisor Mode Execution Prevention */
#define CR4_SMAP     (1ULL << 21)  /* Supervisor Mode Access Prevention */
#define CR4_UMIP     (1ULL << 11)  /* User-Mode Instruction Prevention */

/* EFER bits */
#define EFER_SCE     (1ULL << 0)   /* Syscall Enable */
#define EFER_LME     (1ULL << 8)   /* Long Mode Enable */
#define EFER_LMA     (1ULL << 10)  /* Long Mode Active */
#define EFER_NXE     (1ULL << 11)  /* No-Execute Enable */

/* Read/write control registers */
static inline uint64_t read_cr0(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr0, %0" : "=r"(val));
    return val;
}
static inline void write_cr0(uint64_t val) {
    __asm__ volatile("mov %0, %%cr0" : : "r"(val) : "memory");
}
static inline uint64_t read_cr2(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}
static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}
static inline void write_cr3(uint64_t val) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(val) : "memory");
}
static inline uint64_t read_cr4(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr4, %0" : "=r"(val));
    return val;
}
static inline void write_cr4(uint64_t val) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(val) : "memory");
}

/* MSR access */
static inline uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void write_msr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

/* SMAP toggling: stac (Set AC Flag) / clac (Clear AC Flag)
 * These are no-ops if SMAP is not enabled in CR4. */
static inline void stac(void) {
    __asm__ volatile("stac" : : : "memory");
}
static inline void clac(void) {
    __asm__ volatile("clac" : : : "memory");
}

/* Initialize CPU security features (SMEP, SMAP, NXE, UMIP).
 * Called once during boot. Returns non-zero if features were enabled. */
int cpu_security_init(void);

#endif
