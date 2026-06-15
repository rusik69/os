#ifndef KVM_H
#define KVM_H

#include "types.h"

/* ── KVM API ioctl numbers ─────────────────────────────────────────── */
#define KVM_GET_API_VERSION        _IO(0xAE, 0x00)
#define KVM_CREATE_VM              _IO(0xAE, 0x01)
#define KVM_GET_MSR_INDEX_LIST     _IO(0xAE, 0x02)
#define KVM_CHECK_EXTENSION        _IO(0xAE, 0x03)
#define KVM_CREATE_VCPU            _IO(0xAE, 0x41)
#define KVM_SET_USER_MEMORY_REGION _IOW(0xAE, 0x46, struct kvm_userspace_memory_region)
#define KVM_RUN                    _IO(0xAE, 0x80)
#define KVM_GET_REGS               _IOR(0xAE, 0x81, struct kvm_regs)
#define KVM_SET_REGS               _IOW(0xAE, 0x82, struct kvm_regs)
#define KVM_GET_SREGS              _IOR(0xAE, 0x83, struct kvm_sregs)
#define KVM_SET_SREGS              _IOW(0xAE, 0x84, struct kvm_sregs)

/* Helper macros for ioctl encoding */
#ifndef _IO
#define _IO(type, nr)        (((type) << 8) | (nr))
#endif
#ifndef _IOR
#define _IOR(type, nr, size) (((type) << 8) | (nr) | 0x80000000UL)
#endif
#ifndef _IOW
#define _IOW(type, nr, size) (((type) << 8) | (nr) | 0x40000000UL)
#endif

/* ── KVM API version ───────────────────────────────────────────────── */
#define KVM_API_VERSION 12

/* ── KVM extension identifiers ─────────────────────────────────────── */
#define KVM_CAP_IRQCHIP       0
#define KVM_CAP_HLT           1
#define KVM_CAP_MMU           2
#define KVM_CAP_USER_MEMORY   3
#define KVM_CAP_SET_TSS_ADDR  4
#define KVM_CAP_VAPIC         6
#define KVM_CAP_EXT_CPUID     7
#define KVM_CAP_CLOCKSOURCE   8
#define KVM_CAP_NR_VCPUS      9
#define KVM_CAP_NR_MEMSLOTS   10
#define KVM_CAP_PIT           11
#define KVM_CAP_NOP_IO_DELAY  12
#define KVM_CAP_PV_MMU        13
#define KVM_CAP_MP_STATE      14
#define KVM_CAP_COALESCED_MMIO 15
#define KVM_CAP_SYNC_MMU      16
#define KVM_CAP_IOMMU         18
#define KVM_CAP_ONE_REG       162

/* ── VM exit reasons ────────────────────────────────────────────────── */
#define VM_EXIT_REASON_EPT_VIOLATION   48
#define VM_EXIT_REASON_IO              30
#define VM_EXIT_REASON_HLT             12
#define VM_EXIT_REASON_VMCALL          18
#define VM_EXIT_REASON_MSR_READ        31
#define VM_EXIT_REASON_MSR_WRITE       32
#define VM_EXIT_REASON_CR_ACCESS       28
#define VM_EXIT_REASON_EXTERNAL_INT    1
#define VM_EXIT_REASON_TRIPLE_FAULT    2

/* ── Exit reason strings for diagnostics ────────────────────────────── */
struct vm_exit_info {
    uint32_t reason;
    uint64_t qualification;
    uint64_t guest_rip;
    uint64_t guest_rsp;
};

/* ── KVM userspace memory region ────────────────────────────────────── */
struct kvm_userspace_memory_region {
    uint32_t slot;
    uint32_t flags;
    uint64_t guest_phys_addr;
    uint64_t memory_size;
    uint64_t userspace_addr; /* host virtual address */
};

/* ── KVM register layout ────────────────────────────────────────────── */
struct kvm_regs {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rsp, rbp;
    uint64_t r8,  r9,  r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
};

struct kvm_segment {
    uint64_t base;
    uint32_t limit;
    uint16_t selector;
    uint8_t  type;
    uint8_t  present;
    uint8_t  dpl;
    uint8_t  db;
    uint8_t  s;
    uint8_t  l;
    uint8_t  g;
    uint8_t  avl;
    uint8_t  unusable;
    uint8_t  padding;
};

struct kvm_dtable {
    uint64_t base;
    uint16_t limit;
    uint16_t padding[3];
};

struct kvm_sregs {
    struct kvm_segment cs, ds, es, fs, gs, ss;
    struct kvm_segment tr, ldt;
    struct kvm_dtable gdt, idt;
    uint64_t cr0, cr2, cr3, cr4, cr8;
    uint64_t efer;
    uint64_t apic_base;
    uint64_t interrupt_bitmap[4];
};

/* ── KVM run structure (returned from KVM_RUN) ──────────────────────── */
struct kvm_run {
    /* Input to KVM_RUN */
    uint8_t request_interrupt_window;
    uint8_t immediate_exit;
    uint8_t padding[6];

    /* Output from KVM_RUN */
    uint32_t exit_reason;
    uint32_t exit_reason_ext;
    uint64_t rip;

    union {
        struct {
            uint64_t qualification;
            uint32_t instr_len;
            uint32_t instr_info;
            uint32_t error_code;
        } ept_violation;

        struct {
            uint8_t  direction;
            uint8_t  size;
            uint16_t port;
            uint32_t count;
            uint64_t data;
        } io;

        struct {
            uint32_t nr;
            uint64_t data;
        } hlt;
    };
};

/* ── CPUID helpers ──────────────────────────────────────────────────── */
static inline void cpuidex(uint32_t leaf, uint32_t subleaf,
                           uint32_t *eax, uint32_t *ebx,
                           uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(subleaf));
}

/* Check for VMX support (CPUID.1.ECX bit 5) */
static inline int cpu_has_vmx(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuidex(1, 0, &eax, &ebx, &ecx, &edx);
    return (ecx & (1u << 5)) != 0;
}

/* Check for SVM support (CPUID.1.ECX bit 2) */
static inline int cpu_has_svm(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuidex(1, 0, &eax, &ebx, &ecx, &edx);
    return (ecx & (1u << 2)) != 0;
}

/* ── Public API ─────────────────────────────────────────────────────── */
int kvm_init(void);
void kvm_run(void);

#endif /* KVM_H */
