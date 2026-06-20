/*
 * cmd_trace.c — Trace control shell command
 *
 * Usage:
 *   trace enable                    — enable tracing
 *   trace disable                   — disable tracing
 *   trace show [n]                  — show last N trace events
 *   trace clear                     — clear trace buffer
 *   trace status                    — show trace state
 *   trace help                      — show this help
 *
 * Controls both the legacy trace buffer and the structured
 * trace event subsystem (sched, IRQ, timer, page fault, syscall).
 */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

/* ── Forward declarations of trace subsystem functions ──────────────── */

/* trace.c exports */
extern void trace_dump(void);
extern void trace_init(void);

/* trace_events.c exports (trace_events subsystem) */
extern void trace_events_enable(void);
extern void trace_events_disable(void);
extern int  trace_events_is_enabled(void);
extern void trace_events_dump(int limit);
extern void trace_events_clear(void);
extern void trace_events_stats(uint64_t *sched, uint64_t *timer, uint64_t *irq);

/* ftrace.c exports (ftrace events) */
extern void ftrace_events_enable(void);
extern void ftrace_events_disable(void);
extern int  ftrace_events_is_enabled(void);

/* trace.c new structured events */
extern void trace_ev_enable(void);
extern void trace_ev_disable(void);
extern int  trace_ev_is_enabled(void);
extern void trace_ev_dump(int limit);
extern void trace_ev_clear(void);

static void cmd_trace_usage(void)
{
    kprintf("Usage:\n");
    kprintf("  trace enable             — enable all tracing\n");
    kprintf("  trace disable            — disable all tracing\n");
    kprintf("  trace show [n]           — show last N trace events (default all)\n");
    kprintf("  trace clear              — clear trace buffer\n");
    kprintf("  trace status             — show trace state\n");
    kprintf("  trace dump               — dump full legacy trace\n");
    kprintf("  trace help               — show this help\n");
}

static void cmd_trace_status(void)
{
    kprintf("=== Trace Status ===\n");

    /* Check ftrace event state */
    kprintf("  FTRACE events:   %s\n",
            ftrace_events_is_enabled() ? "ENABLED" : "DISABLED");

    /* Check structured trace events */
    kprintf("  Struct events:   %s\n",
            trace_ev_is_enabled() ? "ENABLED" : "DISABLED");

    /* Show statistics from trace_events subsystem */
    uint64_t sched = 0, timer = 0, irq = 0;
    trace_events_stats(&sched, &timer, &irq);
    kprintf("  Event counters:\n");
    kprintf("    sched_switch:  %llu\n", (unsigned long long)sched);
    kprintf("    timer_expire:  %llu\n", (unsigned long long)timer);
    kprintf("    irq_handlers:  %llu\n", (unsigned long long)irq);
    kprintf("===================\n");
}

void cmd_trace(const char *args)
{
    if (!args || args[0] == '\0') {
        cmd_trace_usage();
        return;
    }

    char buf[256];
    strncpy(buf, args, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *argv[8];
    int argc = 0;
    char *p = buf;
    while (*p && argc < 8) {
        while (*p == ' ') p++;
        if (*p == '\0') break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) { *p = '\0'; p++; }
    }

    if (argc < 1) {
        cmd_trace_usage();
        return;
    }

    if (strcmp(argv[0], "enable") == 0 || strcmp(argv[0], "on") == 0) {
        ftrace_events_enable();
        trace_ev_enable();
        kprintf("Tracing enabled\n");
    } else if (strcmp(argv[0], "disable") == 0 || strcmp(argv[0], "off") == 0) {
        ftrace_events_disable();
        trace_ev_disable();
        kprintf("Tracing disabled\n");
    } else if (strcmp(argv[0], "show") == 0 || strcmp(argv[0], "dump") == 0) {
        int limit = 0;
        if (argc >= 2)
            limit = atoi(argv[1]);
        trace_ev_dump(limit);
    } else if (strcmp(argv[0], "clear") == 0) {
        trace_ev_clear();
        kprintf("Trace buffer cleared\n");
    } else if (strcmp(argv[0], "status") == 0 || strcmp(argv[0], "stat") == 0) {
        cmd_trace_status();
    } else if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "--help") == 0) {
        cmd_trace_usage();
    } else {
        kprintf("trace: unknown subcommand '%s'\n", argv[0]);
        cmd_trace_usage();
    }
}
