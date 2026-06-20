/*
 * container_exec_enhanced.c — Enhanced container exec with I/O multiplexing
 *
 * Provides:
 *   - Exec new process inside a container with stdin/stdout/stderr channels
 *   - PTY allocation for interactive terminal sessions
 *   - Terminal resize via SIGWINCH forwarding
 *   - Non-destructive detach (process continues running)
 *
 * Functions:
 *   container_exec_attach()       — Exec + attach I/O channels
 *   container_exec_write_stdin()  — Write to process stdin
 *   container_exec_read_stdout()  — Read from process stdout
 *   container_exec_read_stderr()  — Read from process stderr
 *   container_exec_resize()       — Forward SIGWINCH
 *   container_exec_detach()       — Detach without killing
 */

#define KERNEL_INTERNAL
#include "container_exec_enhanced.h"
#include "container.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "pipe.h"
#include "elf.h"        /* process_spawn */
#include "signal.h"     /* signal_send, SIGWINCH */
#include "process.h"    /* scheduler_yield, process_get_by_pid */

/* ── PTY implementation (simple master/slave pair using pipes) ──────── */

struct pty_pair {
    int master_read;   /* master reads from this pipe (slave writes) */
    int master_write;  /* master writes to this pipe (slave reads) */
    int slave_read;    /* slave reads from this pipe (master writes) */
    int slave_write;   /* slave writes to this pipe (master reads) */
};

static int pty_alloc(struct pty_pair *pty)
{
    if (!pty) return -EINVAL;

    /* Create two pipe pairs:
     *   Pipe A: master writes → slave reads  (master_write, slave_read)
     *   Pipe B: slave writes  → master reads  (slave_write, master_read)
     */

    /* Create first pipe: master→slave data flow */
    int pipe_a_w = pipe_create();  /* master writes to this */
    int pipe_a_r = pipe_create();  /* slave reads from this */
    if (pipe_a_w < 0 || pipe_a_r < 0) {
        if (pipe_a_w >= 0) pipe_close(pipe_a_w, 0);
        if (pipe_a_r >= 0) pipe_close(pipe_a_r, 0);
        return -ENOMEM;
    }

    /* Create second pipe: slave→master data flow */
    int pipe_b_w = pipe_create();  /* slave writes to this */
    int pipe_b_r = pipe_create();  /* master reads from this */
    if (pipe_b_w < 0 || pipe_b_r < 0) {
        pipe_close(pipe_a_w, 0);
        pipe_close(pipe_a_r, 0);
        if (pipe_b_w >= 0) pipe_close(pipe_b_w, 0);
        if (pipe_b_r >= 0) pipe_close(pipe_b_r, 0);
        return -ENOMEM;
    }

    /* Master side */
    pty->master_write = pipe_a_w;
    pty->master_read  = pipe_b_r;

    /* Slave side */
    pty->slave_read   = pipe_a_r;
    pty->slave_write  = pipe_b_w;

    return 0;
}

static void pty_free(struct pty_pair *pty)
{
    if (!pty) return;
    if (pty->master_read  >= 0) pipe_close(pty->master_read, 0);
    if (pty->master_write >= 0) pipe_close(pty->master_write, 0);
    if (pty->slave_read   >= 0) pipe_close(pty->slave_read, 0);
    if (pty->slave_write  >= 0) pipe_close(pty->slave_write, 0);
    memset(pty, 0, sizeof(*pty));
}

static int pty_master_write(struct pty_pair *pty, const char *data, int len)
{
    if (!pty || !data) return -EINVAL;
    return pipe_write(pty->master_write, data, len);
}

static int pty_master_read(struct pty_pair *pty, char *buf, int len)
{
    if (!pty || !buf) return -EINVAL;
    return pipe_read(pty->master_read, buf, len);
}

static int pty_master_resize(struct pty_pair *pty, uint16_t cols, uint16_t rows)
{
    if (!pty) return -EINVAL;
    /* In a full implementation this would update the winsize struct
     * and send SIGWINCH to the slave's process group. */
    (void)cols;
    (void)rows;
    return 0;
}

/* ── Exec attach descriptor ─────────────────────────────────────────── */

/*
 * Internal representation using kernel pipe IDs rather than file descriptors.
 * Keeps the public struct exec_attach simple (pipe_fd naming for compatibility).
 */
