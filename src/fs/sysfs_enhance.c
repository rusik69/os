/* sysfs_enhance.c — sysfs enhancement: binary attributes, file ops groups
 *
 * Extends the basic sysfs with:
 *   - Binary attributes: read/write raw binary data via sysfs files
 *   - File operations groups: batch read/write operations
 *   - Enhanced attribute management
 *
 * These enhancements allow kernel subsystems to expose binary data
 * (firmware blobs, configuration dumps, register snapshots) through
 * the sysfs interface.
 */

#include "types.h"
#include "sysfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"
#include "errno.h"

/* ── Binary attribute support ──────────────────────────────────────── */

struct sysfs_bin_attr {
    char     name[SYSFS_MAX_NAME];
    void    *priv_data;
    size_t   size;
    int      (*read_fn)(struct sysfs_bin_attr *attr, uint64_t offset,
                        char *buf, size_t len);
    int      (*write_fn)(struct sysfs_bin_attr *attr, uint64_t offset,
                         const char *buf, size_t len);
    int      in_use;
};

#define SYSFS_MAX_BIN_ATTRS 64
static struct sysfs_bin_attr g_sysfs_bin_attrs[SYSFS_MAX_BIN_ATTRS];
static int g_sysfs_num_bin = 0;

/* ── Create a binary attribute ─────────────────────────────────────── */

int sysfs_create_bin_attr(const char *name, size_t size,
                           void *priv,
                           int (*read_fn)(struct sysfs_bin_attr *,
                                          uint64_t, char *, size_t),
                           int (*write_fn)(struct sysfs_bin_attr *,
                                           uint64_t, const char *, size_t))
{
    if (!name || g_sysfs_num_bin >= SYSFS_MAX_BIN_ATTRS)
        return -ENOSPC;

    int idx = g_sysfs_num_bin;
    struct sysfs_bin_attr *attr = &g_sysfs_bin_attrs[idx];

    strncpy(attr->name, name, SYSFS_MAX_NAME - 1);
    attr->size = size;
    attr->priv_data = priv;
    attr->read_fn = read_fn;
    attr->write_fn = write_fn;
    attr->in_use = 1;

    /* Also create a regular sysfs entry pointing to this binary attr */
    sysfs_create_file(name, NULL, NULL, NULL);

    g_sysfs_num_bin++;
    return idx;
}

/* ── Binary attribute read dispatch ────────────────────────────────── */

int sysfs_bin_read(const char *name, uint64_t offset,
                    char *buf, size_t len)
{
    for (int i = 0; i < g_sysfs_num_bin; i++) {
        if (g_sysfs_bin_attrs[i].in_use &&
            strcmp(g_sysfs_bin_attrs[i].name, name) == 0) {
            if (g_sysfs_bin_attrs[i].read_fn) {
                return g_sysfs_bin_attrs[i].read_fn(
                    &g_sysfs_bin_attrs[i], offset, buf, len);
            }
            return -ENOSYS;
        }
    }
    return -ENOENT;
}

/* ── Binary attribute write dispatch ───────────────────────────────── */

int sysfs_bin_write(const char *name, uint64_t offset,
                     const char *buf, size_t len)
{
    for (int i = 0; i < g_sysfs_num_bin; i++) {
        if (g_sysfs_bin_attrs[i].in_use &&
            strcmp(g_sysfs_bin_attrs[i].name, name) == 0) {
            if (g_sysfs_bin_attrs[i].write_fn) {
                return g_sysfs_bin_attrs[i].write_fn(
                    &g_sysfs_bin_attrs[i], offset, buf, len);
            }
            return -ENOSYS;
        }
    }
    return -ENOENT;
}

/* ── File operations group support ─────────────────────────────────── */

struct sysfs_file_ops_group {
    const char                *name;
    const struct sysfs_ops   *ops;
    int                        num_attrs;
    int                        in_use;
};

#define SYSFS_MAX_OPS_GROUPS 32
static struct sysfs_file_ops_group g_sysfs_groups[SYSFS_MAX_OPS_GROUPS];
static int g_sysfs_num_groups = 0;

/* ── Register a file ops group ─────────────────────────────────────── */

int sysfs_register_ops_group(const char *name,
                              const struct sysfs_ops *ops,
                              int num_attrs)
{
    if (!name || !ops || g_sysfs_num_groups >= SYSFS_MAX_OPS_GROUPS)
        return -ENOSPC;

    int idx = g_sysfs_num_groups;
    struct sysfs_file_ops_group *grp = &g_sysfs_groups[idx];

    grp->name = name;
    grp->ops = ops;
    grp->num_attrs = num_attrs;
    grp->in_use = 1;

    /* Create sysfs files for each attribute in the group */
    for (int i = 0; i < num_attrs; i++) {
        sysfs_create_file(ops[i].name,
                          ops[i].read, ops[i].write,
                          (void *)ops);
    }

    g_sysfs_num_groups++;
    return idx;
}

/* ── Initialization ────────────────────────────────────────────────── */

void sysfs_enhance_init(void)
{
    memset(g_sysfs_bin_attrs, 0, sizeof(g_sysfs_bin_attrs));
    memset(g_sysfs_groups, 0, sizeof(g_sysfs_groups));

    kprintf("[OK] SYSFS_ENHANCE: binary attributes + file ops groups active\n");
}
