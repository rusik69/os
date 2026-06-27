/* job_control.c — Enhanced job control (fg/bg/jobs/Ctrl+Z)
 *
 * Provides shell job control including:
 *   - Background execution (& suffix)
 *   - job listing (jobs built-in)
 *   - Foreground/background switching (fg/bg)
 *   - Ctrl+Z suspend (SIGTSTP)
 *   - Process group management
 *
 * Integrates with the shell's command execution pipeline
 * and the process scheduler's signal delivery.
 */

#include "types.h"
#include "shell.h"
#include "process.h"
#include "scheduler.h"
#include "printf.h"
#include "string.h"
#include "signal.h"
#include "errno.h"

/* Maximum number of tracked jobs */
#define JOBS_MAX 64

/* Job states */
#define JOB_RUNNING  0
#define JOB_STOPPED  1
#define JOB_DONE     2
#define JOB_TERMINATED 3

/* ── Job table ───────────────────────────────────────────────────── */

struct job_entry {
    int     used;
    int     job_id;        /* 1-based job ID for shell display */
    uint64_t pgid;         /* process group ID (PID of leader) */
    int     state;
    char    command[128];  /* command line for display */
};

static struct job_entry jobs[JOBS_MAX];
static int next_job_id = 1;

/* ── job_control_init ──────────────────────────────────────────────
 *
 * Initialise the job control subsystem.
 */
void job_control_init(void)
{
    memset(jobs, 0, sizeof(jobs));
    next_job_id = 1;
}

/* ── job_control_add ───────────────────────────────────────────────
 *
 * Add a new job to the job table. Returns the job ID.
 */
int job_control_add(uint64_t pgid, const char *command)
{
    for (int i = 0; i < JOBS_MAX; i++) {
        if (!jobs[i].used) {
            jobs[i].used = 1;
            jobs[i].job_id = next_job_id++;
            jobs[i].pgid = pgid;
            jobs[i].state = JOB_RUNNING;
            strncpy(jobs[i].command, command ? command : "",
                    sizeof(jobs[i].command) - 1);
            jobs[i].command[sizeof(jobs[i].command) - 1] = '\0';
            return jobs[i].job_id;
        }
    }
    return -1; /* table full */
}

/* ── job_control_remove ────────────────────────────────────────────
 *
 * Remove a job from the table by job ID.
 */
void job_control_remove(int job_id)
{
    for (int i = 0; i < JOBS_MAX; i++) {
        if (jobs[i].used && jobs[i].job_id == job_id) {
            jobs[i].used = 0;
            return;
        }
    }
}

/* ── job_control_find_by_pgid ──────────────────────────────────────
 *
 * Find a job by process group ID.
 */
struct job_entry *job_control_find_by_pgid(uint64_t pgid)
{
    for (int i = 0; i < JOBS_MAX; i++) {
        if (jobs[i].used && jobs[i].pgid == pgid)
            return &jobs[i];
    }
    return NULL;
}

/* ── job_control_find_by_id ────────────────────────────────────────
 *
 * Find a job by job ID.
 */
struct job_entry *job_control_find_by_id(int job_id)
{
    for (int i = 0; i < JOBS_MAX; i++) {
        if (jobs[i].used && jobs[i].job_id == job_id)
            return &jobs[i];
    }
    return NULL;
}

/* ── job_control_update_state ──────────────────────────────────────
 *
 * Update the state of a job (e.g., JOB_RUNNING → JOB_STOPPED).
 */
void job_control_update_state(uint64_t pgid, int state)
{
    struct job_entry *job = job_control_find_by_pgid(pgid);
    if (job)
        job->state = state;
}

/* ── job_control_list ──────────────────────────────────────────────
 *
 * Print the list of all tracked jobs (for the 'jobs' built-in).
 */
