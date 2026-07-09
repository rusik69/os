/*
 * sysrq.c — Magic System Request Key handling (Item 209)
 *
 * Implements the Linux-style Magic SysRq mechanism, allowing kernel
 * operations to be performed via Alt+SysRq+<command> even when frozen.
 * Also supports /proc/sysrq-trigger write interface and serial trigger.
 *
 * Operations:
 *   b — Emergency reboot
 *   c — Trigger crash (kdump testing)
 *   e — Send SIGTERM to all tasks except init
 *   f — Full OOM kill
 *   h — Show this help (available commands)
 *   i — Send SIGKILL to all tasks except init/self
 *   k — SAK (Secure Attention Key) — kill console processes
 *   l — Show all CPUs / stack backtrace
 *   m — Show memory info
 *   o — Emergency poweroff
 *   p — Show CPU registers
 *   s — Sync filesystems
 *   t — Show task list
 *   u — Remount all filesystems read-only (emergency)
 *   w — Show blocked (D-state) tasks
 */

#define KERNEL_INTERNAL
#include "sysrq.h"
#include "types.h"
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
#include "keyboard.h"
#include "string.h"
#include "kernel.h"

/* ── Forward declarations of extern functions ────────────────────── */

extern void watchdog_system_reset(void);
extern int smp_get_cpu_count(void);

/* ── Mount table (for 'u' remount) ───────────────────────────────── */

extern struct vfs_mount mounts[VFS_MAX_MOUNTS];
extern int num_mounts;

/* ── OOM kill count ──────────────────────────────────────────────── */

extern uint64_t oom_kill_count;

/* ── Global sysrq enable mask (kernel.sysrq) ─────────────────────── */

static int __read_mostly sysrq_enable_mask = SYSRQ_ENABLE_DEFAULT;

int sysrq_get_mask(void)
{
    return sysrq_enable_mask;
}

void sysrq_set_mask(int mask)
{
    sysrq_enable_mask = mask;
}

/*
 * Check whether a particular SysRq command character is permitted
 * by the current enable mask.  Uses the same bit assignments as
 * the Linux kernel.sysrq sysctl.
 */
int sysrq_is_enabled(char cmd)
{
    /* Mask == 0 means completely disabled */
    if (sysrq_enable_mask == 0)
        return 0;

    /* Mask < 0 (all ones, value -1) is legacy "enable all" */
    if (sysrq_enable_mask < 0)
        return 1;

    int bit = 0;
    switch (cmd) {
    case 's': bit = SYSRQ_ENABLE_SYNC;    break;
    case 'u': bit = SYSRQ_ENABLE_REMOUNT; break;
    case 'e':
    case 'i': bit = SYSRQ_ENABLE_SIGNAL;  break;
    case 'b':
    case 'o': bit = SYSRQ_ENABLE_REBOOT;  break;
    case 'p':
    case 'c':
    case 'l':
    case 't':
    case 'w': bit = SYSRQ_ENABLE_DUMP;    break;
    case 'f': bit = SYSRQ_ENABLE_OOM;     break;
    case 'k': bit = SYSRQ_ENABLE_SAK;     break;
    case 'h': bit = SYSRQ_ENABLE_HELP;    break;
    default:
        /* Unknown commands are denied */
        return 0;
    }

    return (sysrq_enable_mask & bit) != 0 || (sysrq_enable_mask & SYSRQ_ENABLE_ALL) != 0;
}

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

/* ── Command dispatch table structure ────────────────────────────── */

struct sysrq_entry {
    char key;
    const char *desc;
    void (*handler)(void);
};

/* Forward declarations of all handlers */
static void handle_reboot(void);
static void handle_crash(void);
static void handle_term(void);
static void handle_oom(void);
static void handle_killall(void);
static void handle_show_cpus(void);
static void handle_showmem(void);
static void handle_poweroff(void);
static void handle_showregs(void);
static void handle_sync(void);
static void handle_showtasks(void);
static void handle_umount(void);
static void handle_show_blocked(void);
static void handle_help(void);
static void handle_sak(void);

/* ── Command dispatch table ──────────────────────────────────────── */

