#include "pidfd.h"
#include "process.h"
#include "printf.h"
#include "string.h"
#include "errno.h"
#include "kernel.h"

/*
 * Static pidfd table.
 * Each entry associates a file descriptor number with a PID.
 */
struct pidfd_entry {
    uint32_t pid;           /* target process PID (0 = free) */
    int      refcount;      /* number of references */
    int      fd;            /* fd number (table index) */
};

static struct pidfd_entry pidfd_table[PIDFD_MAX];
static int                pidfd_initialised;

void pidfd_init(void)
{
    if (pidfd_initialised)
        return;

    memset(pidfd_table, 0, sizeof(pidfd_table));
    pidfd_initialised = 1;

    kprintf("[OK] pidfd: process file descriptors initialised (%d slots)\n", PIDFD_MAX);
}

static int pidfd_find_free(void)
{
    for (int i = 0; i < PIDFD_MAX; i++) {
        if (pidfd_table[i].pid == 0)
            return i;
    }
    return -EMFILE;
}

static int pidfd_find_by_pid(uint32_t pid)
{
    for (int i = 0; i < PIDFD_MAX; i++) {
        if (pidfd_table[i].pid == pid)
            return i;
    }
    return -ESRCH;
}

int pidfd_open(uint32_t pid, uint32_t flags)
{
    (void)flags;

    /* Validate that the process exists */
    struct process *proc = process_get_by_pid(pid);
    if (!proc)
        return -ESRCH;

    /* Check if a pidfd for this PID already exists */
    int idx = pidfd_find_by_pid(pid);
    if (idx >= 0) {
        pidfd_table[idx].refcount++;
        return pidfd_table[idx].fd;
    }

    /* Allocate a new entry */
    idx = pidfd_find_free();
    if (idx < 0)
        return idx;

    pidfd_table[idx].pid      = pid;
    pidfd_table[idx].refcount = 1;
    pidfd_table[idx].fd       = idx;

    return idx;
}

int pidfd_send_signal(int pidfd, int sig, struct siginfo *info, uint32_t flags)
{
    (void)flags;

    if (pidfd < 0 || pidfd >= PIDFD_MAX)
        return -EBADF;

    struct pidfd_entry *entry = &pidfd_table[pidfd];
    if (entry->pid == 0)
        return -EBADF;

    /* Validate signal number */
    if (sig < 1 || sig > 64)
        return -EINVAL;

    /* Find the target process */
    struct process *target = process_get_by_pid(entry->pid);
    if (!target) {
        /* Process exited since we opened the pidfd — mark it */
        entry->pid = 0;
        return -ESRCH;
    }

    /* Send the signal using the kernel's signal infrastructure */
    if (info) {
        return signal_send_info(entry->pid, sig, info);
    } else {
        return signal_send(entry->pid, sig);
    }
}

/* Release a reference to a pidfd */
void pidfd_put(int pidfd)
{
    if (pidfd < 0 || pidfd >= PIDFD_MAX)
        return;

    struct pidfd_entry *entry = &pidfd_table[pidfd];
    if (entry->pid == 0)
        return;

    entry->refcount--;
    if (entry->refcount <= 0) {
        entry->pid  = 0;
        entry->fd   = -1;
        entry->refcount = 0;
    }
}

/*
 * pidfd_getfd — obtain a duplicate of another process's file descriptor.
 *
 * Given a pidfd referring to a target process, and a target_fd number
 * within that process, returns a new file descriptor in the calling
 * process that refers to the same open file description.
 *
 * This is used by debuggers and container managers to access file
 * descriptors in other processes (/proc/PID/fd equivalent via pidfd).
 */
int pidfd_getfd(int pidfd, int target_fd, uint32_t flags)
{
    if (flags != 0)
        return -EINVAL;

    /* Validate the pidfd */
    if (pidfd < 0 || pidfd >= PIDFD_MAX)
        return -EBADF;

    struct pidfd_entry *entry = &pidfd_table[pidfd];
    if (entry->pid == 0)
        return -EBADF;

    /* Find the target process */
    struct process *target = process_get_by_pid(entry->pid);
    if (!target) {
        entry->pid = 0; /* process exited */
        return -ESRCH;
    }

    /* Validate target_fd */
    if (target_fd < 0 || target_fd >= PROCESS_FD_MAX)
        return -EBADF;

    /* Check that the target fd is actually in use */
    if (!target->fd_table[target_fd].used)
        return -EBADF;

    /* Find a free fd in the calling process */
    struct process *current = process_get_current();
    if (!current)
        return -EPERM;

    uint64_t max_fds = current->file_max > 0 ? current->file_max : PROCESS_FD_MAX;
    int new_fd = -1;
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!current->fd_table[i].used) {
            if ((uint64_t)i >= max_fds)
                return -EMFILE;
            new_fd = i;
            break;
        }
    }

    if (new_fd < 0)
        return -EMFILE;

    /* Copy the fd entry from target to current process */
    current->fd_table[new_fd] = target->fd_table[target_fd];

    return new_fd; /* Note: returned directly, not fd-3 offset */
}
