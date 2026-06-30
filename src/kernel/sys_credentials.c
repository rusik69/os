/*
 * Linux-compatible credential syscalls.
 *
 * Provides setuid, seteuid, setgid, setegid, and related credential
 * management syscalls matching the Linux x86-64 ABI conventions.
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