static const struct sysrq_entry sysrq_table[] = {
    { 'b', "Emergency reboot",                        handle_reboot       },
    { 'c', "Trigger crash (kdump)",                   handle_crash        },
    { 'e', "Send SIGTERM to all tasks",               handle_term         },
    { 'f', "Full OOM kill",                           handle_oom          },
    { 'h', "Show this help text",                     handle_help         },
    { 'i', "Send SIGKILL to all tasks",               handle_killall      },
    { 'k', "SAK — kill console processes",            handle_sak          },
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
    /* Dereference NULL to trigger page fault -> panic */
#if !defined(__clang__)
    _Pragma("GCC diagnostic push")
    _Pragma("GCC diagnostic ignored \"-Wanalyzer-null-dereference\"")
#endif
    volatile int *p = (volatile int *)0;
    *p = 0xDEAD;
#if !defined(__clang__)
    _Pragma("GCC diagnostic pop")
#endif
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

    /* OOM kill count */
    kprintf(" OOM kills: %llu\n", (unsigned long long)oom_kill_count);

    /* System uptime */
    uint64_t secs = timer_get_ticks() / TIMER_FREQ;
    kprintf(" Uptime: %llu seconds\n", (unsigned long long)secs);
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

/* ── New handler: help (h) ───────────────────────────────────────── */

static void handle_help(void)
{
    kprintf("Available SysRq commands:\n");
    kprintf(" %-3s %-45s %s\n", "Key", "Action", "Enabled?");
    kprintf(" %-3s %-45s %s\n", "---", "------", "--------");

    for (int i = 0; sysrq_table[i].handler; i++) {
        kprintf(" %c    %-45s %s\n",
                sysrq_table[i].key,
                sysrq_table[i].desc,
                sysrq_is_enabled(sysrq_table[i].key) ? "yes" : "no");
    }

    kprintf("\n Current sysrq mask: 0x%x\n", sysrq_enable_mask);
    kprintf(" To enable/disable: echo <mask> > /proc/sys/kernel/sysrq\n");
    kprintf(" To trigger: echo <cmd> > /proc/sysrq-trigger\n");
}

/* ── New handler: SAK (k) — kill console processes ───────────────── */

static void handle_sak(void)
{
    kprintf("SAK — killing console processes...\n");

    /*
     * SAK (Secure Attention Key) kills all processes that have the
     * current console open.  In our simplified implementation, we
     * kill all user processes that have an active TTY (console FDs).
     *
     * This is a critical security mechanism: if the login screen
     * is compromised, SAK ensures a clean state.
     */
    struct process *table = process_get_table();
    int killed = 0;

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED)
            continue;
        if (!table[i].is_user)
            continue; /* skip kernel threads */
        if (table[i].pid <= 1)
            continue; /* skip idle and init */

        /* Check if this process has an open console/TTY file descriptor */
        int has_tty = 0;
        for (int fd = 0; fd < PROCESS_FD_MAX && !has_tty; fd++) {
            if (!table[i].fd_table[fd].used)
                continue;
            /* Check if the file path contains "/dev/tty" or "/dev/console" */
            const char *path = table[i].fd_table[fd].path;
            if (path && (strstr(path, "/dev/tty") ||
                         strstr(path, "/dev/console") ||
                         strstr(path, "/dev/ttyS"))) {
                has_tty = 1;
            }
        }

        /* If no explicit tty fd, check if it's the foreground console task */
        if (!has_tty) {
            /* Simple heuristic: user processes with stdin pointing to a tty */
            if (table[i].fd_table[0].used &&
                table[i].fd_table[0].path[0] != '\0' &&
                strstr(table[i].fd_table[0].path, "tty")) {
                has_tty = 1;
            }
        }

        if (has_tty) {
            signal_send(table[i].pid, SIGKILL);
            kprintf("  killed pid=%u (%s)\n",
                    table[i].pid,
                    table[i].name ? table[i].name : "???");
            killed++;
        }
    }

    kprintf(" SAK complete: %d processes killed\n", killed);
}

/* ── Public API ──────────────────────────────────────────────────── */

void sysrq_handle(char cmd)
{
    /* Check the enable mask */
    if (!sysrq_is_enabled(cmd)) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        kprintf("\n[SysRq] Command '%c' is disabled (sysrq mask=0x%x)\n",
                cmd, sysrq_enable_mask);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

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

/*
 * Serial trigger — called when the serial console receives the escape
 * sequence (NUL+cmd) on the serial line.
 */
void sysrq_serial_trigger(char cmd)
{
    kprintf("[SysRq serial trigger]\n");
    sysrq_handle(cmd);
}

/* ── Keyboard hook ───────────────────────────────────────────────── */

/*
 * Called from the keyboard IRQ handler when Alt+SysRq+<key> is
 * detected.  The keyboard handler tracks Alt and SysRq key states
 * and when both are held, the next normal keypress is redirected here.
 */
static void keyboard_sysrq_hook(char ascii_char)
{
    sysrq_handle(ascii_char);
}

void __init sysrq_init(void)
{
    /* Register with keyboard driver for Alt+SysRq detection */
    keyboard_set_sysrq_callback(keyboard_sysrq_hook);

    kprintf("[OK] Magic SysRq initialized (%d commands, mask=0x%x)\n",
            (int)ARRAY_SIZE(sysrq_table) - 1,
            sysrq_enable_mask);
}

/* ── Stub: sysrq_handle_crash ──────────────────────────────────────── */
static void sysrq_handle_crash(void)
{
    kprintf("[SYSRQ] sysrq_handle_crash: not yet implemented\n");
}

/* ── Stub: sysrq_handle_term ───────────────────────────────────────── */
static void sysrq_handle_term(void)
{
    kprintf("[SYSRQ] sysrq_handle_term: not yet implemented\n");
}

/* ── Stub: sysrq_handle_reboot ─────────────────────────────────────── */
static void sysrq_handle_reboot(void)
{
    kprintf("[SYSRQ] sysrq_handle_reboot: not yet implemented\n");
}

/* ── Stub: sysrq_register_handler ──────────────────────────────────── */
static int sysrq_register_handler(int key, void (*handler)(void))
{
    (void)key; (void)handler;
    kprintf("[SYSRQ] sysrq_register_handler: not yet implemented\n");
    return 0;
}

/* ── Stub: sysrq_unregister_handler ────────────────────────────────── */
static int sysrq_unregister_handler(int key)
{
    (void)key;
    kprintf("[SYSRQ] sysrq_unregister_handler: not yet implemented\n");
    return 0;
}