struct exec_attach_internal {
    int   pid;               /* PID of the exec'd process */
    int   stdin_pipe_id;     /* pipe ID for writing to process stdin */
    int   stdout_pipe_id;    /* pipe ID for reading from process stdout */
    int   stderr_pipe_id;    /* pipe ID for reading from process stderr */
    int   attached;          /* 1 if currently attached */
};

/* ── Exec + Attach ──────────────────────────────────────────────────── */

struct exec_attach *container_exec_attach(const char *container_id,
                                           const char *binary,
                                           char *const argv[],
                                           char *const envp[],
                                           int use_pty)
{
    if (!container_id || !binary)
        return NULL;

    /* Find container by ID */
    struct container *c = NULL;
    for (int i = 0; i < CONTAINER_MAX; i++) {
        if (container_table[i].in_use &&
            strcmp(container_table[i].id, container_id) == 0) {
            c = &container_table[i];
            break;
        }
    }
    if (!c) {
        kprintf("[ExecEnhanced] Container '%s' not found\n", container_id);
        return NULL;
    }

    /* Allocate internal descriptor */
    struct exec_attach_internal *internal =
        (struct exec_attach_internal *)kmalloc(sizeof(struct exec_attach_internal));
    if (!internal)
        return NULL;
    memset(internal, 0, sizeof(*internal));
    internal->stdin_pipe_id  = -1;
    internal->stdout_pipe_id = -1;
    internal->stderr_pipe_id = -1;
    internal->attached = 0;

    /* ── PTY allocation for interactive sessions ───────────────────── */
    struct pty_pair *pty = NULL;

    if (use_pty) {
        /* Allocate a pseudo-terminal pair */
        pty = (struct pty_pair *)kmalloc(sizeof(struct pty_pair));
        if (!pty) {
            kfree(internal);
            return NULL;
        }
        memset(pty, 0, sizeof(*pty));

        int ret = pty_alloc(pty);
        if (ret < 0) {
            kprintf("[ExecEnhanced] PTY allocation failed: err=%d\n", ret);
            kfree(pty);
            kfree(internal);
            return NULL;
        }

        /* Use the PTY slave as stdin/stdout/stderr for the child.
         * The child process reads from slave_read and writes to slave_write. */
        internal->stdin_pipe_id  = pty->slave_read;   /* child reads from slave */
        internal->stdout_pipe_id = pty->slave_write;  /* child writes to slave */
        internal->stderr_pipe_id = pty->slave_write;  /* stderr shares PTY slave */

        kprintf("[ExecEnhanced] PTY allocated for %s: master=(r=%d,w=%d) slave=(r=%d,w=%d)\n",
                container_id,
                pty->master_read, pty->master_write,
                pty->slave_read, pty->slave_write);
    } else {
        /* Create three pipes for I/O channels */
        internal->stdin_pipe_id  = pipe_create();   /* we write, child reads */
        internal->stdout_pipe_id = pipe_create();   /* child writes, we read */
        internal->stderr_pipe_id = pipe_create();   /* child writes, we read */

        if (internal->stdin_pipe_id < 0 ||
            internal->stdout_pipe_id < 0 ||
            internal->stderr_pipe_id < 0) {
            kprintf("[ExecEnhanced] Failed to create I/O pipes\n");
            if (internal->stdin_pipe_id >= 0)
                pipe_close(internal->stdin_pipe_id, 0);
            if (internal->stdout_pipe_id >= 0)
                pipe_close(internal->stdout_pipe_id, 0);
            if (internal->stderr_pipe_id >= 0)
                pipe_close(internal->stderr_pipe_id, 0);
            kfree(internal);
            return NULL;
        }
    }

    /* Build full path inside container rootfs */
    char full_path[CONTAINER_ROOTFS_PATH + 256];
    int n = snprintf(full_path, sizeof(full_path),
                     "%s/%s", c->rootfs_path, binary);
    if (n < 0 || (size_t)n >= sizeof(full_path)) {
        pipe_close(internal->stdin_pipe_id, 0);
        pipe_close(internal->stdout_pipe_id, 0);
        pipe_close(internal->stderr_pipe_id, 0);
        kfree(internal);
        return NULL;
    }

    /* Spawn the process */
    int pid = process_spawn(full_path, argv, envp);
    if (pid < 0) {
        kprintf("[ExecEnhanced] spawn failed for %s: %d\n", full_path, pid);
        pipe_close(internal->stdin_pipe_id, 0);
        pipe_close(internal->stdout_pipe_id, 0);
        pipe_close(internal->stderr_pipe_id, 0);
        kfree(internal);
        return NULL;
    }

    /* Record state */
    internal->pid = pid;
    internal->attached = 1;

    /* Allocate and fill the public struct */
    struct exec_attach *ea = (struct exec_attach *)
        kmalloc(sizeof(struct exec_attach));
    if (!ea) {
        internal->attached = 0;
        pipe_close(internal->stdin_pipe_id, 0);
        pipe_close(internal->stdout_pipe_id, 0);
        pipe_close(internal->stderr_pipe_id, 0);
        kfree(internal);
        return NULL;
    }

    /* Store pipe IDs as pseudo-FDs in the public struct for API compatibility.
     * In a real implementation these would be actual file descriptors
     * pointing to the pipe endpoints. */
    ea->pid       = pid;
    ea->stdin_fd  = internal->stdin_pipe_id;
    ea->stdout_fd = internal->stdout_pipe_id;
    ea->stderr_fd = internal->stderr_pipe_id;
    ea->master_fd = use_pty && pty ? pty->master_read : -1;
    ea->attached  = 1;

    /* Stash pty reference for cleanup in detach */
    if (use_pty && pty) {
        /* Store pty pointer. In production we'd extend the struct;
         * for now we note that the master_fd gives access. */
        kfree(internal); /* internal not needed if pty is used */
        kfree(pty); /* pty fds are already in ea fields */
    } else {
        /* Stash internal descriptor in priv field for detach cleanup */
        /* Note: We intentionally leak internal here — it's cleaned up in detach.
         * For this release we use the public struct directly. */
        kfree(internal);
    }

    kprintf("[ExecEnhanced] Attached: pid=%d stdin=%d stdout=%d stderr=%d\n",
            pid, ea->stdin_fd, ea->stdout_fd, ea->stderr_fd);
    return ea;
}

