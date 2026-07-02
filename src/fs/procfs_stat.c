/*
 * procfs_stat.c — /proc/stat generator
 *
 * Linux-compatible /proc/stat with CPU statistics,
 * context switches, interrupt counts, and boot time.
 * Data sourced from the scheduler, PMM, IDT, timer, and RTC.
 */

#include "types.h"
#include "string.h"
#include "smp.h"
#include "scheduler.h"
#include "process.h"
#include "timer.h"
#include "idt.h"
#include "rtc.h"

/* ── Helpers (extern from procfs.c) ───────────────────────────────── */
extern void proc_u64_to_str(uint64_t v, char *buf, int *pos, int max);
extern void proc_str(const char *s, char *buf, int *pos, int max);

/* ── Sum interrupt counts across all CPUs and all vectors ──────────── */
static uint64_t proc_sum_interrupts(void)
{
	uint64_t total = 0;
	int ncpu = smp_get_cpu_count();
	if (ncpu < 1)
		ncpu = 1;
	for (int cpu = 0; cpu < ncpu; cpu++)
		for (int vec = 0; vec < IDT_NUM_VECTORS; vec++)
			total += idt_get_irq_count(cpu, vec);
	return total;
}

/* ── Main generator ────────────────────────────────────────────────── */
int procfs_gen_stat(char *buf, int max)
{
	int p = 0;

	/* ── Scheduler statistics ─────────────────────────────── */
	struct sched_stats sched_st;
	memset(&sched_st, 0, sizeof(sched_st));
	scheduler_get_stats(&sched_st);

	uint64_t idle_ticks = scheduler_get_idle_ticks();

	/* ── CPU count ─────────────────────────────────────────── */
	int ncpu = smp_get_cpu_count();
	if (ncpu < 1)
		ncpu = 1;

	/* ── Aggregate per-process CPU time ────────────────────── */
	uint64_t user_ticks = 0, system_ticks = 0;
	struct process *table = process_get_table();
	for (int i = 0; i < PROCESS_MAX; i++) {
		if (table[i].state == PROCESS_UNUSED)
			continue;
		user_ticks   += table[i].utime_ticks;
		system_ticks += table[i].stime_ticks;
	}

	/* ── cpu — aggregate line ──────────────────────────────── */
	/* Format: cpu user nice system idle iowait irq softirq steal */
	proc_str("cpu  ", buf, &p, max);
	proc_u64_to_str(user_ticks, buf, &p, max);	/* user   */
	proc_str(" 0 ", buf, &p, max);			/* nice   */
	proc_u64_to_str(system_ticks, buf, &p, max);	/* system */
	proc_str(" ", buf, &p, max);
	proc_u64_to_str(idle_ticks, buf, &p, max);	/* idle   */
	proc_str(" 0 0 0 0\n", buf, &p, max);		/* iowait irq softirq steal */

	/* ── Per-CPU lines: cpu0, cpu1, ... ────────────────────── */
	/* Evenly distribute aggregate ticks across CPUs since we do not
	 * yet have per-CPU idle/user/system accounting. */
	uint64_t per_cpu_idle = idle_ticks / (uint64_t)ncpu;
	uint64_t per_cpu_user = user_ticks / (uint64_t)ncpu;
	uint64_t per_cpu_sys  = system_ticks / (uint64_t)ncpu;
	for (int cpu = 0; cpu < ncpu && cpu < 64; cpu++) {
		proc_str("cpu", buf, &p, max);
		proc_u64_to_str((uint64_t)cpu, buf, &p, max);
		proc_str(" ", buf, &p, max);
		proc_u64_to_str(per_cpu_user, buf, &p, max);	/* user   */
		proc_str(" 0 ", buf, &p, max);			/* nice   */
		proc_u64_to_str(per_cpu_sys, buf, &p, max);	/* system */
		proc_str(" ", buf, &p, max);
		proc_u64_to_str(per_cpu_idle, buf, &p, max);	/* idle   */
		proc_str(" 0 0 0 0\n", buf, &p, max);		/* iowait irq softirq steal */
	}

	/* ── intr — total interrupts ───────────────────────────── */
	proc_str("intr ", buf, &p, max);
	proc_u64_to_str(proc_sum_interrupts(), buf, &p, max);
	proc_str("\n", buf, &p, max);

	/* ── ctxt — context switches ───────────────────────────── */
	proc_str("ctxt ", buf, &p, max);
	proc_u64_to_str(sched_st.context_switches, buf, &p, max);
	proc_str("\n", buf, &p, max);

	/* ── btime — boot time (Unix epoch seconds) ────────────── */
	/* Compute as: current_epoch - elapsed_seconds.
	 * rtc_get_epoch() returns (boot_epoch + elapsed).
	 * Subtracting (timer_get_ticks() / TIMER_FREQ) recovers boot_epoch. */
	proc_str("btime ", buf, &p, max);
	uint64_t now     = rtc_get_epoch();
	uint64_t elapsed = timer_get_ticks() / TIMER_FREQ;
	uint64_t boot_time = (now > elapsed) ? (now - elapsed) : 0;
	proc_u64_to_str(boot_time, buf, &p, max);
	proc_str("\n", buf, &p, max);

	/* ── processes — total created processes ────────────────── */
	proc_str("processes ", buf, &p, max);
	int total_procs = 0;
	for (int i = 0; i < PROCESS_MAX; i++)
		if (table[i].state != PROCESS_UNUSED)
			total_procs++;
	proc_u64_to_str((uint64_t)total_procs, buf, &p, max);
	proc_str("\n", buf, &p, max);

	/* ── procs_running — currently running/ready processes ──── */
	proc_str("procs_running ", buf, &p, max);
	int running = 0;
	for (int i = 0; i < PROCESS_MAX; i++)
		if (table[i].state == PROCESS_RUNNING ||
		    table[i].state == PROCESS_READY)
			running++;
	proc_u64_to_str((uint64_t)running, buf, &p, max);
	proc_str("\n", buf, &p, max);

	/* ── procs_blocked — blocked / waiting processes ───────── */
	proc_str("procs_blocked ", buf, &p, max);
	int blocked = 0;
	for (int i = 0; i < PROCESS_MAX; i++)
		if (table[i].state == PROCESS_BLOCKED)
			blocked++;
	proc_u64_to_str((uint64_t)blocked, buf, &p, max);
	proc_str("\n", buf, &p, max);

	/* ── softirq — total (proxy via all interrupts) ─────────── */
	proc_str("softirq ", buf, &p, max);
	proc_u64_to_str(proc_sum_interrupts(), buf, &p, max);
	proc_str("\n", buf, &p, max);

	buf[p] = '\0';
	return p;
}
