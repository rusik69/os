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

    /* Kernel-mode fault: panic */
    if (!(err & (1ULL << 2))) {
        kprintf("\n*** KERNEL PAGE FAULT ***\n");
        kprintf("CR2=0x%x  error=0x%x\n", cr2, err);
        kprintf("RIP=0x%x  RSP=0x%x  CS=0x%x\n", frame->rip, frame->rsp, frame->cs);
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
