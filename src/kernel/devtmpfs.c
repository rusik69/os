#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "kernel.h"
#include "devtmpfs.h"
#include "vfs.h"
#include "errno.h"

/* Static device node table */
static struct devtmpfs_node dev_nodes[DEV_MAX_NODES];
static int num_dev_nodes = 0;
static int devtmpfs_initialised = 0;

void __init devtmpfs_init(void)
{
    if (devtmpfs_initialised)
        return;

    memset(dev_nodes, 0, sizeof(dev_nodes));
    num_dev_nodes = 0;
    devtmpfs_initialised = 1;
    kprintf("[OK] devtmpfs: device filesystem initialised\n");
}

/* Find a free device node slot */
static int dev_find_free(void)
{
    for (int i = 0; i < DEV_MAX_NODES; i++) {
        if (!dev_nodes[i].in_use)
            return i;
    }
    return -ENOSPC;
}

/* Find a node by name */
static int dev_find_by_name(const char *name)
{
    for (int i = 0; i < DEV_MAX_NODES; i++) {
        if (dev_nodes[i].in_use && strcmp(dev_nodes[i].name, name) == 0)
            return i;
    }
    return -ENOENT;
}

int devtmpfs_mknod(const char *path, uint8_t type, uint32_t major, uint32_t minor)
{
    if (!path || (type != DT_CHAR && type != DT_BLOCK))
        return -EINVAL;

    if (!devtmpfs_initialised)
        return 0;

    /* Extract the basename from the path */
    const char *name = path;
    const char *slash = strrchr(path, '/');
    if (slash)
        name = slash + 1;

    /* Check if already exists */
    int idx = dev_find_by_name(name);
    if (idx >= 0)
        return -EEXIST;

    /* Find a free slot */
    idx = dev_find_free();
    if (idx < 0)
        return -ENOSPC;

    struct devtmpfs_node *node = &dev_nodes[idx];
    node->in_use = 1;
    strncpy(node->name, name, sizeof(node->name) - 1);
    node->name[sizeof(node->name) - 1] = '\0';
    node->type  = type;
    node->major = major;
    node->minor = minor;
    num_dev_nodes++;

    /* Create the file entry in VFS (tmpfs on /dev) */
    int ret = vfs_create(path, 1); /* type=1=file */
    if (ret != 0 && ret != -EEXIST) {
        /* VFS doesn't support device nodes directly; we just track them */
        kprintf("devtmpfs: warning vfs_create(%s) returned %d\n", path, ret);
    }

    kprintf("devtmpfs: %s %s %u:%u\n",
            type == DT_CHAR ? "char" : "block", name, major, minor);
    return 0;
}

int devtmpfs_create_device(const char *name, uint8_t type, uint32_t major, uint32_t minor)
{
    char path[96];
    int ret = snprintf(path, sizeof(path), "/dev/%s", name);
    if (ret < 0 || (size_t)ret >= sizeof(path))
        return -ENAMETOOLONG;

    return devtmpfs_mknod(path, type, major, minor);
}

/* Remove a device node by name from the devtmpfs table.
 * Finds the node with matching name, marks it unused, and removes
 * the VFS entry.  Returns 0 on success, -ENOENT if not found. */
static int devtmpfs_delete_node(const char *name)
{
    if (!devtmpfs_initialised || !name)
        return 0;

    int found = -1;
    for (int i = 0; i < DEV_MAX_NODES; i++) {
        if (dev_nodes[i].in_use &&
            strcmp(dev_nodes[i].name, name) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0)
        return -ENOENT;

    memset(&dev_nodes[found], 0, sizeof(struct devtmpfs_node));
    num_dev_nodes--;

    /* Remove the VFS entry under /dev/ */
    char vfs_path[128];
    snprintf(vfs_path, sizeof(vfs_path), "/dev/%s", name);
    vfs_unlink(vfs_path);

    kprintf("[devtmpfs] removed device node '%s'\n", name);
    return 0;
}

int devtmpfs_setup(void)
{
    if (!devtmpfs_initialised)
        return 0;

    /* Ensure /dev exists in VFS */
    struct vfs_stat st;
    int ret = vfs_stat("/dev", &st);
    if (ret != 0) {
        ret = vfs_create("/dev", 2); /* create directory */
        if (ret != 0) {
            kprintf("devtmpfs: could not create /dev (error %d)\n", ret);
            return ret;
        }
    }

    /* Create standard device nodes */
    /* Console (tty) — char major 5, minor 1 */
    devtmpfs_mknod("/dev/console", DT_CHAR, 5, 1);
    /* Serial port — char major 4, minor 64 (ttyS0) */
    devtmpfs_mknod("/dev/ttyS0", DT_CHAR, 4, 64);
    /* Null — char major 1, minor 3 */
    devtmpfs_mknod("/dev/null", DT_CHAR, 1, 3);
    /* Zero — char major 1, minor 5 */
    devtmpfs_mknod("/dev/zero", DT_CHAR, 1, 5);
    /* Random — char major 1, minor 8 */
    devtmpfs_mknod("/dev/random", DT_CHAR, 1, 8);
    /* Urandom — char major 1, minor 9 */
    devtmpfs_mknod("/dev/urandom", DT_CHAR, 1, 9);
    /* Full — char major 1, minor 7 */
    devtmpfs_mknod("/dev/full", DT_CHAR, 1, 7);
    /* TTY — char major 5, minor 0 */
    devtmpfs_mknod("/dev/tty", DT_CHAR, 5, 0);

    /* Block devices */
    /* SDA — block major 8, minor 0 */
    devtmpfs_mknod("/dev/sda", DT_BLOCK, 8, 0);
    /* Loop0 — block major 7, minor 0 */
    devtmpfs_mknod("/dev/loop0", DT_BLOCK, 7, 0);

    kprintf("[OK] devtmpfs: /dev populated with %d device nodes\n", num_dev_nodes);
    return 0;
}

struct devtmpfs_node *devtmpfs_get_table(int *count)
{
    if (count)
        *count = num_dev_nodes;
    return dev_nodes;
}

/* ── Stub: devtmpfs_mount ─────────────────────────────── */
static int devtmpfs_mount(const char *target)
{
    (void)target;
    kprintf("[devtmpfs] devtmpfs_mount: not yet implemented\n");
    return 0;
}
/* ── Stub: devtmpfs_create_node ─────────────────────────────── */
static int devtmpfs_create_node(void *dev)
{
    (void)dev;
    kprintf("[devtmpfs] devtmpfs_create_node: not yet implemented\n");
    return 0;
}
