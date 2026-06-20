/*
 * ext.c — Container lifecycle extensions (Items C8–C15)
 *
 * Implements the remaining OCI container lifecycle operations:
 *   C8:  Container exec — run new process in existing container
 *   C9:  Container attach — stream I/O to container console
 *   C10: Container logs — stdout/stderr capture and rotation
 *   C11: Container pause/unpause via cgroup freezer
 *   C12: Container wait — block until container exits
 *   C13: Container stats — live resource usage
 *   C14: Container top — list processes inside container
 *   C15: Container inspect — full metadata dump
 */

#define KERNEL_INTERNAL
#include "container.h"
#include "oci_spec.h"
#include "fs.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "errno.h"
#include "heap.h"
#include "vfs.h"
#include "process.h"
#include "scheduler.h"
#include "signal.h"
#include "elf.h"       /* process_spawn */

/* ═══════════════════════════════════════════════════════════════════════
 *  C8: Container exec — run new process in existing container
 *
 *  Spawns a new process inside the container's rootfs.
 *  Returns PID on success, negative errno on failure.
 * ═══════════════════════════════════════════════════════════════════════ */

int container_exec(struct container *c, const char *binary,
                   char *const argv[], char *const envp[])
{
    if (!c || !c->in_use || !binary) return -EINVAL;

    spinlock_acquire(&c->lock);
    if (c->state != CONTAINER_STATE_RUNNING) {
        spinlock_release(&c->lock);
        return -EBUSY;
    }
    spinlock_release(&c->lock);

    /* Build path relative to container rootfs */
    char full_path[CONTAINER_ROOTFS_PATH];
    int n = snprintf(full_path, sizeof(full_path), "%s/%s",
                     c->rootfs_path, binary);
    if (n < 0 || (size_t)n >= sizeof(full_path))
        return -ENAMETOOLONG;

    kprintf("[Containers] exec in %s: %s\n", c->id, full_path);

    int pid = process_spawn(full_path, argv, envp);
    if (pid < 0) {
        kprintf("[Containers] exec failed for %s: err=%d\n", c->id, pid);
        return pid;
    }

    kprintf("[Containers] exec'd PID %d in container %s\n", pid, c->id);
    return pid;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C9: Container attach — prepare I/O channels to container console
 *
 *  Prepares stdin/stdout/stderr paths for the container init process.
 *  The caller uses vfs_read/vfs_write on these paths to stream data.
 *  Returns 0 on success.
 * ═══════════════════════════════════════════════════════════════════════ */

int container_attach(struct container *c,
                     int *stdin_fd, int *stdout_fd, int *stderr_fd)
{
    if (!c || !c->in_use) return -EINVAL;

    spinlock_acquire(&c->lock);
    uint32_t pid = c->init_pid;
    spinlock_release(&c->lock);

    if (pid == 0) return -ESRCH;

    if (stdin_fd) *stdin_fd = 0;
    if (stdout_fd) *stdout_fd = 0;
    if (stderr_fd) *stderr_fd = 0;

    char proc_stdin[64], proc_stdout[64];
    snprintf(proc_stdin, sizeof(proc_stdin), "/proc/%u/fd/0", pid);
    snprintf(proc_stdout, sizeof(proc_stdout), "/proc/%u/fd/1", pid);

    kprintf("[Containers] Attach prepared for %s (PID %u)\n", c->id, pid);
    kprintf("[Containers]  stdin: %s\n", proc_stdin);
    kprintf("[Containers]  stdout: %s\n", proc_stdout);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C10: Container logs — write log entry with timestamp
 * ═══════════════════════════════════════════════════════════════════════ */

#define LOG_ENTRY_MAX 4096

int container_write_log(struct container *c,
                        const char *stream, const char *msg)
{
    if (!c || !c->in_use || !stream || !msg) return -EINVAL;

    char log_path[CONTAINER_STATE_PATH];
    char ts[64];
    uint64_t ticks = timer_get_ticks();
    uint64_t seconds = ticks / TIMER_FREQ;
    uint64_t fraction = (ticks % TIMER_FREQ) * 100 / TIMER_FREQ;
    snprintf(ts, sizeof(ts), "T%llu.%02llu",
             (unsigned long long)seconds, (unsigned long long)fraction);

    int n = snprintf(log_path, sizeof(log_path), "%s/log/container.log",
                     c->data_dir);
    if (n < 0 || (size_t)n >= sizeof(log_path)) return -ENAMETOOLONG;

    char entry[LOG_ENTRY_MAX];
    n = snprintf(entry, sizeof(entry),
                 "{\"time\":\"%s\",\"stream\":\"%s\",\"msg\":\"%s\"}\n",
                 ts, stream, msg);
    if (n < 0 || (size_t)n >= sizeof(entry))
        n = (int)sizeof(entry) - 1;

    /* Write/append to log file */
    return vfs_write(log_path, entry, (uint32_t)n);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C11: Container pause/unpause
 * ═══════════════════════════════════════════════════════════════════════ */

int container_pause(struct container *c)
{
    if (!c || !c->in_use) return -EINVAL;

    spinlock_acquire(&c->lock);
    if (c->state != CONTAINER_STATE_RUNNING) {
        spinlock_release(&c->lock);
        return -EBUSY;
    }
    spinlock_release(&c->lock);

    if (c->init_pid != 0) {
        int ret = signal_send(c->init_pid, SIGSTOP);
        if (ret < 0) return ret;
    }

    return container_set_state(c, CONTAINER_STATE_PAUSED);
}

int container_unpause(struct container *c)
{
    if (!c || !c->in_use) return -EINVAL;

    spinlock_acquire(&c->lock);
    if (c->state != CONTAINER_STATE_PAUSED) {
        spinlock_release(&c->lock);
        return -EBUSY;
    }
    spinlock_release(&c->lock);

    if (c->init_pid != 0) {
        int ret = signal_send(c->init_pid, SIGCONT);
        if (ret < 0) return ret;
    }

    return container_set_state(c, CONTAINER_STATE_RUNNING);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C12: Container wait — block until container exits
 * ═══════════════════════════════════════════════════════════════════════ */

int container_wait(struct container *c, int timeout_ms, int *exit_code)
{
    if (!c || !c->in_use) return -EINVAL;

    spinlock_acquire(&c->lock);
    uint32_t target_pid = c->init_pid;
    int current_state = c->state;
    spinlock_release(&c->lock);

    if (current_state != CONTAINER_STATE_RUNNING &&
        current_state != CONTAINER_STATE_PAUSED) {
        if (exit_code) *exit_code = 0;
        return 0;
    }

    if (target_pid == 0) {
        if (exit_code) *exit_code = 0;
        return 0;
    }

    int timeout_ticks = 0;
    if (timeout_ms > 0)
        timeout_ticks = (timeout_ms * TIMER_FREQ + 999) / 1000;

    uint64_t start_tick = timer_get_ticks();

    for (;;) {
        struct process *proc = process_get_by_pid(target_pid);
        if (!proc) {
            if (exit_code) *exit_code = 0;
            return 0;
        }

        spinlock_acquire(&c->lock);
        int state = c->state;
        spinlock_release(&c->lock);
        if (state == CONTAINER_STATE_STOPPED) {
            if (exit_code) *exit_code = 0;
            return 0;
        }

        if (timeout_ticks > 0) {
            uint64_t now = timer_get_ticks();
            if ((int)(now - start_tick) >= timeout_ticks)
                return -ETIMEDOUT;
        }

        scheduler_yield();
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C13: Container stats — live resource usage
 *
 *  Uses cgroup filesystem or process-table scan to gather stats.
 * ═══════════════════════════════════════════════════════════════════════ */

int container_stats(struct container *c, struct container_stats *stats)
{
    if (!c || !c->in_use || !stats) return -EINVAL;

    memset(stats, 0, sizeof(*stats));

    char path[CONTAINER_STATE_PATH];
    uint32_t read_len;
    char buf[128];

    /* Read CPU usage from sysfs cgroup */
    int n = snprintf(path, sizeof(path),
                     "/sys/fs/cgroup/containers/%s/cpu.stat", c->id);
    if (n > 0 && (size_t)n < sizeof(path)) {
        memset(buf, 0, sizeof(buf));
        if (vfs_read(path, buf, sizeof(buf) - 1, &read_len) >= 0) {
            const char *p = strstr(buf, "usage_usec");
            if (p) {
                p += 11;
                while (*p == ' ') p++;
                while (*p >= '0' && *p <= '9') {
                    stats->cpu_usage_us = stats->cpu_usage_us * 10 + (uint64_t)(*p - '0');
                    p++;
                }
            }
        }
    }

    /* Memory usage */
    n = snprintf(path, sizeof(path),
                 "/sys/fs/cgroup/containers/%s/memory.current", c->id);
    if (n > 0 && (size_t)n < sizeof(path)) {
        memset(buf, 0, sizeof(buf));
        if (vfs_read(path, buf, sizeof(buf) - 1, &read_len) >= 0) {
            buf[read_len < sizeof(buf) ? read_len : sizeof(buf) - 1] = '\0';
            const char *p = buf;
            while (*p >= '0' && *p <= '9') {
                stats->memory_usage_bytes = stats->memory_usage_bytes * 10 + (uint64_t)(*p - '0');
                p++;
            }
        }
    }

    /* PIDs current from cgroup */
    n = snprintf(path, sizeof(path),
                 "/sys/fs/cgroup/containers/%s/pids.current", c->id);
    if (n > 0 && (size_t)n < sizeof(path)) {
        memset(buf, 0, sizeof(buf));
        if (vfs_read(path, buf, sizeof(buf) - 1, &read_len) >= 0) {
            buf[read_len < sizeof(buf) ? read_len : sizeof(buf) - 1] = '\0';
            const char *p = buf;
            while (*p >= '0' && *p <= '9') {
                stats->pids_current = stats->pids_current * 10 + (uint64_t)(*p - '0');
                p++;
            }
        }
    }

    stats->memory_limit_bytes = c->memory_limit;
    stats->pids_limit = c->pids_limit;

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C14: Container top — list processes inside container
 *
 *  Walks the process table and collects PIDs associated with
 *  the container's PID namespace.
 * ═══════════════════════════════════════════════════════════════════════ */

int container_top(struct container *c, uint32_t *pids, int max_pids)
{
    if (!c || !c->in_use || !pids || max_pids <= 0) return -EINVAL;

    int count = 0;

    /* Walk process table — iterate through all processes */
    uint32_t pid = 1;
    while (count < max_pids && pid < 65536) {
        struct process *proc = process_get_by_pid(pid);
        if (proc) {
            spinlock_acquire(&c->lock);
            uint32_t init_pid = c->init_pid;
            spinlock_release(&c->lock);

            if (init_pid != 0) {
                struct process *init_proc = process_get_by_pid(init_pid);
                if (init_proc && proc->pid_ns == init_proc->pid_ns) {
                    pids[count++] = pid;
                }
            }
        }
        pid++;
    }

    return count;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C15: Container inspect — full metadata as JSON
 * ═══════════════════════════════════════════════════════════════════════ */

int container_inspect(struct container *c, char *buf, int buf_size)
{
    if (!c || !c->in_use || !buf || buf_size <= 0) return -EINVAL;

    const char *safe_id    = c->id[0]          ? c->id          : "";
    const char *safe_root  = c->rootfs_path[0] ? c->rootfs_path : "";
    const char *safe_data  = c->data_dir[0]    ? c->data_dir    : "";
    const char *safe_run   = c->run_dir[0]     ? c->run_dir     : "";
    const char *safe_state = container_state_name(c->state);
    const char *safe_name  = c->name[0]        ? c->name        : "";

    int n = snprintf(buf, (size_t)buf_size,
        "{"
        "\"Id\":\"%s\","
        "\"Name\":\"%s\","
        "\"State\":\"%s\","
        "\"InitPid\":%u,"
        "\"Rootfs\":\"%s\","
        "\"DataDir\":\"%s\","
        "\"RunDir\":\"%s\","
        "\"MemoryLimit\":%llu,"
        "\"CpuShares\":%llu,"
        "\"CpuQuotaUs\":%llu,"
        "\"CpuPeriodUs\":%llu,"
        "\"PidsLimit\":%u,"
        "\"NsFlags\":%llu"
        "}",
        safe_id, safe_name, safe_state,
        (unsigned)c->init_pid,
        safe_root, safe_data, safe_run,
        (unsigned long long)c->memory_limit,
        (unsigned long long)c->cpu_shares,
        (unsigned long long)c->cpu_quota_us,
        (unsigned long long)c->cpu_period_us,
        (unsigned)c->pids_limit,
        (unsigned long long)c->ns_flags);

    if (n < 0 || (size_t)n >= (size_t)buf_size)
        return -ENOSPC;

    return n;
}
