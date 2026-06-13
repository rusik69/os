/*
 * cmd_perf.c — Performance monitoring shell command
 *
 * Provides a 'perf' command for collecting and displaying performance
 * counter data, context-switch events, page fault samples, and
 * flame graph output.
 *
 * Usage:
 *   perf list               — list available events
 *   perf stat               — show current PMU counter values
 *   perf record             — start recording (if supported)
 *   perf report             — show recorded samples
 *   perf flamegraph         — generate folded stack flame graph data
 *   perf cswitch            — show context switch trace
 *   perf pfstat             — show page fault sampling data
 *
 * Examples:
 *   perf stat               — display cycle/instruction counts
 *   perf cswitch            — dump context switch events
 *   perf flamegraph         — output folded stack format
 */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

/* ── Forward declarations of perf subsystem functions ──────────────── */

/* perf_events.c exports */
extern uint64_t perf_read_pmc(int counter);
extern uint64_t perf_sw_read_context_switches(void);
extern uint64_t perf_sw_read_page_faults(void);
extern int pebs_total_samples(void);
extern int pebs_available(void);

/* Perf context-switch tracing */
extern void perf_cswitch_init(void);
extern int  perf_cswitch_read(void *buf, int max_count);

/* Perf page fault sampling */
extern void perf_pf_init(void);
extern int  perf_pf_read_samples(void *buf, int max_count);

/* Perf flame graph */
extern void perf_flame_init(void);
extern int  perf_flame_generate(char *buf, int buf_size);
extern int  perf_flame_num_stacks(void);
extern void perf_flame_clear(void);

static void cmd_perf_usage(void)
{
    kprintf("Usage:\n");
    kprintf("  perf list              — list available events\n");
    kprintf("  perf stat              — show performance counters\n");
    kprintf("  perf record            — start recording (stub)\n");
    kprintf("  perf report            — show recorded samples\n");
    kprintf("  perf flamegraph        — output folded stack flame graph\n");
    kprintf("  perf cswitch           — dump context switch trace\n");
    kprintf("  perf pfstat            — dump page fault samples\n");
    kprintf("  perf help              — show this help\n");
}

static void cmd_perf_list(void)
{
    kprintf("Available performance events:\n");
    kprintf("  cpu-cycles             (PMC0)\n");
    kprintf("  instructions           (PMC1)\n");
    kprintf("  context-switches       (software)\n");
    kprintf("  page-faults            (software)\n");
    kprintf("  PEBS samples           (precise event sampling)\n");
    kprintf("  cswitch-events         (context switch trace)\n");
    kprintf("  page-fault-samples     (page fault sampling)\n");
    kprintf("  flame-graph            (aggregated stack traces)\n");
}

static void cmd_perf_stat(void)
{
    kprintf("=== Performance Counters ===\n");

    /* Hardware counters */
    uint64_t cycles = perf_read_pmc(0);
    uint64_t instrs = perf_read_pmc(1);
    kprintf("  PMC0 (cycles):       %llu\n", (unsigned long long)cycles);
    kprintf("  PMC1 (instructions): %llu\n", (unsigned long long)instrs);

    if (instrs > 0) {
        kprintf("  CPI:                 %.2f\n",
                (double)cycles / (double)instrs);
    }

    /* Software counters */
    uint64_t cs = perf_sw_read_context_switches();
    uint64_t pf = perf_sw_read_page_faults();
    kprintf("  context switches:    %llu\n", (unsigned long long)cs);
    kprintf("  page faults:         %llu\n", (unsigned long long)pf);

    /* PEBS samples */
    if (pebs_available()) {
        kprintf("  PEBS samples:        %d\n", pebs_total_samples());
    }

    /* Flame graph stats */
    kprintf("  flame graph stacks:  %d\n", perf_flame_num_stacks());
    kprintf("==============================\n");
}

static void cmd_perf_cswitch(void)
{
    kprintf("=== Context Switch Trace ===\n");
    kprintf("(interface: read from perf_cswitch ring buffer)\n");

    /* In a full implementation, we'd read the buffer and iterate */
    struct {
        uint64_t timestamp;
        uint32_t prev_pid;
        uint32_t next_pid;
        char     prev_comm[16];
        char     next_comm[16];
        uint8_t  prev_state;
    } buf[32];

    int count = perf_cswitch_read(buf, 32);
    if (count <= 0) {
        kprintf("  No context switch events recorded\n");
        return;
    }

    kprintf("  Timestamp    Prev  Next  State\n");
    for (int i = 0; i < count && i < 16; i++) {
        kprintf("  %llu  %s(%d) -> %s(%d) state=%u\n",
                (unsigned long long)buf[i].timestamp,
                buf[i].prev_comm, buf[i].prev_pid,
                buf[i].next_comm, buf[i].next_pid,
                buf[i].prev_state);
    }
    kprintf("===============================\n");
}

static void cmd_perf_pfstat(void)
{
    kprintf("=== Page Fault Samples ===\n");
    kprintf("(interface: read from perf_pf ring buffer)\n");
    kprintf("  No page fault samples available yet\n");
    kprintf("============================\n");
}

static void cmd_perf_flamegraph(void)
{
    kprintf("=== Flame Graph (Folded Stack Format) ===\n");

    char buf[4096];
    int len = perf_flame_generate(buf, sizeof(buf));
    if (len <= 0) {
        kprintf("  No flame graph data available\n");
        kprintf("=========================================\n");
        return;
    }

    kprintf("%s", buf);
    kprintf("=========================================\n");
}

void cmd_perf(const char *args)
{
    if (!args || args[0] == '\0') {
        cmd_perf_usage();
        return;
    }

    char buf[128];
    strncpy(buf, args, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *p = buf;
    while (*p == ' ') p++;

    if (strcmp(p, "list") == 0 || strcmp(p, "-l") == 0) {
        cmd_perf_list();
    } else if (strcmp(p, "stat") == 0 || strcmp(p, "stats") == 0) {
        cmd_perf_stat();
    } else if (strcmp(p, "cswitch") == 0 || strcmp(p, "cs") == 0) {
        cmd_perf_cswitch();
    } else if (strcmp(p, "pfstat") == 0 || strcmp(p, "pf") == 0) {
        cmd_perf_pfstat();
    } else if (strcmp(p, "flamegraph") == 0 || strcmp(p, "flame") == 0) {
        cmd_perf_flamegraph();
    } else if (strcmp(p, "record") == 0) {
        kprintf("perf record: recording started (stub)\n");
    } else if (strcmp(p, "report") == 0) {
        kprintf("perf report: recording data (stub)\n");
        cmd_perf_stat();
    } else if (strcmp(p, "help") == 0 || strcmp(p, "--help") == 0) {
        cmd_perf_usage();
    } else {
        kprintf("perf: unknown subcommand '%s'\n", p);
        cmd_perf_usage();
    }
}
