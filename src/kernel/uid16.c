/* uid16.c — 16-bit UID syscall compatibility layer for old binaries
 *
 * Provides backwards-compatible syscalls that use 16-bit UID/GID values.
 * These translate between the old 16-bit UID syscalls and the internal
 * 32-bit UID storage.
 *
 * Implemented syscalls:
 *   sys_setuid16     — set 16-bit UID
 *   sys_setgid16     — set 16-bit GID
 *   sys_getuid16     — get 16-bit real UID
 *   sys_getgid16     — get 16-bit real GID
 *   sys_geteuid16    — get 16-bit effective UID
 *   sys_getegid16    — get 16-bit effective GID
 *   sys_setreuid16   — set real/effective 16-bit UID
 *   sys_setregid16   — set real/effective 16-bit GID
 *   sys_getresuid16  — get real/effective/saved 16-bit UID
 *   sys_getresgid16  — get real/effective/saved 16-bit GID
 *   sys_setresuid16  — set real/effective/saved 16-bit UID
 *   sys_setresgid16  — set real/effective/saved 16-bit GID
 *   sys_setfsuid16   — set filesystem 16-bit UID
 *   sys_setfsgid16   — set filesystem 16-bit GID
 */

#include "types.h"
#include "process.h"
#include "errno.h"
#include "printf.h"
#include "scheduler.h"
#include "uaccess.h"

/* ── Helpers ───────────────────────────────────────────────────────── */

static inline uint16_t uid_to_16(uint32_t uid)
{
    return (uid > 0xFFFF) ? 0xFFFE : (uint16_t)uid;
}

static inline uint32_t uid_from_16(uint16_t uid16)
{
    return (uint32_t)uid16;
}

/* ── Syscall implementations ───────────────────────────────────────── */

int sys_getuid16(void)
{
    struct process *p = process_get_current();
    if (!p) return 0;
    return uid_to_16(p->uid);
}

int sys_getgid16(void)
{
    struct process *p = process_get_current();
    if (!p) return 0;
    return uid_to_16(p->gid);
}

int sys_geteuid16(void)
{
    struct process *p = process_get_current();
    if (!p) return 0;
    return uid_to_16(p->euid);
}

int sys_getegid16(void)
{
    struct process *p = process_get_current();
    if (!p) return 0;
    return uid_to_16(p->egid);
}

int sys_setuid16(uint16_t uid16)
{
    struct process *p = process_get_current();
    if (!p) return -EINVAL;

    uint32_t new_uid = uid_from_16(uid16);

    /* Only privileged processes can change UID */
    if (p->euid != 0 && new_uid != p->uid && new_uid != p->euid)
        return -EPERM;

    p->uid  = new_uid;
    p->euid = new_uid;
    return 0;
}

int sys_setgid16(uint16_t gid16)
{
    struct process *p = process_get_current();
    if (!p) return -EINVAL;

    uint32_t new_gid = uid_from_16(gid16);

    if (p->egid != 0 && new_gid != p->gid && new_gid != p->egid)
        return -EPERM;

    p->gid  = new_gid;
    p->egid = new_gid;
    return 0;
}

int sys_setreuid16(uint16_t ruid16, uint16_t euid16)
{
    struct process *p = process_get_current();
    if (!p) return -EINVAL;

    uint32_t new_ruid = uid_from_16(ruid16);
    uint32_t new_euid = uid_from_16(euid16);
    int is_priv = (p->euid == 0);

    if (!is_priv) {
        if ((uint16_t)new_ruid != (uint16_t)-1 && new_ruid != p->uid && new_ruid != p->euid)
            return -EPERM;
        if ((uint16_t)new_euid != (uint16_t)-1 && new_euid != p->uid && new_euid != p->euid)
            return -EPERM;
    }

    if ((uint16_t)ruid16 != (uint16_t)-1) {
        p->uid = new_ruid;
    }
    if ((uint16_t)euid16 != (uint16_t)-1) {
        p->euid = new_euid;
    }

    return 0;
}

int sys_setregid16(uint16_t rgid16, uint16_t egid16)
{
    struct process *p = process_get_current();
    if (!p) return -EINVAL;

    uint32_t new_rgid = uid_from_16(rgid16);
    uint32_t new_egid = uid_from_16(egid16);
    int is_priv = (p->egid == 0);

    if (!is_priv) {
        if ((uint16_t)new_rgid != (uint16_t)-1 && new_rgid != p->gid && new_rgid != p->egid)
            return -EPERM;
        if ((uint16_t)new_egid != (uint16_t)-1 && new_egid != p->gid && new_egid != p->egid)
            return -EPERM;
    }

    if ((uint16_t)rgid16 != (uint16_t)-1) {
        p->gid = new_rgid;
    }
    if ((uint16_t)egid16 != (uint16_t)-1) {
        p->egid = new_egid;
    }

    return 0;
}

