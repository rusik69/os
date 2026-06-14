#ifndef CONTAINER_EXEC_ENHANCED_H
#define CONTAINER_EXEC_ENHANCED_H

#include "types.h"

/*
 * Enhanced container exec — stdin/stdout/stderr multiplexing with
 * terminal resize support.
 *
 * Extends the basic container_exec() with:
 *   - Attach multiple I/O channels (stdin, stdout, stderr)
 *   - Terminal resize via SIGWINCH forwarding
 *   - Detach without affecting the running process
 */

/* ── Constants ──────────────────────────────────────────────────────── */

#define EXEC_CHANNEL_STDIN   0
#define EXEC_CHANNEL_STDOUT  1
#define EXEC_CHANNEL_STDERR  2
#define EXEC_CHANNEL_MAX     3

/* ── Exec attach descriptor ─────────────────────────────────────────── */

struct exec_attach {
    int   pid;               /* PID of the exec'd process */
    int   stdin_fd;          /* FD for writing stdin to the process */
    int   stdout_fd;         /* FD for reading stdout from the process */
    int   stderr_fd;         /* FD for reading stderr from the process */
    int   master_fd;         /* PTY master fd (if using PTY) */
    int   attached;          /* 1 if currently attached */
};

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * container_exec_attach() — Exec a new process in a container and attach
 * multiplexed I/O channels.
 *
 * @container_id: Container ID string.
 * @binary:       Path to the executable inside the container rootfs.
 * @argv:         Argument vector (NULL-terminated).
 * @envp:         Environment vector (NULL-terminated).
 * @use_pty:      Non-zero to allocate a PTY for the process.
 *
 * Returns a pointer to an exec_attach descriptor on success, NULL on error.
 * The descriptor must be freed with container_exec_detach().
 */
struct exec_attach *container_exec_attach(const char *container_id,
                                           const char *binary,
                                           char *const argv[],
                                           char *const envp[],
                                           int use_pty);

/**
 * container_exec_resize() — Send terminal resize event (SIGWINCH).
 *
 * @ea:    Exec attach descriptor from container_exec_attach().
 * @cols:  New terminal width in columns.
 * @rows:  New terminal height in rows.
 *
 * Returns 0 on success, negative on error.
 */
int container_exec_resize(struct exec_attach *ea, uint16_t cols, uint16_t rows);

/**
 * container_exec_detach() — Detach from an exec'd process without
 * killing it.  The process continues running in the background.
 *
 * @ea: Exec attach descriptor to detach and free.
 * Returns 0 on success.
 */
int container_exec_detach(struct exec_attach *ea);

/**
 * container_exec_write_stdin() — Write data to the exec'd process's stdin.
 * @ea:    Exec attach descriptor.
 * @data:  Data buffer.
 * @len:   Number of bytes to write.
 * Returns bytes written, or negative on error.
 */
int container_exec_write_stdin(struct exec_attach *ea,
                                const char *data, size_t len);

/**
 * container_exec_read_stdout() — Read data from the exec'd process's stdout.
 * @ea:    Exec attach descriptor.
 * @buf:   Output buffer.
 * @len:   Buffer size.
 * Returns bytes read, or negative on error.
 */
int container_exec_read_stdout(struct exec_attach *ea,
                                char *buf, size_t len);

/**
 * container_exec_read_stderr() — Read data from the exec'd process's stderr.
 * Returns bytes read, or negative on error.
 */
int container_exec_read_stderr(struct exec_attach *ea,
                                char *buf, size_t len);

#endif /* CONTAINER_EXEC_ENHANCED_H */