void job_control_list(void)
{
    kprintf("JOB  STATUS   PGID   COMMAND\n");
    kprintf("---  ------   ----   -------\n");
    for (int i = 0; i < JOBS_MAX; i++) {
        if (!jobs[i].used)
            continue;
        const char *state_str = "unknown";
        switch (jobs[i].state) {
        case JOB_RUNNING:     state_str = "running";   break;
        case JOB_STOPPED:     state_str = "stopped";   break;
        case JOB_DONE:        state_str = "done";      break;
        case JOB_TERMINATED:  state_str = "terminated"; break;
        default: break;
        }
        kprintf("[%d]  %-8s  %-6llu  %s\n",
               jobs[i].job_id, state_str,
               (unsigned long long)jobs[i].pgid,
               jobs[i].command);
    }
}

/* ── job_control_fg ────────────────────────────────────────────────
 *
 * Bring a job to the foreground: continue it if stopped, then
 * wait for it to complete or stop again.
 *
 * Proper terminal handling: sets the foreground process group
 * of the terminal before continuing the job.
 */
int job_control_fg(int job_id)
{
    struct job_entry *job = job_control_find_by_id(job_id);
    if (!job) {
        kprintf("fg: job not found: %d\n", job_id);
        return -1;
    }

    /* Set terminal foreground process group */
    extern void pgrp_set_foreground(uint64_t pgid);
    pgrp_set_foreground(job->pgid);

    if (job->state == JOB_STOPPED) {
        /* Send SIGCONT to the process group */
        signal_send_pgid((uint32_t)job->pgid, SIGCONT);
        job->state = JOB_RUNNING;
        kprintf("[%d] continued %s\n", job->job_id, job->command);
    }

    /* Wait for the job to complete */
    int status;
    while (1) {
        uint64_t waited = process_waitpid((uint32_t)job->pgid, &status);
        if (waited == (uint64_t)-1)
            break;
        if (waited == (uint64_t)job->pgid || waited == 0)
            break;
    }

    /* Restore shell as foreground process group */
    struct process *cur = process_get_current();
    if (cur) pgrp_set_foreground(cur->pgid);

    /* Notify job termination status */
    if (status != 0) {
        kprintf("[%d]+ done %s (status %d)\n", job->job_id, job->command, status);
    } else {
        kprintf("[%d]+ done %s\n", job->job_id, job->command);
    }

    job_control_remove(job_id);
    return 0;
}

/* ── job_control_bg ────────────────────────────────────────────────
 *
 * Put a stopped job into the background (continue it).
 */
int job_control_bg(int job_id)
{
    struct job_entry *job = job_control_find_by_id(job_id);
    if (!job) {
        kprintf("bg: job not found: %d\n", job_id);
        return -1;
    }

    if (job->state == JOB_STOPPED) {
        signal_send_pgid((uint32_t)job->pgid, SIGCONT);
        job->state = JOB_RUNNING;
        kprintf("[%d] %s &\n", job->job_id, job->command);
    }

    return 0;
}

/* ── job_control_handle_suspend ────────────────────────────────────
 *
 * Called when a foreground process receives SIGTSTP (Ctrl+Z).
 * Marks the job as stopped so the shell can manage it.
 * Restores terminal foreground process group to the shell.
 */
void job_control_handle_suspend(uint64_t pgid)
{
    struct job_entry *job = job_control_find_by_pgid(pgid);
    if (job) {
        job->state = JOB_STOPPED;
        kprintf("\n[%d]+ suspended %s\n", job->job_id, job->command);

        /* Restore shell as foreground process group */
        struct process *cur = process_get_current();
        if (cur) {
            extern void pgrp_set_foreground(uint64_t pgid);
            pgrp_set_foreground(cur->pgid);
        }
    }
}

/* ── job_control_cleanup ───────────────────────────────────────────
 *
 * Remove completed/terminated jobs from the table.
 */
void job_control_cleanup(void)
{
    for (int i = 0; i < JOBS_MAX; i++) {
        if (jobs[i].used &&
            (jobs[i].state == JOB_DONE || jobs[i].state == JOB_TERMINATED)) {
            jobs[i].used = 0;
        }
    }
}
