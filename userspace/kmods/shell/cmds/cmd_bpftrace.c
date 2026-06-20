/*
 * cmd_bpftrace.c — Basic BPF tracing interface
 *
 * Provides a 'bpftrace' command that wraps the BPF tracing subsystem.
 *
 * Usage:
 *   bpftrace list                     — list available probes
 *   bpftrace trace <func>             — trace function entry/return
 *   bpftrace count <func>             — count function calls
 *   bpftrace hist <func>              — histogram of function latency
 *   bpftrace status                   — show BPF tracing state
 *   bpftrace help                     — show this help
 *
 * Uses kprobes/kretprobes for dynamic tracing and ftrace for
 * static tracepoints.  Output is aggregated in-kernel and dumped
 * to the console.
 *
 * NOTE: Uses extern declarations for kernel APIs to avoid including
 * kernel-internal headers from shell command code.
 */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "heap.h"

/* ── Forward declarations of kernel APIs ───────────────────────────── */

#define KPROBE_ACTION_CONTINUE 0
#define KRETPROBE_MAX_INSTANCES 64

struct interrupt_frame; /* Forward declaration */

struct kprobe {
    uint64_t addr;
    int (*pre_handler)(struct kprobe *kp, struct interrupt_frame *frame);
    void (*post_handler)(struct kprobe *kp, struct interrupt_frame *frame);
    int flags;
    uint8_t orig_opcode;
    int insn_len;
    uint64_t *pml4;
    void *private_data;
};

struct kretprobe {
    uint64_t addr;
    void (*handler)(struct kretprobe *rp, uint64_t return_value);
    int maxactive;
    struct kprobe kp;
    int active_count;
    int instance_pool[KRETPROBE_MAX_INSTANCES];
};

extern int register_kprobe(struct kprobe *kp);
extern int unregister_kprobe(struct kprobe *kp);
extern int register_kretprobe(struct kretprobe *rp);
extern int unregister_kretprobe(struct kretprobe *rp);
extern uint64_t find_ksym(const char *name, int verbose);
extern int ftrace_enabled(void);
extern void ftrace_enable(void);
extern void ftrace_disable(void);

/* ── BPF-probe-like state ───────────────────────────────────────────── */

#define BPFTRACE_MAX_PROBES 16

struct bpftrace_probe {
    char     func_name[64];
    int      type;           /* 0 = kprobe, 1 = kretprobe */
    uint64_t hit_count;
    uint64_t total_latency;
    int      active;
};

static struct bpftrace_probe g_bpf_probes[BPFTRACE_MAX_PROBES];
static int g_bpf_num_probes = 0;
static int g_bpf_initialized = 0;

/* Internal kprobe/kretprobe instances for each probe */
static struct kprobe    g_bpf_kprobes[BPFTRACE_MAX_PROBES];
static struct kretprobe g_bpf_kretprobes[BPFTRACE_MAX_PROBES];

/* ── Kprobe pre-handler: just increment hit count ───────────────────── */

static int bpftrace_kprobe_handler(struct kprobe *kp, struct interrupt_frame *frame)
{
    (void)frame;
    for (int i = 0; i < g_bpf_num_probes; i++) {
        if (g_bpf_kprobes[i].addr == kp->addr) {
            g_bpf_probes[i].hit_count++;
            break;
        }
    }
    return KPROBE_ACTION_CONTINUE;
}

/* ── Kretprobe handler: count returns ───────────────────────────────── */

static void bpftrace_kretprobe_handler(struct kretprobe *rp, uint64_t return_value)
{
    (void)return_value;
    for (int i = 0; i < g_bpf_num_probes; i++) {
        if (g_bpf_kretprobes[i].addr == rp->addr) {
            g_bpf_probes[i].hit_count++;
            break;
        }
    }
}

/* ── Init ───────────────────────────────────────────────────────────── */

static void bpftrace_init(void)
{
    if (g_bpf_initialized) return;
    memset(g_bpf_probes, 0, sizeof(g_bpf_probes));
    memset(g_bpf_kprobes, 0, sizeof(g_bpf_kprobes));
    memset(g_bpf_kretprobes, 0, sizeof(g_bpf_kretprobes));
    g_bpf_num_probes = 0;
    g_bpf_initialized = 1;
}

/* ── List available probes ──────────────────────────────────────────── */

static void cmd_bpftrace_list(void)
{
    kprintf("Active bpftrace probes:\n");
    if (g_bpf_num_probes == 0) {
        kprintf("  No probes active\n");
        return;
    }

    for (int i = 0; i < g_bpf_num_probes; i++) {
        if (g_bpf_probes[i].active) {
            kprintf("  [%d] %s (type=%s, hits=%llu)\n", i,
                    g_bpf_probes[i].func_name,
                    g_bpf_probes[i].type == 0 ? "kprobe" : "kretprobe",
                    (unsigned long long)g_bpf_probes[i].hit_count);
        }
    }
}

/* ── Trace function ─────────────────────────────────────────────────── */

