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

        /* Check for kernel stack overflow via guard page access */
        struct process *pt = process_get_table();
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (pt[i].guard_page &&
                (cr2 & ~(uint64_t)0xFFF) == (pt[i].guard_page & ~(uint64_t)0xFFF)) {
                kprintf("*** KERNEL STACK OVERFLOW *** pid=%u name=%s\n",
                        pt[i].pid, pt[i].name ? pt[i].name : "?");
                break;
            }
        }
        kprintf("CR2=0x%llx  error=0x%llx  (PF: %s %s %s)\n", cr2, err,
                (err & 1) ? "prot" : "np",
                (err & 2) ? "wr" : "rd",
                (err & 4) ? "usr" : "sup");
        kprintf("RIP=0x%llx  RSP=0x%llx  RBP=0x%llx\n",
                frame->rip, frame->rsp, frame->rbp);
        kprintf("RAX=0x%llx  RBX=0x%llx  RCX=0x%llx  RDX=0x%llx\n",
                frame->rax, frame->rbx, frame->rcx, frame->rdx);
        kprintf("RSI=0x%llx  RDI=0x%llx  R8=0x%llx   R9=0x%llx\n",
                frame->rsi, frame->rdi, frame->r8, frame->r9);
        kprintf("R10=0x%llx  R11=0x%llx  R12=0x%llx  R13=0x%llx\n",
                frame->r10, frame->r11, frame->r12, frame->r13);
        kprintf("R14=0x%llx  R15=0x%llx\n", frame->r14, frame->r15);
        kprintf("CS=0x%llx  SS=0x%llx  RFLAGS=0x%llx\n",
                frame->cs, frame->ss, frame->rflags);
        arch_print_backtrace();
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
    }

    /* User-mode write fault — check for COW */
    if ((err & (1ULL << 1))) {
        struct process *proc = process_get_current();
        if (proc && proc->pml4 && vmm_handle_cow_fault(proc->pml4, cr2)) {
            if (proc) proc->minflt++;
            return; /* handled */
        }
    }

    /* Unhandled user fault: kill the process with SIGSEGV (code 11) */
    struct process *proc = process_get_current();
    kprintf("[fault] SIGSEGV pid=%u addr=0x%llx err=0x%llx rip=0x%llx\n",
            proc ? (unsigned int)proc->pid : 0, cr2, err, frame->rip);
    process_exit_code(11); /* SIGSEGV = 11 — does not return */
}

void fault_init(void) {
    idt_register_handler(14, page_fault_handler);
}

/* ── Frame-pointer-based backtrace ──────────────────────────── */

#include "io.h"
#include "printf.h"
#include "process.h"

/* Print a backtrace by walking the RBP-linked frame pointer chain.
 * Each frame: [rbp+0] = previous rbp, [rbp+8] = return address.
 */
void arch_print_backtrace(void) {
    uint64_t rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));

    /* Determine stack bounds from current process if available */
    uint64_t stack_low = 0, stack_high = ~0ULL;
    struct process *proc = process_get_current();
    if (proc) {
        stack_low  = proc->kernel_stack;
        stack_high = proc->stack_top;
    }

    kprintf("Backtrace:\n");
    for (int i = 0; i < 32; i++) {
        /* Validate frame pointer is in reasonable kernel range */
        if (rbp < 0xFFFF800000000000ULL || rbp >= 0xFFFFFFFFFFFFFFFFULL)
            break;
        /* Validate within kernel stack if we know bounds */
        if (stack_low && (rbp < stack_low || rbp >= stack_high))
            break;
        /* Check alignment */
        if (rbp & 0xF)
            break;

        uint64_t *frame = (uint64_t *)rbp;
        uint64_t ret_addr = frame[1];
        if (ret_addr == 0)
            break;

        kprintf("  [%02d] 0x%llx\n", i, ret_addr);

        rbp = frame[0];
    }
}

/* ── kpanic — print a formatted message then halt ─────────────────── */

void kpanic(const char *fmt, ...) {
    __builtin_va_list ap;
    __asm__ volatile("cli");
    kprintf("\n*** KERNEL PANIC ***\n");
    __builtin_va_start(ap, fmt);
    vkprintf(fmt, ap);
    __builtin_va_end(ap);
    kprintf("\n");
    kprintf("CR0=0x%llx  CR2=0x%llx  CR3=0x%llx  CR4=0x%llx\n",
            read_cr0(), read_cr2(), read_cr3(), read_cr4());
    arch_print_backtrace();
    for (;;) __asm__ volatile("hlt");
    __builtin_unreachable();
}
