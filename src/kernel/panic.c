#define KERNEL_INTERNAL
#include "types.h"
#include "panic.h"
#include "printf.h"
#include "vga.h"
#include "io.h"
#include "process.h"
#include "smp.h"
#include "timer.h"
#include "acpi.h"
#include "sysrq.h"
#include "kallsyms.h"

/* Stack trace: walk frame pointers (RBP chain) using kallsyms */
void dump_stack(void) {
    uint64_t rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));

    kprintf("Call trace:\n");
    int frame = 0;
    while (rbp && rbp >= 0xFFFF800000000000ULL && frame < 32) {
        uint64_t ret_addr = ((uint64_t *)rbp)[1];
        if (ret_addr == 0) break;
        kprintf("  [<%016llx>] ", (unsigned long long)ret_addr);
        if (ret_addr < 0xFFFF800000000000ULL)
            kprintf("user-mode\n");
        else
            kprintf("%s\n", kallsyms_lookup(ret_addr));
        rbp = *(uint64_t *)rbp;
        frame++;
    }
    if (frame == 0)
        kprintf("  (no frame pointers in trace)\n");
}

void dump_regs(void) {
    uint64_t cr0, cr2, cr3, cr4, cr8;
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rsp, rbp, rip, rflags;
    uint64_t cs, ds, es, fs, gs, ss;

    __asm__ volatile("mov %%cr0, %0"  : "=r"(cr0));
    __asm__ volatile("mov %%cr2, %0"  : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0"  : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0"  : "=r"(cr4));
    __asm__ volatile("mov %%cr8, %0"  : "=r"(cr8));
    __asm__ volatile("mov %%rax, %0"  : "=r"(rax));
    __asm__ volatile("mov %%rbx, %0"  : "=r"(rbx));
    __asm__ volatile("mov %%rcx, %0"  : "=r"(rcx));
    __asm__ volatile("mov %%rdx, %0"  : "=r"(rdx));
    __asm__ volatile("mov %%rsi, %0"  : "=r"(rsi));
    __asm__ volatile("mov %%rdi, %0"  : "=r"(rdi));
    __asm__ volatile("mov %%r8,  %0"  : "=r"(r8));
    __asm__ volatile("mov %%r9,  %0"  : "=r"(r9));
    __asm__ volatile("mov %%r10, %0"  : "=r"(r10));
    __asm__ volatile("mov %%r11, %0"  : "=r"(r11));
    __asm__ volatile("mov %%r12, %0"  : "=r"(r12));
    __asm__ volatile("mov %%r13, %0"  : "=r"(r13));
    __asm__ volatile("mov %%r14, %0"  : "=r"(r14));
    __asm__ volatile("mov %%r15, %0"  : "=r"(r15));
    __asm__ volatile("mov %%rsp, %0"  : "=r"(rsp));
    __asm__ volatile("mov %%rbp, %0"  : "=r"(rbp));
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
    __asm__ volatile("mov %%cs,  %0"  : "=r"(cs));
    __asm__ volatile("mov %%ds,  %0"  : "=r"(ds));
    __asm__ volatile("mov %%es,  %0"  : "=r"(es));
    __asm__ volatile("mov %%fs,  %0"  : "=r"(fs));
    __asm__ volatile("mov %%gs,  %0"  : "=r"(gs));
    __asm__ volatile("mov %%ss,  %0"  : "=r"(ss));

    /* RIP via call trick */
    rip = (uint64_t)dump_regs;

    kprintf("=== REGISTER DUMP ===\n");
    kprintf(" RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n",
            (unsigned long long)rax, (unsigned long long)rbx,
            (unsigned long long)rcx, (unsigned long long)rdx);
    kprintf(" RSI=%016llx RDI=%016llx RBP=%016llx RSP=%016llx\n",
            (unsigned long long)rsi, (unsigned long long)rdi,
            (unsigned long long)rbp, (unsigned long long)rsp);
    kprintf(" R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx\n",
            (unsigned long long)r8,  (unsigned long long)r9,
            (unsigned long long)r10, (unsigned long long)r11);
    kprintf(" R12=%016llx R13=%016llx R14=%016llx R15=%016llx\n",
            (unsigned long long)r12, (unsigned long long)r13,
            (unsigned long long)r14, (unsigned long long)r15);
    kprintf(" RIP=%016llx RFL=%016llx\n",
            (unsigned long long)rip, (unsigned long long)rflags);
    kprintf(" CS=%04llx DS=%04llx ES=%04llx FS=%04llx GS=%04llx SS=%04llx\n",
            cs, ds, es, fs, gs, ss);
    kprintf(" CR0=%016llx CR2=%016llx CR3=%016llx CR4=%016llx CR8=%016llx\n",
            (unsigned long long)cr0, (unsigned long long)cr2,
            (unsigned long long)cr3, (unsigned long long)cr4,
            (unsigned long long)cr8);
}

__attribute__((noreturn))
void panic(const char *fmt, ...) {
    cli();

    vga_set_color(VGA_WHITE, VGA_RED);
    kprintf("\n\n=== KERNEL PANIC ===\n");

    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    vkprintf(fmt, args);
    __builtin_va_end(args);

    kprintf("\n");

    /* Try to dump CPU state */
    struct process *cur = process_get_current();
    if (cur) {
        kprintf("Process: %s (pid=%u, state=%u)\n",
                cur->name ? cur->name : "?", cur->pid, (uint32_t)cur->state);
    }

    if (smp_get_cpu_count() > 1) {
        kprintf("CPU: %d/%d\n", smp_get_cpu_id(), smp_get_cpu_count());
    }

    dump_regs();
    dump_stack();

    kprintf("=== SYSTEM HALTED ===\n");

    for (;;) hlt();
}

void panic_init(void) {
    kprintf("[OK] Panic/oops handler initialized\n");
}