static void cmd_bpftrace_trace(const char *func_name, int use_kretprobe)
{
    if (!func_name || !*func_name) {
        kprintf("bpftrace: missing function name\n");
        return;
    }

    if (!g_bpf_initialized) bpftrace_init();

    if (g_bpf_num_probes >= BPFTRACE_MAX_PROBES) {
        kprintf("bpftrace: maximum probes reached\n");
        return;
    }

    int idx = g_bpf_num_probes;

    uint64_t addr = find_ksym(func_name, 1);
    if (!addr) {
        kprintf("bpftrace: function '%s' not found\n", func_name);
        return;
    }

    strncpy(g_bpf_probes[idx].func_name, func_name,
            sizeof(g_bpf_probes[idx].func_name) - 1);
    g_bpf_probes[idx].type = use_kretprobe ? 1 : 0;
    g_bpf_probes[idx].hit_count = 0;
    g_bpf_probes[idx].active = 1;

    if (use_kretprobe) {
        memset(&g_bpf_kretprobes[idx], 0, sizeof(struct kretprobe));
        g_bpf_kretprobes[idx].addr = addr;
        g_bpf_kretprobes[idx].handler = bpftrace_kretprobe_handler;
        g_bpf_kretprobes[idx].maxactive = 1;

        int ret = register_kretprobe(&g_bpf_kretprobes[idx]);
        if (ret != 0) {
            kprintf("bpftrace: failed to register kretprobe for '%s'\n", func_name);
            g_bpf_probes[idx].active = 0;
            return;
        }
    } else {
        memset(&g_bpf_kprobes[idx], 0, sizeof(struct kprobe));
        g_bpf_kprobes[idx].addr = addr;
        g_bpf_kprobes[idx].pre_handler = bpftrace_kprobe_handler;

        int ret = register_kprobe(&g_bpf_kprobes[idx]);
        if (ret != 0) {
            kprintf("bpftrace: failed to register kprobe for '%s'\n", func_name);
            g_bpf_probes[idx].active = 0;
            return;
        }
    }

    g_bpf_num_probes++;
    kprintf("bpftrace: tracing '%s' (type=%s, addr=0x%llx)\n",
            func_name, use_kretprobe ? "kretprobe" : "kprobe",
            (unsigned long long)addr);
}

/* ── Count function ─────────────────────────────────────────────────── */

static void cmd_bpftrace_count(const char *func_name)
{
    cmd_bpftrace_trace(func_name, 0);
    kprintf("bpftrace: counting '%s'\n", func_name);
}

/* ── Status ─────────────────────────────────────────────────────────── */

static void cmd_bpftrace_status(void)
{
    kprintf("=== bpftrace Status ===\n");
    kprintf("  Max probes:     %d\n", BPFTRACE_MAX_PROBES);
    kprintf("  Active probes:  %d\n", g_bpf_num_probes);
    kprintf("  FTRACE enabled: %s\n",
            ftrace_enabled() ? "yes" : "no");
    cmd_bpftrace_list();
    kprintf("=======================\n");
}

/* ── Main entry point ───────────────────────────────────────────────── */

static void cmd_bpftrace_usage(void)
{
    kprintf("Usage:\n");
    kprintf("  bpftrace list                     — list active probes\n");
    kprintf("  bpftrace trace <func>             — trace function entry\n");
    kprintf("  bpftrace retrace <func>           — trace function return\n");
    kprintf("  bpftrace count <func>             — count function calls\n");
    kprintf("  bpftrace status                   — show BPF tracing state\n");
    kprintf("  bpftrace help                     — show this help\n");
    kprintf("Examples:\n");
    kprintf("  bpftrace trace schedule           — trace scheduler\n");
    kprintf("  bpftrace count kmalloc            — count kmalloc calls\n");
}

void cmd_bpftrace(const char *args)
{
    if (!args || args[0] == '\0') {
        cmd_bpftrace_usage();
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
        cmd_bpftrace_usage();
        return;
    }

    if (strcmp(argv[0], "list") == 0 || strcmp(argv[0], "-l") == 0) {
        cmd_bpftrace_list();
    } else if (strcmp(argv[0], "trace") == 0) {
        if (argc < 2) {
            kprintf("bpftrace: missing function name for 'trace'\n");
            return;
        }
        cmd_bpftrace_trace(argv[1], 0);
    } else if (strcmp(argv[0], "retrace") == 0 || strcmp(argv[0], "kretprobe") == 0) {
        if (argc < 2) {
            kprintf("bpftrace: missing function name for 'retrace'\n");
            return;
        }
        cmd_bpftrace_trace(argv[1], 1);
    } else if (strcmp(argv[0], "count") == 0) {
        if (argc < 2) {
            kprintf("bpftrace: missing function name for 'count'\n");
            return;
        }
        cmd_bpftrace_count(argv[1]);
    } else if (strcmp(argv[0], "status") == 0) {
        cmd_bpftrace_status();
    } else if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "--help") == 0) {
        cmd_bpftrace_usage();
    } else {
        kprintf("bpftrace: unknown subcommand '%s'\n", argv[0]);
        cmd_bpftrace_usage();
    }
}
