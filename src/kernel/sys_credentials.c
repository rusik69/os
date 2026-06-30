/*
 * Linux-compatible credential syscalls.
 *
 * Provides setuid, seteuid, setgid, setegid, getgroups, setgroups,
 * and related credential management syscalls matching the Linux
 * x86-64 ABI conventions.
 *
 * Each function returns (uint64_t)(int64_t)-errno on error, or a
 * non-negative value on success.
 */
#include "syscall.h"
#include "module.h"
#include "process.h"
#include "errno.h"
#include "uaccess.h"
#include "scheduler.h"

/* Module metadata */
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Linux-compatible credential syscalls (setuid, seteuid, etc.)");
MODULE_AUTHOR("Ruslan Gustomiasov");

/* ── sys_setuid — set user identity ───────────────────────────────
 *
 * int setuid(uid_t uid);
 *
 * Sets the effective user ID. If called by root (euid == 0), also
 * sets the real and saved user IDs. Unprivileged callers may only
 * set the effective ID to the real, effective, or saved user ID.
 *
 * Returns 0 on success, -EPERM if not allowed.
 */
uint64_t sys_setuid(uint64_t uid)
{
    struct process *p = process_get_current();
    if (!p)
        return (uint64_t)(int64_t)-ESRCH;

    /* Root (euid 0) can set all UID values */
    if (p->euid == 0) {
        p->uid  = (uint32_t)uid;
        p->euid = (uint32_t)uid;
        /* Clear dumpable on credential change */
        p->dumpable = 0;
        return 0;
    }

    /* Non-root: only allowed if uid matches real or saved (effective) UID */
    if ((uint32_t)uid != p->uid && (uint32_t)uid != p->euid)
        return (uint64_t)(int64_t)-EPERM;

    p->euid = (uint32_t)uid;
    p->dumpable = 0;
    return 0;
}

/* ── sys_seteuid — set effective user identity ────────────────────
 *
 * int seteuid(uid_t euid);
 *
 * Sets the effective user ID. Unprivileged callers may only set
 * the effective ID to the real user ID. Privileged callers
 * (euid == 0) may set to any value.
 *
 * Returns 0 on success, -EPERM if not allowed.
 */
uint64_t sys_seteuid(uint64_t euid)
{
    struct process *p = process_get_current();
    if (!p)
        return (uint64_t)(int64_t)-ESRCH;

    /* Root (euid 0) can set effective UID to any value */
    if (p->euid == 0) {
        p->euid = (uint32_t)euid;
        p->dumpable = 0;
        return 0;
    }

    /* Non-root: only allowed if euid matches real UID */
    if ((uint32_t)euid != p->uid)
        return (uint64_t)(int64_t)-EPERM;

    p->euid = (uint32_t)euid;
    p->dumpable = 0;
    return 0;
}

/* ── sys_setgid — set group identity ──────────────────────────────
 *
 * int setgid(gid_t gid);
 *
 * Sets the effective group ID. If called by root (euid == 0), also
 * sets the real and saved group IDs. Unprivileged callers may only
 * set the effective ID to the real, effective, or saved group ID.
 *
 * Returns 0 on success, -EPERM if not allowed.
 */
uint64_t sys_setgid(uint64_t gid)
{
    struct process *p = process_get_current();
    if (!p)
        return (uint64_t)(int64_t)-ESRCH;

    /* Root (euid 0) can set all GID values */
    if (p->euid == 0) {
        p->gid  = (uint32_t)gid;
        p->egid = (uint32_t)gid;
        /* Clear dumpable on credential change */
        p->dumpable = 0;
        return 0;
    }

    /* Non-root: only allowed if gid matches real or saved (effective) GID */
    if ((uint32_t)gid != p->gid && (uint32_t)gid != p->egid)
        return (uint64_t)(int64_t)-EPERM;

    p->egid = (uint32_t)gid;
    p->dumpable = 0;
    return 0;
}

