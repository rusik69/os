/*
 * sysrq.c — Magic System Request Key handling (Item 209)
 *
 * Implements the Linux-style Magic SysRq mechanism, allowing kernel
 * operations to be performed via Alt+SysRq+<command> even when frozen.
 *
 * Operations:
 *   b — Emergency reboot
 *   c — Trigger crash (kdump testing)
 *   e — Send SIGTERM to all tasks except init
 *   f — Full OOM kill
 *   i — Send SIGKILL to all tasks except init
 *   l — Show all CPUs / stack backtrace
 *   m — Memory info
 *   o — Emergency poweroff
 *   p — Show CPU registers
 *   s — Sync filesystems
 *   t — Show task list
 *   u — Remount all filesystems read-only (emergency)
 *   w — Show blocked (D-state) tasks
 */

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
#include "vfs.h"

/* ── Forward declarations of extern functions ────────────────────── */

extern void watchdog_system_reset(void);
extern int smp_get_cpu_count(void);

/* ── Mount table (for 'u' remount) ───────────────────────────────── */

extern struct vfs_mount mounts[VFS_MAX_MOUNTS];
extern int num_mounts;

/* ── Helper: color banner ────────────────────────────────────────── */

static void sysrq_banner(char cmd)
{
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    kprintf("\n========================================\n");
    kprintf("[SysRq] (%c) ", cmd);
}

static void sysrq_footer(void)
{
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("\n");
}

/* ── Individual handlers ─────────────────────────────────────────── */

static void handle_reboot(void)
{
    kprintf("Emergency reboot\n");
    watchdog_system_reset();
    /* If watchdog reset fails, try ACPI reset */
    acpi_reboot();
    /* Triple fault as last resort */
    cli();
    for (;;) hlt();
}

static void handle_crash(void)
{
    kprintf("Triggering kernel crash for kdump...\n");
    sysrq_footer();
    /* Dereference NULL to trigger page fault → panic */
    volatile int *p = (volatile int *)0;
    *p = 0xDEAD;
}

static void handle_term(void)
{
    kprintf("Sending SIGTERM to all tasks...\n");
    struct process *table = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED) continue;
        if (table[i].pid <= 1) continue; /* skip idle + init */
        signal_send(table[i].pid, SIGTERM);
    }
    kprintf(" done\n");
}

static void handle_killall(void)
{
    kprintf("Sending SIGKILL to all tasks...\n");
    struct process *cur = process_get_current();
    struct process *table = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED) continue;
        if (cur && table[i].pid == cur->pid) continue;
        if (table[i].pid <= 1) continue; /* skip idle and init */
        signal_send(table[i].pid, SIGKILL);
        kprintf(" killed pid=%u\n", table[i].pid);
    }
    kprintf(" done\n");
}

static void handle_show_cpus(void)
{
    kprintf("All CPUs:");
    int ncpu = smp_get_cpu_count();
    if (ncpu < 1) ncpu = 1;

    for (int cpu = 0; cpu < ncpu; cpu++) {
        struct process *cur = process_get_current();
        if (cur) {
            kprintf(" CPU%d:PID=%lu Name=\"%s\" State=%d",
                    cpu,
                    (unsigned long)cur->pid,
                    cur->name ? cur->name : "?",
                    (int)cur->state);
        } else {
            kprintf(" CPU%d:(none)", cpu);
        }
    }
    kprintf("\n");
}

static void handle_showmem(void)
{
    uint64_t total = pmm_get_total_frames();
    uint64_t used  = pmm_get_used_frames();
    kprintf("Physical memory: total=%llu frames (%llu KB), "
            "used=%llu frames (%llu KB), free=%llu KB\n",
            (unsigned long long)total, (unsigned long long)(total * 4),
            (unsigned long long)used, (unsigned long long)(used * 4),
            (unsigned long long)((total - used) * 4));

    extern uint64_t oom_kill_count;
    kprintf(" OOM kills: %llu\n", (unsigned long long)oom_kill_count);
}

static void handle_poweroff(void)
{
    kprintf("Emergency poweroff\n");
    acpi_shutdown();
    /* If ACPI fails, halt */
    cli();
    for (;;) hlt();
}

static void handle_showregs(void)
{
    uint64_t cr0, cr2, cr3, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    kprintf(" CR0=%016llx  CR2=%016llx\n CR3=%016llx  CR4=%016llx\n",
            (unsigned long long)cr0, (unsigned long long)cr2,
            (unsigned long long)cr3, (unsigned long long)cr4);
    uint64_t rsp, rbp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    kprintf(" RSP=%016llx  RBP=%016llx\n",
            (unsigned long long)rsp, (unsigned long long)rbp);
}

