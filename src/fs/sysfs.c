#include "sysfs.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "panic.h"         /* for panic_timeout */
#include "dmesg.h"         /* for dmesg_restrict */
#include "kptr_restrict.h" /* for kptr_restrict */

static struct sysfs_entry sysfs_entries[SYSFS_MAX_ENTRIES];
static int sysfs_mounted = 0;

/* ── Forward declarations ──────────────────────────────────────────── */
static void sysfs_clear_entry(int idx);

/* ── helpers ───────────────────────────────────────────────────── */

static int alloc_entry(void) {
    for (int i = 0; i < SYSFS_MAX_ENTRIES; i++) {
        if (!sysfs_entries[i].in_use) {
            sysfs_entries[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

static int find_entry(const char *path) {
    if (!path || path[0] != '/') return -1;
    /* Skip leading "/sys" if present */
    const char *rel = path;
    if (strncmp(path, "/sys", 4) == 0) {
        rel = path + 4;
        if (*rel == '\0') rel = "/";
    }
    if (rel[0] == '/' && rel[1] == '\0') return 0; /* root */

    /* Trim trailing slash */
    const char *name = rel + 1;
    int len = (int)strlen(name);
    if (len > 0 && name[len - 1] == '/') len--;

    for (int i = 0; i < SYSFS_MAX_ENTRIES; i++) {
        if (!sysfs_entries[i].in_use) continue;
        if ((int)strlen(sysfs_entries[i].name) == len &&
            memcmp(sysfs_entries[i].name, name, (size_t)len) == 0)
            return i;
    }
    return -1;
}

/* Create entry under parent dir */
static int create_entry(const char *name, uint8_t type, const char *content,
                        int parent, void *priv,
                        sysfs_read_cb_t read_cb, sysfs_write_cb_t write_cb) {
    int idx = alloc_entry();
    if (idx < 0) return -1;
    int nlen = (int)strlen(name);
    if (nlen >= SYSFS_MAX_NAME) nlen = SYSFS_MAX_NAME - 1;
    memcpy(sysfs_entries[idx].name, name, (size_t)nlen);
    sysfs_entries[idx].name[nlen] = '\0';
    sysfs_entries[idx].type = type;
    sysfs_entries[idx].parent = parent;
    sysfs_entries[idx].size = 0;
    sysfs_entries[idx].priv = priv;
    sysfs_entries[idx].read_cb = read_cb;
    sysfs_entries[idx].write_cb = write_cb;
    sysfs_entries[idx].release_cb = NULL;
    sysfs_entries[idx].content[0] = '\0';
    if (content) {
        int clen = (int)strlen(content);
        if (clen > 255) clen = 255;
        memcpy(sysfs_entries[idx].content, content, (size_t)clen);
        sysfs_entries[idx].content[clen] = '\0';
        sysfs_entries[idx].size = (uint32_t)clen;
    }
    return idx;
}

/* ── Public API ────────────────────────────────────────────────── */

int sysfs_set_release_cb(const char *path, sysfs_release_cb_t release_cb)
{
    int idx = find_entry(path);
    if (idx < 0)
        return -1;
    sysfs_entries[idx].release_cb = release_cb;
    return 0;
}

int sysfs_create_file(const char *path, const char *content) {
    /* Find parent directory */
    char dirpath[128];
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) return -1;
    int dirlen = (int)(slash - path);
    if (dirlen > 126) return -1;
    memcpy(dirpath, path, (size_t)dirlen); dirpath[dirlen] = '\0';
    int parent = find_entry(dirpath);
    if (parent < 0) return -1;
    if (sysfs_entries[parent].type != 2) return -1;

    const char *name = slash + 1;
    if (create_entry(name, 1, content, parent, NULL, NULL, NULL) < 0) return -1;
    return 0;
}

int sysfs_create_writable_file(const char *path, const char *initial_content,
                                void *priv,
                                sysfs_read_cb_t read_cb, sysfs_write_cb_t write_cb) {
    /* Find parent directory */
    char dirpath[128];
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) return -1;
    int dirlen = (int)(slash - path);
    if (dirlen > 126) return -1;
    memcpy(dirpath, path, (size_t)dirlen); dirpath[dirlen] = '\0';
    int parent = find_entry(dirpath);
    if (parent < 0) return -1;
    if (sysfs_entries[parent].type != 2) return -1;

    const char *name = slash + 1;
    if (create_entry(name, 1, initial_content, parent, priv, read_cb, write_cb) < 0) return -1;
    return 0;
}

int sysfs_create_dir(const char *path) {
    /* Find parent */
    char dirpath[128];
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) return -1;
    int dirlen = (int)(slash - path);
    if (dirlen > 126) return -1;
    memcpy(dirpath, path, (size_t)dirlen); dirpath[dirlen] = '\0';
    int parent;
    if (dirlen == 0 || (dirlen == 1 && dirpath[0] == '/')) {
        parent = 0; /* root */
    } else {
        parent = find_entry(dirpath);
        if (parent < 0) return -1;
    }
    if (sysfs_entries[parent].type != 2) return -1;

    const char *name = slash + 1;
    if (create_entry(name, 2, NULL, parent, NULL, NULL, NULL) < 0) return -1;
    return 0;
}

/* ── VFS operations ────────────────────────────────────────────── */

static int sysfs_vfs_read(void *priv, const char *path, void *buf,
                          uint32_t max_size, uint32_t *out_size) {
    (void)priv;
    int idx = find_entry(path);
    if (idx < 0 || sysfs_entries[idx].type != 1)
        return -1;

    /* Use dynamic read callback if available */
    if (sysfs_entries[idx].read_cb) {
        int ret = sysfs_entries[idx].read_cb((char *)buf, max_size, sysfs_entries[idx].priv);
        if (ret < 0) return -1;
        *out_size = (uint32_t)ret;
        return 0;
    }

    /* Fall back to static content */
    uint32_t copy = sysfs_entries[idx].size < max_size ? sysfs_entries[idx].size : max_size;
    if (copy > 0)
        memcpy(buf, sysfs_entries[idx].content, copy);
    *out_size = copy;
    return 0;
}

static int sysfs_vfs_write(void *priv, const char *path, const void *data, uint32_t size) {
    (void)priv;
    int idx = find_entry(path);
    if (idx < 0 || sysfs_entries[idx].type != 1)
        return -1;

    /* Use write callback if available */
    if (sysfs_entries[idx].write_cb) {
        return sysfs_entries[idx].write_cb((const char *)data, size, sysfs_entries[idx].priv);
    }

    return -1; /* read-only if no write callback */
}

static int sysfs_vfs_stat(void *priv, const char *path, struct vfs_stat *st) {
    (void)priv;
    int idx = find_entry(path);
    if (idx < 0) return -1;
    st->size = sysfs_entries[idx].size;
    st->type = sysfs_entries[idx].type; /* 1=file, 2=dir */
    st->uid = 0; st->gid = 0;
    st->mode = 0444;
    return 0;
}

static int sysfs_vfs_create(void *priv, const char *path, uint8_t type) {
    (void)priv; (void)path; (void)type;
    return -1; /* read-only */
}

static int sysfs_vfs_unlink(void *priv, const char *path) {
    (void)priv; (void)path;
    return -1; /* read-only */
}

static int sysfs_vfs_readdir(void *priv, const char *path) {
    (void)priv;
    int idx = find_entry(path);
    if (idx < 0 || sysfs_entries[idx].type != 2) return -1;
    for (int i = 0; i < SYSFS_MAX_ENTRIES; i++) {
        if (!sysfs_entries[i].in_use) continue;
        if (sysfs_entries[i].parent != idx && !(idx == 0 && i == 0)) continue;
        if (i == idx) continue;
        const char *t = (sysfs_entries[i].type == 2) ? "D" : "F";
        kprintf("  [%s] %s (%u bytes)\n", t, sysfs_entries[i].name, sysfs_entries[i].size);
    }
    return 0;
}

static int sysfs_vfs_readdir_names(void *priv, const char *path, char names[][64], int max) {
    (void)priv;
    int idx = find_entry(path);
    if (idx < 0 || sysfs_entries[idx].type != 2) return 0;
    int count = 0;
    for (int i = 0; i < SYSFS_MAX_ENTRIES && count < max; i++) {
        if (!sysfs_entries[i].in_use) continue;
        if (sysfs_entries[i].parent != idx && !(idx == 0 && i == 0)) continue;
        if (i == idx) continue;
        int len = (int)strlen(sysfs_entries[i].name);
        int copylen = len < 63 ? len : 63;
        memcpy(names[count], sysfs_entries[i].name, (size_t)copylen);
        names[count][copylen] = '\0';
        count++;
    }
    return count;
}

struct vfs_ops sysfs_vfs_ops = {
    .read        = sysfs_vfs_read,
    .write       = sysfs_vfs_write,
    .stat        = sysfs_vfs_stat,
    .create      = sysfs_vfs_create,
    .unlink      = sysfs_vfs_unlink,
    .readdir     = sysfs_vfs_readdir,
    .readdir_names = sysfs_vfs_readdir_names,
};

/* ── Initialisation ────────────────────────────────────────────── */

/*
 * sysfs_remove — Remove a single sysfs entry by path.
 *
 * Finds the entry, calls its release callback (if set), clears the
 * callbacks, and marks it as unused.  This constitutes the "kobject_del
 * + kobject_put" lifecycle for sysfs entries.
 * Returns 0 on success, -1 if not found.
 */
int sysfs_remove(const char *path)
{
    if (!path || path[0] != '/')
        return -1;

    int idx = find_entry(path);
    if (idx < 0)
        return -1;

    /* Invoke the release callback before tearing down the entry.
     * The callback can still access all entry fields, including priv,
     * so it can properly release any dynamically allocated resources. */
    sysfs_clear_entry(idx);
    return 0;
}

/*
 * Clear a single sysfs entry, invoking its release callback if set.
 * This is the internal workhorse that implements kobject_del + kobject_put.
 */
static void sysfs_clear_entry(int idx)
{
    if (idx < 0 || idx >= SYSFS_MAX_ENTRIES)
        return;
    if (!sysfs_entries[idx].in_use)
        return;

    /* Invoke release callback before clearing */
    if (sysfs_entries[idx].release_cb) {
        sysfs_entries[idx].release_cb(sysfs_entries[idx].priv);
    }

    sysfs_entries[idx].in_use = 0;
    sysfs_entries[idx].read_cb = NULL;
    sysfs_entries[idx].write_cb = NULL;
    sysfs_entries[idx].release_cb = NULL;
    sysfs_entries[idx].content[0] = '\0';
    sysfs_entries[idx].size = 0;
}

/*
 * sysfs_remove_recursive — Remove a directory and all its children.
 *
 * Iterates all entries; removes any whose parent chain leads to @path.
 * The directory itself is removed last.
 */
int sysfs_remove_recursive(const char *path)
{
    if (!path || path[0] != '/')
        return -1;

    int idx = find_entry(path);
    if (idx < 0)
        return -1;
    if (sysfs_entries[idx].type != 2)
        return -1; /* Not a directory */

    /* Remove all children recursively (multi-pass because children may have children) */
    int changed;
    do {
        changed = 0;
        for (int i = 1; i < SYSFS_MAX_ENTRIES; i++) {
            if (!sysfs_entries[i].in_use)
                continue;

            /* Check if this entry's parent is the target dir */
            if (sysfs_entries[i].parent == idx) {
                if (sysfs_entries[i].type == 2) {
                    /* Directory child — remove children of this child too */
                    int sub_idx = i;
                    for (int j = 1; j < SYSFS_MAX_ENTRIES; j++) {
                        if (sysfs_entries[j].in_use && sysfs_entries[j].parent == sub_idx) {
                            sysfs_clear_entry(j);
                        }
                    }
                }
                /* Remove the child entry itself */
                sysfs_clear_entry(i);
                changed = 1;
            }
        }
    } while (changed);

    /* Finally remove the directory itself */
    sysfs_clear_entry(idx);
    return 0;
}

/* ── Read/write callbacks for /sys/kernel/ parameters ───────────── */

/*
 * Read callback for panic_timeout.
 * Returns the current panic_timeout value as an ASCII string.
 */
static int sysfs_read_panic_timeout(char *buf, uint32_t max_sz, void *priv)
{
    (void)priv;
    int n = snprintf(buf, max_sz, "%d\n", panic_timeout);
    if (n < 0)
        return -1;
    return (uint32_t)n < max_sz ? n : (int)(max_sz - 1);
}

/*
 * Write callback for panic_timeout.
 * Parses an integer (possibly with trailing newline) and sets panic_timeout.
 * Clamps to [0, 3600] as a safety measure.
 */
static int sysfs_write_panic_timeout(const char *data, uint32_t size, void *priv)
{
    (void)priv;
    char tmp[32];
    uint32_t copy = size < sizeof(tmp) - 1 ? size : sizeof(tmp) - 1;
    memcpy(tmp, data, copy);
    tmp[copy] = '\0';

    /* Strip trailing whitespace/newline */
    int len = (int)strlen(tmp);
    while (len > 0 && (tmp[len - 1] == '\n' || tmp[len - 1] == ' '))
        tmp[--len] = '\0';
    if (len == 0)
        return -1;

    int val = 0;
    int neg = 0;
    const char *p = tmp;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9')
        val = val * 10 + (*p++ - '0');
    if (*p != '\0')
        return -1; /* non-numeric data */

    if (neg)
        val = -val;

    /* Clamp to a sane range: 0 (disable) to 3600 seconds (1 hour) */
    if (val < 0)    val = 0;
    if (val > 3600) val = 3600;

    panic_timeout = val;
    return 0;
}

/*
 * Read callback for dmesg_restrict.
 */
static int sysfs_read_dmesg_restrict(char *buf, uint32_t max_sz, void *priv)
{
    (void)priv;
    int n = snprintf(buf, max_sz, "%d\n", dmesg_restrict);
    if (n < 0) return -1;
    return (uint32_t)n < max_sz ? n : (int)(max_sz - 1);
}

/*
 * Write callback for dmesg_restrict.
 * Accepts "0" or "1". All other values are rejected.
 */
static int sysfs_write_dmesg_restrict(const char *data, uint32_t size, void *priv)
{
    (void)priv;
    if (size < 1) return -1;
    int val = data[0] - '0';
    if (val != 0 && val != 1) return -1;
    dmesg_restrict = val;
    return 0;
}

/*
 * Read callback for kptr_restrict.
 */
static int sysfs_read_kptr_restrict(char *buf, uint32_t max_sz, void *priv)
{
    (void)priv;
    int n = snprintf(buf, max_sz, "%d\n", kptr_restrict);
    if (n < 0) return -1;
    return (uint32_t)n < max_sz ? n : (int)(max_sz - 1);
}

/*
 * Write callback for kptr_restrict.
 * Accepts 0, 1, or 2. All other values are rejected.
 */
static int sysfs_write_kptr_restrict(const char *data, uint32_t size, void *priv)
{
    (void)priv;
    if (size < 1) return -1;
    int val = data[0] - '0';
    if (val < 0 || val > 2) return -1;
    kptr_restrict = val;
    return 0;
}

void sysfs_init(void) {
    if (sysfs_mounted) return;

    /* Clear all entries */
    for (int i = 0; i < SYSFS_MAX_ENTRIES; i++) {
        sysfs_entries[i].in_use = 0;
        sysfs_entries[i].read_cb = NULL;
        sysfs_entries[i].write_cb = NULL;
    }

    /* Create root directory "sys" at index 0 */
    sysfs_entries[0].in_use = 1;
    sysfs_entries[0].type = 2; /* dir */
    sysfs_entries[0].name[0] = '\0';
    sysfs_entries[0].parent = -1;
    sysfs_entries[0].size = 0;
    sysfs_entries[0].read_cb = NULL;
    sysfs_entries[0].write_cb = NULL;

    /* Pre-populate standard directories */
    sysfs_create_dir("/sys/class");
    sysfs_create_dir("/sys/block");
    sysfs_create_dir("/sys/devices");
    sysfs_create_dir("/sys/kernel");

    /* /sys/class/block/ - list block devices */
    sysfs_create_file("/sys/class/block", "sda\nsdb\n");

    /* /sys/kernel/ files — kernel parameters with read/write callbacks */
    sysfs_create_file("/sys/kernel/version", "OS Kernel v1.0\n");
    sysfs_create_writable_file("/sys/kernel/panic_timeout", "30\n", NULL,
        sysfs_read_panic_timeout, sysfs_write_panic_timeout);
    sysfs_create_writable_file("/sys/kernel/dmesg_restrict", "1\n", NULL,
        sysfs_read_dmesg_restrict, sysfs_write_dmesg_restrict);
    sysfs_create_writable_file("/sys/kernel/kptr_restrict", "2\n", NULL,
        sysfs_read_kptr_restrict, sysfs_write_kptr_restrict);

    /* Mount under /sys */
    if (vfs_mount("/sys", &sysfs_vfs_ops, NULL) == 0) {
        kprintf("[OK] sysfs mounted on /sys\n");
    } else {
        kprintf("[!!] sysfs mount failed\n");
    }

    sysfs_mounted = 1;
}
