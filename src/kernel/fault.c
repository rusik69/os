#include "fault.h"
#include "idt.h"
#include "vmm.h"
#include "process.h"
#include "printf.h"
#include "nx_enforce.h"

/* Add vmm.h inclusion for vm_pgfault counter - already present via vmm.h */

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

/* Page fault tracing flag */
int page_fault_trace = 0;

void page_fault_trace_enable(int enable) {
    page_fault_trace = enable;
}

/* Kernel stack depth check: verify RSP is within the current process's kernel stack */
int check_kernel_stack_depth(void) {
    struct process *proc = process_get_current();
    if (!proc) return 1; /* no process context, assume OK */
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    /* Kernel stack is between kernel_stack and stack_top */
    if (rsp < proc->kernel_stack || rsp >= proc->stack_top) {
        kprintf("*** KERNEL STACK OVERFLOW DETECTED *** pid=%u name=%s "
                "rsp=0x%llx stack_base=0x%llx stack_top=0x%llx\n",
                proc->pid, proc->name ? proc->name : "?",
                rsp, proc->kernel_stack, proc->stack_top);
        return 0;
    }
    return 1;
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

    /* Update vmstat counters */
    vm_pgfault++;
    if (err & (1ULL << 1) && (err & (1ULL << 2))) {
        /* User write fault — could be COW (minor) or major */
        /* We don't have a way to differentiate yet; count as minor for now */
    }

    /* Page fault tracing */
    if (page_fault_trace) {
        struct process *proc = process_get_current();
        kprintf("[PFTRACE] addr=0x%llx err=0x%llx (%s %s %s %s) rip=0x%llx pid=%u name=%s\n",
                cr2, err,
                (err & 1) ? "prot" : "np",
                (err & 2) ? "wr" : "rd",
                (err & 4) ? "usr" : "sup",
                (err & 16) ? "if" : "",
                frame->rip,
                proc ? (unsigned int)proc->pid : 0,
                proc && proc->name ? proc->name : "?");
    }

    /* Check for NX violation (instruction fetch on a non-executable page) */
    if ((err & (1ULL << 4)) && (err & 1ULL)) {
        if (nx_enforce_check_fault(cr2, err, frame)) {
            /* NX violation was handled (kernel: panic, user: SIGSEGV) */
            return;
        }
    }

    /* Kernel-mode fault: panic with register dump */
    if (!(err & (1ULL << 2))) {
        kprintf("\n*** KERNEL PAGE FAULT ***\n");

        /* Check for kernel stack overflow via guard page access */
        struct process *pt = process_get_table();
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (pt[i].guard_page &&
                (cr2 & ~(uint64_t)0xFFF) == (pt[i].guard_page & ~(uint64_t)0xFFF)) {
                kprintf("*** KERNEL STACK OVERFLOW DETECTED! Process: %s (pid=%u) ***\n",
                        pt[i].name ? pt[i].name : "?",
                        (unsigned int)pt[i].pid);
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

/* ── Double-fault handler (#DF, vector 8) ────────────────────────── */
/* Runs on a dedicated IST stack, safe even when the regular kernel
 * stack has overflowed.  Prints full register dump and halts. */
static void double_fault_handler(struct interrupt_frame *frame) {
    kprintf("\n*** DOUBLE FAULT (#DF) ***\n");
    kprintf("Error code: 0x%lx\n", (unsigned long)frame->error_code);
    kprintf("RIP: 0x%lx  RSP: 0x%lx  RBP: 0x%lx\n",
            (unsigned long)frame->rip, (unsigned long)frame->rsp,
            (unsigned long)frame->rbp);
    kprintf("RAX: 0x%lx  RBX: 0x%lx  RCX: 0x%lx  RDX: 0x%lx\n",
            (unsigned long)frame->rax, (unsigned long)frame->rbx,
            (unsigned long)frame->rcx, (unsigned long)frame->rdx);
    kprintf("RSI: 0x%lx  RDI: 0x%lx\n",
            (unsigned long)frame->rsi, (unsigned long)frame->rdi);
    kprintf("R8: 0x%lx   R9: 0x%lx   R10: 0x%lx  R11: 0x%lx\n",
            (unsigned long)frame->r8, (unsigned long)frame->r9,
            (unsigned long)frame->r10, (unsigned long)frame->r11);
    kprintf("R12: 0x%lx  R13: 0x%lx  R14: 0x%lx  R15: 0x%lx\n",
            (unsigned long)frame->r12, (unsigned long)frame->r13,
            (unsigned long)frame->r14, (unsigned long)frame->r15);
    kprintf("CS: 0x%lx  SS: 0x%lx  RFLAGS: 0x%lx\n",
            (unsigned long)frame->cs, (unsigned long)frame->ss,
            (unsigned long)frame->rflags);
    arch_print_backtrace();
    kprintf("*** SYSTEM HALTED (double fault, cannot recover) ***\n");
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
    __builtin_unreachable();
}

void fault_init(void) {
    idt_register_handler(14, page_fault_handler);
    idt_register_handler(8, double_fault_handler);
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

/* ── Per-task stack usage ─────────────────────────────────────── */

uint64_t task_stack_usage(struct process *p) {
    if (!p || p->kernel_stack == 0 || p->stack_top == 0) return 0;
    /* Read current RSP */
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    /* If we're not running in this process's context, try to estimate */
    struct process *cur = process_get_current();
    if (cur != p) {
        /* Return last known watermark */
        return p->stack_watermark;
    }
    /* Stack usage = current stack top - current RSP */
    uint64_t used = p->stack_top - rsp;
    /* Update watermark (lowest RSP = deepest stack usage) */
    if (rsp < p->stack_watermark || p->stack_watermark == 0)
        p->stack_watermark = rsp;
    return used;
}

/* Update stack watermark on context switch - called from scheduler */
void task_update_stack_watermark(struct process *p) {
    if (!p) return;
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    if (rsp < p->stack_watermark || p->stack_watermark == 0)
        p->stack_watermark = rsp;
}