static void handle_sync(void)
{
    kprintf("Syncing filesystems...\n");
    vfs_sync_all();
    kprintf(" sync done\n");
}

static void handle_showtasks(void)
{
    struct process *table = process_get_table();
    kprintf(" PID  STATE   NAME              USER CPU\n");
    kprintf(" ---  -----  ----------------  ---- ---\n");
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED) continue;
        const char *state_str = "?";
        switch (table[i].state) {
            case PROCESS_READY:   state_str = "READY"; break;
            case PROCESS_RUNNING: state_str = "RUN";   break;
            case PROCESS_BLOCKED: state_str = "BLKD";  break;
            case PROCESS_ZOMBIE:  state_str = "ZOMB";  break;
            default: break;
        }
        kprintf(" %3u  %-5s  %-16s %-4s %u\n",
                table[i].pid, state_str,
                table[i].name ? table[i].name : "???",
                table[i].is_user ? "usr" : "kern",
                table[i].cpu_affinity);
    }
}

static void handle_umount(void)
{
    kprintf("Remounting all filesystems read-only...\n");
    vfs_sync_all();

    /* Set the MS_RDONLY flag on every mount */
    for (int i = 0; i < num_mounts && i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].mountpoint[0] == '\0')
            continue;
        mounts[i].flags |= MS_RDONLY;
        kprintf("  %s -> read-only\n", mounts[i].mountpoint);
    }
    kprintf(" done\n");
}

static void handle_show_blocked(void)
{
    kprintf("Blocked (D-state) tasks:\n");
    struct process *table = process_get_table();
    int found = 0;
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state != PROCESS_BLOCKED) continue;
        kprintf("  PID=%u Name=\"%s\"\n",
                table[i].pid,
                table[i].name ? table[i].name : "???");
        found = 1;
    }
    if (!found)
        kprintf(" (none)\n");
}

static void handle_oom(void)
{
    kprintf("Full OOM kill...\n");
    oom_kill(0);
    kprintf(" done\n");
}

/* ── Command dispatch table ──────────────────────────────────────── */

struct sysrq_entry {
    char key;
    const char *desc;
    void (*handler)(void);
};

static const struct sysrq_entry sysrq_table[] = {
    { 'b', "Emergency reboot",                        handle_reboot       },
    { 'c', "Trigger crash (kdump)",                   handle_crash        },
    { 'e', "Send SIGTERM to all tasks",               handle_term         },
    { 'f', "Full OOM kill",                           handle_oom          },
    { 'i', "Send SIGKILL to all tasks",               handle_killall      },
    { 'l', "Show all CPUs / backtrace",               handle_show_cpus    },
    { 'm', "Show memory information",                 handle_showmem      },
    { 'o', "Emergency poweroff",                      handle_poweroff     },
    { 'p', "Show CPU registers",                      handle_showregs     },
    { 's', "Sync filesystems",                        handle_sync         },
    { 't', "Show task list",                          handle_showtasks    },
    { 'u', "Remount filesystems read-only",           handle_umount       },
    { 'w', "Show blocked (D-state) tasks",            handle_show_blocked },
    { 0, NULL, NULL },
};

/* ── Public API ──────────────────────────────────────────────────── */

void sysrq_handle(char cmd)
{
    for (int i = 0; sysrq_table[i].handler; i++) {
        if (sysrq_table[i].key == cmd) {
            sysrq_banner(cmd);
            kprintf("%s\n", sysrq_table[i].desc);
            sysrq_table[i].handler();
            sysrq_footer();
            return;
        }
    }

    /* Unknown command */
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    kprintf("\n[SysRq] Unknown command '%c' (0x%02x)\n", cmd, (unsigned char)cmd);
    kprintf("[SysRq] Available: ");
    for (int i = 0; sysrq_table[i].handler; i++)
        kprintf("%c ", sysrq_table[i].key);
    kprintf("\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

int sysrq_is_valid(char cmd)
{
    for (int i = 0; sysrq_table[i].handler; i++)
        if (sysrq_table[i].key == cmd) return 1;
    return 0;
}

void sysrq_init(void)
{
    kprintf("[OK] Magic SysRq initialized (%d commands)\n",
            (int)(sizeof(sysrq_table) / sizeof(sysrq_table[0])) - 1);
}
