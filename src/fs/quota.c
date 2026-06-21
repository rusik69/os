/*
 * quota.c — Per-UID filesystem quota enforcement
 *
 * Tracks block and inode usage per UID and enforces soft/hard limits.
 * When a write or create would exceed the hard limit, the operation
 * fails with EDQUOT.  Soft limits trigger a warning via kprintf.
 *
 * Integration:
 *   vfs_check_quota_blocks() is called from vfs_write() before write.
 *   vfs_check_quota_inodes() is called from vfs_create() before create.
 *   vfs_update_quota_blocks() is called after successful write/truncate.
 *   vfs_update_quota_inodes() is called after successful create/unlink.
 */

#define KERNEL_INTERNAL
#include "quota.h"
#include "string.h"
#include "printf.h"
#include "process.h"

/* timer_get_ticks from the kernel timer subsystem */
extern uint64_t timer_get_ticks(void);

/* ── Configuration ───────────────────────────────────────────────────── */

#define QUOTA_MAX_USERS 32      /* maximum tracked UIDs */

/* ── Per-UID quota structure ─────────────────────────────────────────── */

struct fs_quota_entry {
    int      in_use;
    uint16_t uid;

    /* Limits (0 = unlimited) */
    uint32_t block_soft_limit;   /* soft limit in QUOTA_BLOCK_SIZE blocks */
    uint32_t block_hard_limit;   /* hard limit in QUOTA_BLOCK_SIZE blocks */
    uint32_t inode_soft_limit;   /* soft limit in inodes */
    uint32_t inode_hard_limit;   /* hard limit in inodes */

    /* Current usage */
    uint32_t cur_blocks;         /* current block count */
    uint32_t cur_inodes;         /* current inode count */

    /* Grace period tracking */
    uint64_t block_grace_start;  /* tick when soft limit was first exceeded (0 = no grace) */
    uint64_t inode_grace_start;  /* tick when inode soft limit was first exceeded */
};

/* ── Static state ────────────────────────────────────────────────────── */

static struct fs_quota_entry quota_entries[QUOTA_MAX_USERS];
static int quota_initialized = 0;

/* Default grace period in timer ticks (assuming ~100 Hz tick rate → 7 days) */
#define QUOTA_DEFAULT_GRACE_TICKS  (7UL * 24 * 3600 * 100)

/* ── Internal helpers ────────────────────────────────────────────────── */

static struct fs_quota_entry *quota_find(uint16_t uid)
{
    for (int i = 0; i < QUOTA_MAX_USERS; i++) {
        if (quota_entries[i].in_use && quota_entries[i].uid == uid)
            return &quota_entries[i];
    }
    return NULL;
}

static struct fs_quota_entry *quota_alloc(uint16_t uid)
{
    struct fs_quota_entry *existing = quota_find(uid);
    if (existing)
        return existing;

    for (int i = 0; i < QUOTA_MAX_USERS; i++) {
        if (!quota_entries[i].in_use) {
            memset(&quota_entries[i], 0, sizeof(quota_entries[i]));
            quota_entries[i].in_use = 1;
            quota_entries[i].uid   = uid;
            return &quota_entries[i];
        }
    }
    return NULL;
}

