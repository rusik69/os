#ifndef CPU_FEATURES_H
#define CPU_FEATURES_H

#include "types.h"

/* CR4 bits (extended) */
#define CR4_FSGSBASE    (1ULL << 16)  /* FSGSBASE instructions enable */
#define CR4_INVPCID     (1ULL << 10)  /* INVPCID instruction enable */
#define CR4_SMEP        (1ULL << 20)  /* Supervisor Mode Execution Prevention */
#define CR4_SMAP        (1ULL << 21)  /* Supervisor Mode Access Prevention */
#define CR4_UMIP        (1ULL << 11)  /* User-Mode Instruction Prevention */
#define CR4_PKE         (1ULL << 22)  /* Protection Key Enable */

/* CPUID leaf 7, EBX feature bits */
#define CPUID_7_EBX_FSGSBASE   (1 << 0)
#define CPUID_7_EBX_INVPCID    (1 << 10)
#define CPUID_7_EBX_RDPID      (1 << 22)

/* x2APIC MSR */
#define IA32_APIC_BASE          0x1B
#define IA32_APIC_BASE_X2APIC   (1ULL << 10)
#define IA32_APIC_BASE_ENABLE   (1ULL << 11)

/* TSC deadline MSR */
#define IA32_TSC_DEADLINE       0x6E0

/* FS/GS base MSRs */
#define MSR_FS_BASE             0xC0000100
#define MSR_GS_BASE             0xC0000101
#define MSR_KERNEL_GS_BASE      0xC0000102

/* TSC AUX MSR (used by RDPID) */
#define MSR_TSC_AUX             0xC0000103

/* Initialize all CPU features */
int cpu_features_init(void);

/* Individual feature inits — all return 0 on success, -1 on failure */
int smap_smep_init(void);
int umip_init(void);
int x2apic_init(void);
int tsc_deadline_init(void);
int invpcid_init(void);
int fsgsbase_init(void);
int rdpid_init(void);
int nx_enforce_init(void);

/* INVPCID wrappers */
void invpcid_flush_all(void);
void invpcid_flush_single(uint64_t addr);

/* FSGSBASE wrappers */
static inline uint64_t rdfsbase(void) {
    uint64_t val;
    __asm__ volatile("rdfsbase %0" : "=r"(val));
    return val;
}
static inline void wrfsbase(uint64_t val) {
    __asm__ volatile("wrfsbase %0" : : "r"(val) : "memory");
}
static inline uint64_t rdgsbase(void) {
    uint64_t val;
    __asm__ volatile("rdgsbase %0" : "=r"(val));
    return val;
}
static inline void wrgsbase(uint64_t val) {
    __asm__ volatile("wrgsbase %0" : : "r"(val) : "memory");
}

/* RDPID wrapper */
static inline uint32_t rdpid(void) {
    uint32_t val;
    __asm__ volatile(".byte 0xF3, 0x0F, 0xC7, 0xF8" : "=a"(val));
    return val;
}

#endif /* CPU_FEATURES_H */