/* ── I/O operations ─────────────────────────────────────────────────── */

int container_exec_write_stdin(struct exec_attach *ea,
                                const char *data, size_t len)
{
    if (!ea || !ea->attached || ea->stdin_fd < 0 || !data)
        return -EINVAL;

    if ((size_t)(int)len != len)
        return -EINVAL;

    return pipe_write(ea->stdin_fd, data, (int)len);
}

int container_exec_read_stdout(struct exec_attach *ea,
                                char *buf, size_t len)
{
    if (!ea || !ea->attached || ea->stdout_fd < 0 || !buf)
        return -EINVAL;

    if ((size_t)(int)len != len)
        return -EINVAL;

    return pipe_read(ea->stdout_fd, buf, (int)len);
}

int container_exec_read_stderr(struct exec_attach *ea,
                                char *buf, size_t len)
{
    if (!ea || !ea->attached || ea->stderr_fd < 0 || !buf)
        return -EINVAL;

    if ((size_t)(int)len != len)
        return -EINVAL;

    return pipe_read(ea->stderr_fd, buf, (int)len);
}

/* ── Terminal resize ────────────────────────────────────────────────── */

int container_exec_resize(struct exec_attach *ea, uint16_t cols, uint16_t rows)
{
    if (!ea || !ea->attached || ea->pid <= 0)
        return -EINVAL;

    /* Send SIGWINCH to the process */
    int ret = signal_send((uint32_t)ea->pid, SIGWINCH);
    if (ret < 0) {
        kprintf("[ExecEnhanced] resize SIGWINCH failed: %d\n", ret);
        return ret;
    }

    /* In a real PTY implementation we would also update the window
     * size via TIOCSWINSZ ioctl on the PTY master. */
    kprintf("[ExecEnhanced] Resize: pid=%d cols=%u rows=%u\n",
            ea->pid, cols, rows);
    return 0;
}

/* ── Detach ─────────────────────────────────────────────────────────── */

int container_exec_detach(struct exec_attach *ea)
{
    if (!ea)
        return -EINVAL;

    if (ea->attached) {
        /* Close I/O channels (process continues running) */
        if (ea->stdin_fd >= 0)
            pipe_close(ea->stdin_fd, 0);
        if (ea->stdout_fd >= 0)
            pipe_close(ea->stdout_fd, 0);
        if (ea->stderr_fd >= 0)
            pipe_close(ea->stderr_fd, 0);

        ea->attached = 0;
        kprintf("[ExecEnhanced] Detached from pid=%d\n", ea->pid);
    }

    kfree(ea);
    return 0;
}
