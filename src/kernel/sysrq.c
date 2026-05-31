#define KERNEL_INTERNAL
#include "types.h"
#include "sysrq.h"
#include "printf.h"
#include "process.h"
#include "scheduler.h"
#include "pmm.h"
#include "signal.h"
#include "acpi.h"
#include "vga.h"
#include "io.h"
#include "timer.h"
#include "oom.h"

void sysrq_handle_reboot(void);
void sysrq_handle_showregs(void);
void sysrq_handle_showtasks(void);
void sysrq_handle_showmem(void);
void sysrq_handle_killall(void);
void sysrq_handle_poweroff(void);
void sysrq_handle_sync(void);
void sysrq_handle_oom(void);

void sysrq_handle(char cmd) {
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    switch (cmd) {
        case 'b':
            kprintf("\n[SysRq] Emergency reboot\n");
            sysrq_handle_reboot();
            break;
        case 'p':
            kprintf("\n[SysRq] Registers:\n");
            sysrq_handle_showregs();
            break;
        case 't':
            kprintf("\n[SysRq] Task list:\n");
            sysrq_handle_showtasks();
            break;
        case 'm':
            kprintf("\n[SysRq] Memory info:\n");
            sysrq_handle_showmem();
            break;
        case 'i':
            kprintf("\n[SysRq] Send SIGKILL to all processes\n");
            sysrq_handle_killall();
            break;
        case 'o':
            kprintf("\n[SysRq] Emergency poweroff\n");
            sysrq_handle_poweroff();
            break;
        case 's':
            kprintf("\n[SysRq] Syncing filesystems\n");
            sysrq_handle_sync();
            break;
        case 'f':
            kprintf("\n[SysRq] Full OOM kill\n");
            sysrq_handle_oom();
            break;
        default:
            kprintf("\n[SysRq] Unknown command: '%c'\n", cmd);
            break;
    }
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

int sysrq_is_valid(char cmd) {
    const char *valid = "bptmikosf";
    for (const char *p = valid; *p; p++)
        if (*p == cmd) return 1;
    return 0;
}

void sysrq_handle_reboot(void) {
    /* Use ACPI reset or keyboard controller reset */
    __asm__ volatile("int $0x19"); /* BIOS reset — won't work in protected/long mode */
    /* Try ACPI reset */
    acpi_reboot();
    /* Triple fault reset */
    cli();
    for (;;) hlt();
}

void sysrq_handle_showregs(void) {
    uint64_t cr0, cr2, cr3, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    kprintf(" CR0=%016llx CR2=%016llx\n CR3=%016llx CR4=%016llx\n",
            (unsigned long long)cr0, (unsigned long long)cr2,
            (unsigned long long)cr3, (unsigned long long)cr4);
    uint64_t rsp, rbp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    kprintf(" RSP=%016llx RBP=%016llx\n", (unsigned long long)rsp, (unsigned long long)rbp);
}

void sysrq_handle_showtasks(void) {
    struct process *table = process_get_table();
    kprintf(" PID  STATE   NAME            USER CPU\n");
    kprintf(" ---  -----  --------------  ---- ---\n");
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED) continue;
        const char *state_str = "?";
        switch (table[i].state) {
            case PROCESS_READY:   state_str = "READY"; break;
            case PROCESS_RUNNING: state_str = "RUN"; break;
            case PROCESS_BLOCKED: state_str = "BLKD"; break;
            case PROCESS_ZOMBIE:  state_str = "ZOMB"; break;
            default: break;
        }
        kprintf(" %3u  %s  %-14s %s %u\n",
                table[i].pid, state_str,
                table[i].name ? table[i].name : "???",
                table[i].is_user ? "usr" : "kern",
                table[i].cpu_affinity);
    }
}

void sysrq_handle_showmem(void) {
    uint64_t total = pmm_get_total_frames();
    uint64_t used = pmm_get_used_frames();
    kprintf(" PMM: total=%llu frames (%llu KB), used=%llu frames (%llu KB), free=%llu KB\n",
            (unsigned long long)total, (unsigned long long)(total * 4),
            (unsigned long long)used, (unsigned long long)(used * 4),
            (unsigned long long)((total - used) * 4));
    extern uint64_t oom_kill_count;
    kprintf(" OOM kills: %llu\n", (unsigned long long)oom_kill_count);
}

void sysrq_handle_killall(void) {
    struct process *cur = process_get_current();
    struct process *table = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED) continue;
        if (cur && table[i].pid == cur->pid) continue;
        if (table[i].pid <= 1) continue; /* skip idle and init */
        signal_send(table[i].pid, SIGKILL);
        kprintf(" killed pid=%u\n", table[i].pid);
    }
}

void sysrq_handle_poweroff(void) {
    acpi_shutdown();
    /* If ACPI shutdown fails, halt */
    cli();
    for (;;) hlt();
}

void sysrq_handle_sync(void) {
    extern int fat32_sync(void);
    fat32_sync();
    kprintf(" sync done\n");
}

void sysrq_handle_oom(void) {
    oom_kill(0);
}

void sysrq_init(void) {
    kprintf("[OK] SysRq initialized (Alt+PrintScreen+<cmd>)\n");
}
