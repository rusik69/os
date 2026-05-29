#include "fault.h"
#include "idt.h"
#include "vmm.h"
#include "process.h"
#include "printf.h"

static inline uint64_t read_cr2(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr0(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr4(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr4, %0" : "=r"(val));
    return val;
}

/*
 * Page fault handler (ISR 14).
 *
 * error_code bits:
 *   bit 0 = PRESENT   (0 = not-present, 1 = protection fault)
 *   bit 1 = WRITE     (0 = read, 1 = write)
 *   bit 2 = USER      (0 = kernel, 1 = user-mode access)
 *
 * CR2 holds the faulting virtual address.
 */
static void page_fault_handler(struct interrupt_frame *frame) {
    uint64_t cr2 = read_cr2();
    uint64_t err = frame->error_code;

    /* Kernel-mode fault: panic with register dump */
    if (!(err & (1ULL << 2))) {
        kprintf("\n*** KERNEL PAGE FAULT ***\n");
        kprintf("CR2=0x%x  error=0x%x  (PF: %s %s %s)\n", cr2, err,
                (err & 1) ? "prot" : "np",
                (err & 2) ? "wr" : "rd",
                (err & 4) ? "usr" : "sup");
        kprintf("RIP=0x%x  RSP=0x%x  RBP=0x%x\n",
                frame->rip, frame->rsp, frame->rbp);
        kprintf("RAX=0x%x  RBX=0x%x  RCX=0x%x  RDX=0x%x\n",
                frame->rax, frame->rbx, frame->rcx, frame->rdx);
        kprintf("RSI=0x%x  RDI=0x%x  R8=0x%x   R9=0x%x\n",
                frame->rsi, frame->rdi, frame->r8, frame->r9);
        kprintf("R10=0x%x  R11=0x%x  R12=0x%x  R13=0x%x\n",
                frame->r10, frame->r11, frame->r12, frame->r13);
        kprintf("R14=0x%x  R15=0x%x\n", frame->r14, frame->r15);
        kprintf("CS=0x%x  SS=0x%x  RFLAGS=0x%x\n",
                frame->cs, frame->ss, frame->rflags);
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
    }

    /* User-mode write fault — check for COW */
    if ((err & (1ULL << 1))) {
        struct process *proc = process_get_current();
        if (proc && proc->pml4 && vmm_handle_cow_fault(proc->pml4, cr2))
            return; /* handled */
    }

    /* Unhandled user fault: kill the process with SIGSEGV (code 11) */
    struct process *proc = process_get_current();
    kprintf("[fault] SIGSEGV pid=%u addr=0x%x err=0x%x rip=0x%x\n",
            proc ? (uint64_t)proc->pid : 0ULL, cr2, err, frame->rip);
    process_exit_code(11); /* SIGSEGV = 11 — does not return */
}

void fault_init(void) {
    idt_register_handler(14, page_fault_handler);
}

/* ── kpanic — print a formatted message then halt ─────────────────── */

#include "io.h"
#include "printf.h"

void kpanic(const char *fmt, ...) {
    __builtin_va_list ap;
    __asm__ volatile("cli");
    kprintf("\n*** KERNEL PANIC ***\n");
    __builtin_va_start(ap, fmt);
    vkprintf(fmt, ap);
    __builtin_va_end(ap);
    kprintf("\n");
    kprintf("CR0=0x%x  CR2=0x%x  CR3=0x%x  CR4=0x%x\n",
            read_cr0(), read_cr2(), read_cr3(), read_cr4());
    for (;;) __asm__ volatile("hlt");
    __builtin_unreachable();
}
