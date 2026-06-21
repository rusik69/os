/*
 * kdb.c — KDB (Kernel Debugger) shell
 *
 * Implements a simple interactive kernel debugger shell that can be
 * entered via a magic sysrq key or a serial break.  Provides basic
 * commands for inspecting kernel state:
 *   - bt:    backtrace
 *   - ps:    list processes
 *   - lsmod: list loaded modules
 *   - dmesg: print kernel log buffer
 *   - rd:    read memory
 *   - wr:    write memory
 *   - go:    continue execution
 *   - help:  show help
 */

#define KERNEL_INTERNAL
#include "printf.h"
#include "string.h"
#include "serial.h"
#include "process.h"
#include "scheduler.h"
#include "stacktrace.h"
#include "kallsyms.h"
#include "module.h"
#include "logbuf.h"
#include "panic.h"

#define KDB_BUF_SIZE 256
#define KDB_HISTORY  8
#define KDB_PROMPT   "kdb> "

/* Input buffer */
static char kdb_buf[KDB_BUF_SIZE];
static int  kdb_buf_pos;

/* Command history */
static char kdb_history[KDB_HISTORY][KDB_BUF_SIZE];
static int  kdb_history_count;
static int  kdb_history_pos;

/* Active flag */
static volatile int kdb_active;

/* ── Serial I/O helpers ─────────────────────────────────────────── */

static void kdb_putchar(char c)
{
    serial_write(c);
}

static void kdb_puts(const char *s)
{
    while (*s) {
        kdb_putchar(*s++);
    }
}

static char kdb_getchar(void)
{
    return (char)serial_read();
}

/* ── Command implementations ────────────────────────────────────── */

static void cmd_help(void)
{
    kdb_puts(
        "KDB Kernel Debugger Commands:\n"
        "  bt           - Print stack backtrace\n"
        "  ps           - List processes\n"
        "  lsmod        - List loaded kernel modules\n"
        "  dmesg        - Print kernel message buffer\n"
        "  rd <addr>    - Read memory at address (hex)\n"
        "  wr <addr> <val> - Write 32-bit value to address\n"
        "  go           - Resume execution\n"
        "  help         - Show this help\n"
        "  reboot       - Reboot the system\n"
    );
}

static void cmd_bt(void)
{
    kdb_puts("Backtrace:\n");
    print_stack_trace();
}

static void cmd_ps(void)
{
    kdb_puts("PID  NAME         STATE\n");
    kdb_puts("---  ----         -----\n");
    /* Iterate process table */
    for (uint32_t pid = 1; pid < 1024; pid++) {
        struct process *p = process_get_by_pid(pid);
        if (p && p->state != PROCESS_UNUSED && p->name) {
            char line[80];
            int n = snprintf(line, sizeof(line), "%-4u %-12s %d\n",
                             pid, p->name, (int)p->state);
            kdb_puts(line);
        }
    }
}

static void cmd_lsmod(void)
{
    kdb_puts("Loaded modules:\n");
    module_list();  /* defined in module.c */
}

static void cmd_dmesg(void)
{
    char buf[512];
    int len = logbuf_read(buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        kdb_puts(buf);
    }
}

static void cmd_rd(const char *args)
{
    if (!args || !*args) {
        kdb_puts("Usage: rd <hex_address>\n");
        return;
    }
    uint64_t addr = 0;
    while (*args) {
        char c = *args++;
        if (c >= '0' && c <= '9') addr = addr * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') addr = addr * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') addr = addr * 16 + (c - 'A' + 10);
    }
    if (addr == 0) {
        kdb_puts("Invalid address\n");
        return;
    }
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)addr;
    char line[80];
    int n = snprintf(line, sizeof(line), "[0x%llx] = 0x%08x\n",
                     (unsigned long long)addr, *p);
    kdb_puts(line);
}

static void cmd_wr(const char *args)
{
    if (!args || !*args) {
        kdb_puts("Usage: wr <hex_address> <hex_value>\n");
        return;
    }
    uint64_t addr = 0;
    while (*args && *args != ' ') {
        char c = *args++;
        if (c >= '0' && c <= '9') addr = addr * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') addr = addr * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') addr = addr * 16 + (c - 'A' + 10);
    }
    if (addr == 0 || *args != ' ') {
        kdb_puts("Usage: wr <hex_address> <hex_value>\n");
        return;
    }
    args++; /* skip space */
    uint32_t val = 0;
    while (*args) {
        char c = *args++;
        if (c >= '0' && c <= '9') val = val * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') val = val * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val = val * 16 + (c - 'A' + 10);
    }
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)addr;
    *p = val;
    char line[80];
    int n = snprintf(line, sizeof(line), "Wrote 0x%08x to [0x%llx]\n",
                     val, (unsigned long long)addr);
    kdb_puts(line);
}

/* ── Command dispatch ───────────────────────────────────────────── */