/* ── sys_setegid — set effective group identity ───────────────────
 *
 * int setegid(gid_t egid);
 *
 * Sets the effective group ID. Unprivileged callers may only set
 * the effective ID to the real group ID. Privileged callers
 * (euid == 0) may set to any value.
 *
 * Returns 0 on success, -EPERM if not allowed.
 */
uint64_t sys_setegid(uint64_t egid)
{
    struct process *p = process_get_current();
    if (!p)
        return (uint64_t)(int64_t)-ESRCH;

    /* Root (euid 0) can set effective GID to any value */
    if (p->euid == 0) {
        p->egid = (uint32_t)egid;
        p->dumpable = 0;
        return 0;
    }

    /* Non-root: only allowed if egid matches real GID */
    if ((uint32_t)egid != p->gid)
        return (uint64_t)(int64_t)-EPERM;

    p->egid = (uint32_t)egid;
    p->dumpable = 0;
    return 0;
}

/* ── sys_getpgrp — get process group ID of calling process ───────
 *
 * pid_t getpgrp(void);
 *
 * Returns the process group ID of the calling process.
 * Equivalent to getpgid(0).
 */
uint64_t sys_getpgrp(void)
{
    struct process *p = process_get_current();
    if (!p)
        return (uint64_t)(int64_t)-ESRCH;
    return (uint64_t)p->pgid;
}

/* ── sys_getgroups — get list of supplementary group IDs ──────────
 *
 * int getgroups(int size, gid_t list[]);
 *
 * If size is 0, return the number of supplementary group IDs.
 * If size is non-zero, copy up to size group IDs into list.
 * Returns the number of groups copied, or -EINVAL if size is
 * non-zero but smaller than the actual number of groups.
 */
uint64_t sys_getgroups(uint64_t size, uint64_t list_addr)
{
    struct process *p = process_get_current();
    if (!p)
        return (uint64_t)(int64_t)-ESRCH;

    int ngroups = p->ngroups;

    /* If size is 0, just return the count */
    if (size == 0)
        return (uint64_t)(int64_t)ngroups;

    /* Size must be at least the number of groups */
    if ((int)size < ngroups)
        return (uint64_t)(int64_t)-EINVAL;

    /* Copy groups to user buffer */
    if (ngroups > 0) {
        if (copy_to_user(list_addr, p->groups,
                         (size_t)ngroups * sizeof(uint32_t)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    }

    return (uint64_t)(int64_t)ngroups;
}

/* ── sys_setgroups — set supplementary group IDs ──────────────────
 *
 * int setgroups(size_t size, const gid_t *list);
 *
 * Sets the supplementary group IDs for the current process.
 * Only privileged (euid == 0) processes may call this.
 * Returns 0 on success, -EPERM if not privileged, -EFAULT if
 * the list pointer is invalid, -EINVAL if size is too large.
 */
uint64_t sys_setgroups(uint64_t size, uint64_t list_addr)
{
    struct process *p = process_get_current();
    if (!p)
        return (uint64_t)(int64_t)-ESRCH;

    /* Only root (euid == 0) may change supplementary groups */
    if (p->euid != 0)
        return (uint64_t)(int64_t)-EPERM;

    /* Validate size */
    if ((int)size > NGROUPS_MAX)
        return (uint64_t)(int64_t)-EINVAL;

    /* Clear existing groups */
    p->ngroups = 0;

    /* Copy in new groups from user buffer */
    if (size > 0) {
        uint32_t buf[NGROUPS_MAX];
        if (copy_from_user(buf, list_addr,
                           (size_t)size * sizeof(uint32_t)) < 0)
            return (uint64_t)(int64_t)-EFAULT;

        for (int i = 0; i < (int)size; i++)
            p->groups[i] = buf[i];
        p->ngroups = (uint16_t)size;
    }

    return 0;
}
