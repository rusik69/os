#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "freeze.h"
#include "bufcache.h"
#include "page_cache.h"
#include "blockdev.h"
#include "vfs.h"

static int fs_frozen = 0;
static int freeze_count = 0;

void freeze_init(void) {
    fs_frozen = 0;
    freeze_count = 0;
    kprintf("[OK] Filesystem freeze subsystem initialized\n");
}

/* Freeze the filesystem: flush pending I/O and block new writes.
 * Returns 0 on success, -1 if already frozen. */
int freeze_fs(void) {
    if (fs_frozen) {
        freeze_count++;
        return 0; /* nested freeze allowed */
    }

    kprintf("[freeze] Freezing filesystem...\n");

    /* Step 1: Flush buffer cache dirty entries */
    bufcache_flush();
    kprintf("[freeze] Buffer cache flushed\n");

    /* Step 2: Flush page cache dirty pages */
    page_cache_flush();
    kprintf("[freeze] Page cache flushed\n");

    /* Step 3: Set frozen flag to block new writes */
    fs_frozen = 1;
    freeze_count = 1;

    /* Step 4: Mark all mounted filesystems - freeze will block writes
     * via the is_frozen() check that filesystem ops must call. */
    for (int i = 0; i < num_mounts && i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].flags & 1) { /* just log active mounts */
            kprintf("[freeze]  Frozen mount: %s\n", mounts[i].mountpoint);
        }
    }

    kprintf("[freeze] Filesystem frozen\n");
    return 0;
}

/* Thaw the filesystem: allow writes to resume.
 * Returns 0 on success, -1 if not frozen. */
int thaw_fs(void) {
    if (!fs_frozen) return -1;

    freeze_count--;
    if (freeze_count > 0)
        return 0; /* still frozen by nested freeze */

    kprintf("[freeze] Thawing filesystem...\n");

    /* Unfrozen flag */
    fs_frozen = 0;

    kprintf("[freeze] Filesystem thawed\n");
    return 0;
}

int is_frozen(void) { return fs_frozen; }
#include "module.h"
module_init(freeze_init);

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Stub: freeze_super ────────────────────────────── */
int freeze_super(struct super_block *sb)
{
    (void)sb;
    kprintf("[freeze] freeze_super: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: thaw_super ──────────────────────────────── */
int thaw_super(struct super_block *sb)
{
    (void)sb;
    kprintf("[freeze] thaw_super: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: freeze_bdev ─────────────────────────────── */
int freeze_bdev(struct block_device *bdev)
{
    (void)bdev;
    kprintf("[freeze] freeze_bdev: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: thaw_bdev ───────────────────────────────── */
int thaw_bdev(struct block_device *bdev)
{
    (void)bdev;
    kprintf("[freeze] thaw_bdev: not yet implemented\n");
    return -ENOSYS;
}