/* Check whether a soft limit grace period has expired */
static int grace_expired(uint64_t grace_start)
{
    if (grace_start == 0)
        return 0;  /* no active grace period */
    /* Use timer ticks if available, else assume expiry */
    uint64_t now;
    now = timer_get_ticks();
    if (now < grace_start)
        return 0;  /* tick wraparound — treat as not expired */
    return (now - grace_start) >= QUOTA_DEFAULT_GRACE_TICKS;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void vfs_quota_init(void)
{
    if (quota_initialized)
        return;
    memset(quota_entries, 0, sizeof(quota_entries));
    quota_initialized = 1;
    kprintf("[OK] vfs_quota: initialized (%d entries, block size %d)\n",
            QUOTA_MAX_USERS, QUOTA_BLOCK_SIZE);
}

int vfs_set_quota(uint16_t uid, uint32_t block_limit, uint32_t inode_limit)
{
    if (!quota_initialized)
        vfs_quota_init();

    struct fs_quota_entry *eq = quota_find(uid);
    if (!eq) {
        /* Auto-create entry for new UID */
        eq = quota_alloc(uid);
        if (!eq)
            return -1;
    }

    eq->block_hard_limit = block_limit;
    eq->inode_hard_limit = inode_limit;

    /* Soft limits default to 80% of hard limits */
    if (block_limit > 0)
        eq->block_soft_limit = (uint32_t)((uint64_t)block_limit * 80 / 100);
    else
        eq->block_soft_limit = 0;

    if (inode_limit > 0)
        eq->inode_soft_limit = (uint32_t)((uint64_t)inode_limit * 80 / 100);
    else
        eq->inode_soft_limit = 0;

    return 0;
}

int vfs_get_quota(uint16_t uid, struct fs_quota *quota)
{
    if (!quota || !quota_initialized)
        return -1;

    struct fs_quota_entry *eq = quota_find(uid);
    if (!eq)
        return -1;

    quota->uid              = eq->uid;
    quota->block_soft_limit = eq->block_soft_limit;
    quota->block_hard_limit = eq->block_hard_limit;
    quota->inode_soft_limit = eq->inode_soft_limit;
    quota->inode_hard_limit = eq->inode_hard_limit;
    quota->cur_blocks       = eq->cur_blocks;
    quota->cur_inodes       = eq->cur_inodes;
    quota->block_grace      = grace_expired(eq->block_grace_start) ? 0 : 1;
    quota->inode_grace      = grace_expired(eq->inode_grace_start) ? 0 : 1;

    return 0;
}

/*
 * Check whether @uid has enough quota to allocate @blocks_needed blocks.
 * Returns 0 if OK, -EDQUOT if the hard limit would be exceeded,
 * or -1 if the soft limit grace period has expired.
 */
int vfs_check_quota_blocks(uint16_t uid, uint32_t blocks_needed)
{
    if (!quota_initialized)
        return 0;  /* no quota enforcement */

    struct fs_quota_entry *eq = quota_find(uid);
    if (!eq)
        return 0;  /* no quota configured for this UID */

    uint32_t new_total = eq->cur_blocks + blocks_needed;

    /* Check for wraparound */
    if (new_total < eq->cur_blocks)
        return -EDQUOT;  /* overflow — deny */

    /* Hard limit check */
    if (eq->block_hard_limit > 0 && new_total > eq->block_hard_limit)
        return -EDQUOT;

    /* Soft limit check — warn but allow if within grace period */
    if (eq->block_soft_limit > 0 && new_total > eq->block_soft_limit) {
        if (eq->block_grace_start == 0) {
            /* First time exceeding the soft limit — start grace period */
            eq->block_grace_start = timer_get_ticks();
            kprintf("[QUOTA] UID %u: block soft limit (%u blocks) exceeded, "
                    "grace period started\n",
                    (unsigned int)uid, (unsigned int)eq->block_soft_limit);
        } else if (grace_expired(eq->block_grace_start)) {
            /* Grace period expired — enforce as hard limit */
            kprintf("[QUOTA] UID %u: block soft limit grace period expired, "
                    "denying allocation\n", (unsigned int)uid);
            return -EDQUOT;
        } else {
            /* Within grace period — just warn */
            kprintf("[QUOTA] UID %u: block soft limit exceeded (%u > %u), "
                    "grace period active\n",
                    (unsigned int)uid,
                    (unsigned int)new_total, (unsigned int)eq->block_soft_limit);
        }
    }

    return 0;
}

/*
 * Check whether @uid can create a new inode.
 * Returns 0 if OK, -EDQUOT if the hard limit would be exceeded.
 */
int vfs_check_quota_inodes(uint16_t uid)
{
    if (!quota_initialized)
        return 0;

    struct fs_quota_entry *eq = quota_find(uid);
    if (!eq)
        return 0;

    uint32_t new_total = eq->cur_inodes + 1;

    /* Hard limit check */
    if (eq->inode_hard_limit > 0 && new_total > eq->inode_hard_limit)
        return -EDQUOT;

    /* Soft limit check */
    if (eq->inode_soft_limit > 0 && new_total > eq->inode_soft_limit) {
        if (eq->inode_grace_start == 0) {
            eq->inode_grace_start = timer_get_ticks();
            kprintf("[QUOTA] UID %u: inode soft limit (%u inodes) exceeded, "
                    "grace period started\n",
                    (unsigned int)uid, (unsigned int)eq->inode_soft_limit);
        } else if (grace_expired(eq->inode_grace_start)) {
            kprintf("[QUOTA] UID %u: inode soft limit grace period expired, "
                    "denying creation\n", (unsigned int)uid);
            return -EDQUOT;
        }
    }

    return 0;
}

/*
 * Update block usage after a successful write or truncate.
 * @delta_blocks: positive to add, negative to subtract.
 */
void vfs_update_quota_blocks(uint16_t uid, int32_t delta_blocks)
{
    struct fs_quota_entry *eq = quota_find(uid);
    if (!eq)
        return;

    int32_t new_val = (int32_t)eq->cur_blocks + delta_blocks;
    if (new_val < 0)
        new_val = 0;

    eq->cur_blocks = (uint32_t)new_val;

    /* If usage drops below the soft limit, reset the grace period */
    if (eq->block_soft_limit > 0 && eq->cur_blocks <= eq->block_soft_limit)
        eq->block_grace_start = 0;
}

/*
 * Update inode usage after a successful create or unlink.
 * @delta: +1 for create, -1 for unlink.
 */
void vfs_update_quota_inodes(uint16_t uid, int32_t delta)
{
    struct fs_quota_entry *eq = quota_find(uid);
    if (!eq)
        return;

    int32_t new_val = (int32_t)eq->cur_inodes + delta;
    if (new_val < 0)
        new_val = 0;

    eq->cur_inodes = (uint32_t)new_val;

    if (eq->inode_soft_limit > 0 && eq->cur_inodes <= eq->inode_soft_limit)
        eq->inode_grace_start = 0;
}

/* ── Debug: dump all quota entries ──────────────────────────────────── */

void vfs_quota_dump(void)
{
    if (!quota_initialized) {
        kprintf("Quota: not initialized\n");
        return;
    }

    int count = 0;
    for (int i = 0; i < QUOTA_MAX_USERS; i++) {
        if (quota_entries[i].in_use) {
            struct fs_quota_entry *eq = &quota_entries[i];
            kprintf("  UID %u: blocks %u/%u/%u, inodes %u/%u/%u%s\n",
                    (unsigned int)eq->uid,
                    (unsigned int)eq->cur_blocks,
                    (unsigned int)eq->block_soft_limit,
                    (unsigned int)eq->block_hard_limit,
                    (unsigned int)eq->cur_inodes,
                    (unsigned int)eq->inode_soft_limit,
                    (unsigned int)eq->inode_hard_limit,
                    eq->block_grace_start ? " [B-GRACE]" : "");
            count++;
        }
    }
    if (count == 0)
        kprintf("  (no quota entries)\n");
}
#include "module.h"
module_init(vfs_quota_init);

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Stub: quota_free ──────────────────────────────── */
int quota_free(uint16_t uid)
{
    (void)uid;
    kprintf("[QUOTA] quota_free: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: quota_check ─────────────────────────────── */
int quota_check(uint16_t uid, uint32_t blocks_needed)
{
    (void)uid;
    (void)blocks_needed;
    kprintf("[QUOTA] quota_check: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: quota_sync ──────────────────────────────── */
int quota_sync(void)
{
    kprintf("[QUOTA] quota_sync: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: quota_get ────────────────────────────────── */
int quota_get(uint16_t uid, struct fs_quota *quota)
{
    (void)uid;
    (void)quota;
    kprintf("[QUOTA] quota_get: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: quota_set ────────────────────────────────── */
int quota_set(uint16_t uid, uint32_t block_limit, uint32_t inode_limit)
{
    (void)uid;
    (void)block_limit;
    (void)inode_limit;
    kprintf("[QUOTA] quota_set: not yet implemented\n");
    return -ENOSYS;
}
