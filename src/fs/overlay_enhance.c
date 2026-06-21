/* overlay_enhance.c — Whiteout and opaque directory support for overlay FS
 *
 * Implements:
 *   C17: Whiteout entries — hide files from lower layers
 *   C18: Opaque directories — prevent readdir from merging lower layers
 *
 * In overlay filesystems, when a file is deleted from the merged view,
 * a whiteout entry is placed in the upper layer. Similarly, an opaque
 * xattr on an upper directory tells the overlay to stop merging lower
 * directory contents at that point.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "errno.h"
#include "vfs.h"
#include "overlay.h"

/* ── Constants ──────────────────────────────────────────────────────── */

#define OVL_WHITEOUT_PREFIX ".wh."
#define OVL_OPAQUE_XATTR    "trusted.overlay.opaque"
#define OVL_WHITEOUT_XATTR  "trusted.overlay.whiteout"
#define OVL_WHITEOUT_MODE   0
#define MAX_WHITEOUTS       1024

/* ── Whiteout table (per-overlay-mount) ─────────────────────────────── */

struct whiteout_entry {
    int  in_use;
    char mountpoint[64];
    char name[256];
};

static struct whiteout_entry whiteout_table[MAX_WHITEOUTS];
static int overlay_enhance_initialised = 0;

/* ── Initialisation ─────────────────────────────────────────────────── */

void overlay_enhance_init(void)
{
    if (overlay_enhance_initialised)
        return;

    memset(whiteout_table, 0, sizeof(whiteout_table));
    overlay_enhance_initialised = 1;
    kprintf("[OverlayEnh] Whiteout + opaque directory support initialised\n");
}

/* ── Whiteout API (C17) ─────────────────────────────────────────────── */

/* Create a whiteout entry for 'name' in the overlay mounted at 'mountpoint'.
 * This effectively hides the file from all lower layers. */
int overlay_whiteout_create(const char *mountpoint, const char *name)
{
    if (!mountpoint || !name)
        return -EINVAL;
    if (!overlay_enhance_initialised)
        return -EPERM;

    /* Find a free whiteout slot */
    int idx = -1;
    for (int i = 0; i < MAX_WHITEOUTS; i++) {
        if (!whiteout_table[i].in_use) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return -ENOSPC;

    strncpy(whiteout_table[idx].mountpoint, mountpoint,
            sizeof(whiteout_table[idx].mountpoint) - 1);
    strncpy(whiteout_table[idx].name, name,
            sizeof(whiteout_table[idx].name) - 1);
    whiteout_table[idx].in_use = 1;

    kprintf("[OverlayEnh] Whiteout created: %s/%s\n", mountpoint, name);
    return 0;
}

/* Check whether 'name' is whiteouted in the overlay at 'mountpoint'.
 * Returns 1 if whiteouted, 0 otherwise. */
int overlay_whiteout_check(const char *mountpoint, const char *name)
{
    if (!mountpoint || !name || !overlay_enhance_initialised)
        return 0;

    for (int i = 0; i < MAX_WHITEOUTS; i++) {
        if (whiteout_table[i].in_use &&
            strcmp(whiteout_table[i].mountpoint, mountpoint) == 0 &&
            strcmp(whiteout_table[i].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Remove a whiteout entry (un-hide a file). */
int overlay_whiteout_remove(const char *mountpoint, const char *name)
{
    if (!mountpoint || !name || !overlay_enhance_initialised)
        return -EINVAL;

    for (int i = 0; i < MAX_WHITEOUTS; i++) {
        if (whiteout_table[i].in_use &&
            strcmp(whiteout_table[i].mountpoint, mountpoint) == 0 &&
            strcmp(whiteout_table[i].name, name) == 0) {
            memset(&whiteout_table[i], 0, sizeof(struct whiteout_entry));
            return 0;
        }
    }
    return -ENOENT;
}

/* ── Opaque directory API (C18) ─────────────────────────────────────── */

/* Mark a directory as opaque in the overlay.
 * When a directory is opaque, readdir will NOT merge entries from
 * lower layers — only entries from the topmost (upper) layer are shown. */

/* Each overlay mount can have a set of opaque directories. */
#define MAX_OPAQUE_DIRS 128

struct opaque_dir {
    int  in_use;
    char mountpoint[64];
    char dir_path[256];
};

static struct opaque_dir opaque_table[MAX_OPAQUE_DIRS];

int overlay_opaque_set(const char *mountpoint, const char *dir_path)
{
    if (!mountpoint || !dir_path)
        return -EINVAL;
    if (!overlay_enhance_initialised)
        return -EPERM;

    int idx = -1;
    for (int i = 0; i < MAX_OPAQUE_DIRS; i++) {
        if (!opaque_table[i].in_use) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return -ENOSPC;

    strncpy(opaque_table[idx].mountpoint, mountpoint,
            sizeof(opaque_table[idx].mountpoint) - 1);
    strncpy(opaque_table[idx].dir_path, dir_path,
            sizeof(opaque_table[idx].dir_path) - 1);
    opaque_table[idx].in_use = 1;

    kprintf("[OverlayEnh] Opaque directory set: %s%s\n", mountpoint, dir_path);
    return 0;
}

int overlay_opaque_check(const char *mountpoint, const char *dir_path)
{
    if (!mountpoint || !dir_path || !overlay_enhance_initialised)
        return 0;

    for (int i = 0; i < MAX_OPAQUE_DIRS; i++) {
        if (opaque_table[i].in_use &&
            strcmp(opaque_table[i].mountpoint, mountpoint) == 0 &&
            strcmp(opaque_table[i].dir_path, dir_path) == 0) {
            return 1;
        }
    }
    return 0;
}

int overlay_opaque_clear(const char *mountpoint, const char *dir_path)
{
    if (!mountpoint || !dir_path || !overlay_enhance_initialised)
        return -EINVAL;

    for (int i = 0; i < MAX_OPAQUE_DIRS; i++) {
        if (opaque_table[i].in_use &&
            strcmp(opaque_table[i].mountpoint, mountpoint) == 0 &&
            strcmp(opaque_table[i].dir_path, dir_path) == 0) {
            memset(&opaque_table[i], 0, sizeof(struct opaque_dir));
            return 0;
        }
    }
    return -ENOENT;
}

/* ── Stub: overlay_enhance_mount ─────────────────────────────── */
int overlay_enhance_mount(const char *lower, const char *upper, const char *work, const char *target)
{
    (void)lower;
    (void)upper;
    (void)work;
    (void)target;
    kprintf("[overlay] overlay_enhance_mount: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: overlay_enhance_umount ─────────────────────────────── */
int overlay_enhance_umount(const char *target)
{
    (void)target;
    kprintf("[overlay] overlay_enhance_umount: not yet implemented\n");
    return -ENOSYS;
}