static void kdb_execute(const char *cmd)
{
    /* Skip leading whitespace */
    while (*cmd == ' ') cmd++;
    if (!*cmd) return;

    /* Find first word (command) */
    char cmd_name[32];
    int i = 0;
    while (*cmd && *cmd != ' ' && i < 31) cmd_name[i++] = *cmd++;
    cmd_name[i] = '\0';
    while (*cmd == ' ') cmd++;
    const char *args = *cmd ? cmd : NULL;

    if (strcmp(cmd_name, "help") == 0 || strcmp(cmd_name, "?") == 0)
        cmd_help();
    else if (strcmp(cmd_name, "bt") == 0)
        cmd_bt();
    else if (strcmp(cmd_name, "ps") == 0)
        cmd_ps();
    else if (strcmp(cmd_name, "lsmod") == 0)
        cmd_lsmod();
    else if (strcmp(cmd_name, "dmesg") == 0)
        cmd_dmesg();
    else if (strcmp(cmd_name, "rd") == 0)
        cmd_rd(args);
    else if (strcmp(cmd_name, "wr") == 0)
        cmd_wr(args);
    else if (strcmp(cmd_name, "go") == 0) {
        kdb_puts("Resuming...\n");
        kdb_active = 0;
        return;
    } else if (strcmp(cmd_name, "reboot") == 0) {
        kdb_puts("Rebooting...\n");
        /* Trigger reboot via 8042 keyboard controller */
        outb(0x64, 0xFE);
    } else {
        kdb_puts("Unknown command: ");
        kdb_puts(cmd_name);
        kdb_puts("\nType 'help' for available commands.\n");
    }
}

/* ── Main loop ──────────────────────────────────────────────────── */

void kdb_enter(void)
{
    if (kdb_active) return;
    kdb_active = 1;

    kdb_puts("\n=== KDB Kernel Debugger ===\n");
    kdb_puts("Type 'help' for commands, 'go' to resume.\n");

    kdb_buf_pos = 0;
    kdb_buf[0] = '\0';
    kdb_history_pos = kdb_history_count;

    while (kdb_active) {
        kdb_puts(KDB_PROMPT);

        /* Read a line */
        kdb_buf_pos = 0;
        for (;;) {
            char c = kdb_getchar();
            if (c == '\r' || c == '\n') {
                kdb_putchar('\n');
                break;
            }
            if (c == '\b' || c == 0x7F) {
                if (kdb_buf_pos > 0) {
                    kdb_buf_pos--;
                    kdb_puts("\b \b");
                }
                continue;
            }
            if (c >= ' ' && c <= '~' && kdb_buf_pos < KDB_BUF_SIZE - 1) {
                kdb_buf[kdb_buf_pos++] = c;
                kdb_putchar(c);
            }
        }
        kdb_buf[kdb_buf_pos] = '\0';

        /* Add to history */
        if (kdb_buf_pos > 0) {
            if (kdb_history_count < KDB_HISTORY) {
                strncpy(kdb_history[kdb_history_count], kdb_buf, KDB_BUF_SIZE - 1);
                kdb_history_count++;
            } else {
                /* Shift history */
                for (int i = 1; i < KDB_HISTORY; i++)
                    memcpy(kdb_history[i - 1], kdb_history[i], KDB_BUF_SIZE);
                strncpy(kdb_history[KDB_HISTORY - 1], kdb_buf, KDB_BUF_SIZE - 1);
            }
            kdb_history_pos = kdb_history_count;
        }

        kdb_execute(kdb_buf);
    }
}

/* Check if KDB is active */
int kdb_is_active(void)
{
    return kdb_active;
}

/* Initialize KDB */
void kdb_init(void)
{
    kdb_active = 0;
    kdb_buf_pos = 0;
    kdb_history_count = 0;
    kdb_history_pos = 0;
    kprintf("[OK] KDB kernel debugger initialized (use 'kdb' sysrq to enter)\n");
}

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Stub: kdb_parse ───────────────────────────────── */
int kdb_parse(const char *cmd)
{
    (void)cmd;
    kprintf("[KDB] kdb_parse: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: kdb_exec ────────────────────────────────── */
int kdb_exec(const char *cmd)
{
    (void)cmd;
    kprintf("[KDB] kdb_exec: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: kdb_printf ──────────────────────────────── */
int kdb_printf(const char *fmt, ...)
{
    (void)fmt;
    kprintf("[KDB] kdb_printf: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: kdb_register ────────────────────────────── */
int kdb_register(const char *cmd, kdb_func_t func, const char *usage)
{
    (void)cmd;
    (void)func;
    (void)usage;
    kprintf("[KDB] kdb_register: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: kdb_unregister ──────────────────────────── */
int kdb_unregister(const char *cmd)
{
    (void)cmd;
    kprintf("[KDB] kdb_unregister: not yet implemented\n");
    return -ENOSYS;
}