int __no_sanitize_address sys_getresuid16(uint16_t *ruid, uint16_t *euid, uint16_t *suid)
{
    struct process *p = process_get_current();
    if (!p) return -EINVAL;

    uint16_t r = uid_to_16(p->uid);
    uint16_t e = uid_to_16(p->euid);
    uint16_t s = uid_to_16(p->euid);  /* saved uid not stored separately */

    if (copy_to_user((uint64_t)ruid, &r, 2) < 0) return -EFAULT;
    if (copy_to_user((uint64_t)euid, &e, 2) < 0) return -EFAULT;
    if (copy_to_user((uint64_t)suid, &s, 2) < 0) return -EFAULT;

    return 0;
}

int __no_sanitize_address sys_getresgid16(uint16_t *rgid, uint16_t *egid, uint16_t *sgid)
{
    struct process *p = process_get_current();
    if (!p) return -EINVAL;

    uint16_t r = uid_to_16(p->gid);
    uint16_t e = uid_to_16(p->egid);
    uint16_t s = uid_to_16(p->egid);  /* saved gid not stored separately */

    if (copy_to_user((uint64_t)rgid, &r, 2) < 0) return -EFAULT;
    if (copy_to_user((uint64_t)egid, &e, 2) < 0) return -EFAULT;
    if (copy_to_user((uint64_t)sgid, &s, 2) < 0) return -EFAULT;

    return 0;
}

int sys_setresuid16(uint16_t ruid16, uint16_t euid16, uint16_t suid16)
{
    struct process *p = process_get_current();
    if (!p) return -EINVAL;

    int is_priv = (p->euid == 0);

    if (!is_priv) {
        if ((uint16_t)ruid16 != (uint16_t)-1 && uid_from_16(ruid16) != p->uid &&
            uid_from_16(ruid16) != p->euid)
            return -EPERM;
        if ((uint16_t)euid16 != (uint16_t)-1 && uid_from_16(euid16) != p->uid &&
            uid_from_16(euid16) != p->euid)
            return -EPERM;
        if ((uint16_t)suid16 != (uint16_t)-1 && uid_from_16(suid16) != p->uid &&
            uid_from_16(suid16) != p->euid)
            return -EPERM;
    }

    if ((uint16_t)ruid16 != (uint16_t)-1) p->uid = uid_from_16(ruid16);
    if ((uint16_t)euid16 != (uint16_t)-1) p->euid = uid_from_16(euid16);
    if ((uint16_t)suid16 != (uint16_t)-1) p->euid = uid_from_16(suid16);

    return 0;
}

int sys_setresgid16(uint16_t rgid16, uint16_t egid16, uint16_t sgid16)
{
    struct process *p = process_get_current();
    if (!p) return -EINVAL;

    int is_priv = (p->egid == 0);

    if (!is_priv) {
        if ((uint16_t)rgid16 != (uint16_t)-1 && uid_from_16(rgid16) != p->gid &&
            uid_from_16(rgid16) != p->egid)
            return -EPERM;
        if ((uint16_t)egid16 != (uint16_t)-1 && uid_from_16(egid16) != p->gid &&
            uid_from_16(egid16) != p->egid)
            return -EPERM;
        if ((uint16_t)sgid16 != (uint16_t)-1 && uid_from_16(sgid16) != p->gid &&
            uid_from_16(sgid16) != p->egid)
            return -EPERM;
    }

    if ((uint16_t)rgid16 != (uint16_t)-1) p->gid = uid_from_16(rgid16);
    if ((uint16_t)egid16 != (uint16_t)-1) p->egid = uid_from_16(egid16);
    if ((uint16_t)sgid16 != (uint16_t)-1) p->egid = uid_from_16(sgid16);

    return 0;
}

int sys_setfsuid16(uint16_t uid16)
{
    struct process *p = process_get_current();
    if (!p) return -EINVAL;

    uint32_t new_fsuid = uid_from_16(uid16);
    int old_fsuid = uid_to_16(p->euid);  /* no separate fsuid */

    if (p->euid == 0 ||
        new_fsuid == p->uid ||
        new_fsuid == p->euid) {
        p->euid = new_fsuid;
    }

    return old_fsuid;
}

int sys_setfsgid16(uint16_t gid16)
{
    struct process *p = process_get_current();
    if (!p) return -EINVAL;

    uint32_t new_fsgid = uid_from_16(gid16);
    int old_fsgid = uid_to_16(p->egid);  /* no separate fsgid */

    if (p->egid == 0 ||
        new_fsgid == p->gid ||
        new_fsgid == p->egid) {
        p->egid = new_fsgid;
    }

    return old_fsgid;
}

/* ── Initialization ────────────────────────────────────────────────── */

void __init uid16_init(void)
{
    kprintf("[OK] UID16: 16-bit UID syscall compatibility layer\n");
}

/* ── Stub: uid16_to_uid ─────────────────────────────── */
int uid16_to_uid(uint16_t uid16)
{
    (void)uid16;
    kprintf("[uid] uid16_to_uid: not yet implemented\n");
    return 0;
}
/* ── Stub: uid_to_uid16 ─────────────────────────────── */
uint16_t uid_to_uid16(int uid)
{
    (void)uid;
    kprintf("[uid] uid_to_uid16: not yet implemented\n");
    return 0;
}
/* ── Stub: uid16_validate ─────────────────────────────── */
int uid16_validate(uint16_t uid16)
{
    (void)uid16;
    kprintf("[uid] uid16_validate: not yet implemented\n");
    return 0;
}
