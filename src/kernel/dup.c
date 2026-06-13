/* dup.c — dup, dup2, dup3 syscalls for fd duplication */

#include "syscall_dup.h"
#include "process.h"
#include "errno.h"
#include "string.h"

/* Internal fd table access helpers */
extern int fd_find_free(struct process *proc);
extern struct process_fd *sys_get_fd(int i);

int sys_dup_impl(int old_fd)
{
    struct process *proc = process_get_current();
    if (!proc) return -1;
    if (old_fd >= PROCESS_FD_MAX || !proc->fd_table[old_fd].used)
        return -1;

    int new_fd = fd_find_free(proc);
    if (new_fd < 0) return -1;

    proc->fd_table[new_fd] = proc->fd_table[old_fd];
    proc->fd_table[new_fd].offset = proc->fd_table[old_fd].offset;
    return new_fd;
}

int sys_dup2_impl(int old_fd, int new_fd)
{
    struct process *proc = process_get_current();
    if (!proc) return -1;
    if (old_fd >= PROCESS_FD_MAX || !proc->fd_table[old_fd].used)
        return -1;
    if (new_fd >= PROCESS_FD_MAX) return -1;

    /* If old_fd == new_fd, just return it */
    if (old_fd == new_fd) return new_fd;

    /* Close new_fd if open */
    if (proc->fd_table[new_fd].used) {
        memset(&proc->fd_table[new_fd], 0, sizeof(struct process_fd));
    }

    proc->fd_table[new_fd] = proc->fd_table[old_fd];
    proc->fd_table[new_fd].offset = proc->fd_table[old_fd].offset;
    return new_fd;
}

int sys_dup3_impl(int old_fd, int new_fd, int flags)
{
    /* Validate flags — only O_CLOEXEC is valid for dup3 */
    if (flags & ~O_CLOEXEC)
        return -1;

    (void)sys_get_fd; /* suppress unused warning */

    struct process *proc = process_get_current();
    if (!proc) return -1;
    if (old_fd >= PROCESS_FD_MAX || !proc->fd_table[old_fd].used)
        return -1;
    if (new_fd >= PROCESS_FD_MAX)
        return -1;

    /* If oldfd == newfd and flags has O_CLOEXEC, just update the flag */
    if (old_fd == new_fd) {
        if (flags & O_CLOEXEC)
            proc->fd_table[new_fd].flags |= FD_CLOEXEC;
        else
            proc->fd_table[new_fd].flags &= ~FD_CLOEXEC;
        return new_fd;
    }

    /* Close new_fd if open */
    if (proc->fd_table[new_fd].used) {
        memset(&proc->fd_table[new_fd], 0, sizeof(struct process_fd));
    }

    /* Duplicate fd entry */
    proc->fd_table[new_fd] = proc->fd_table[old_fd];

    /* Set CLOEXEC flag if requested */
    if (flags & O_CLOEXEC)
        proc->fd_table[new_fd].flags |= FD_CLOEXEC;
    else
        proc->fd_table[new_fd].flags &= ~FD_CLOEXEC;

    return new_fd;
}
