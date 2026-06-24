#include "debugfs.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "export.h"

static struct debugfs_entry debugfs_entries[DEBUGFS_MAX_ENTRIES];
static int debugfs_mounted = 0;

/* debugfs uses a flat namespace under /sys/kernel/debug/<name> */

static int alloc_entry(void) {
    for (int i = 0; i < DEBUGFS_MAX_ENTRIES; i++) {
        if (!debugfs_entries[i].in_use) {
            debugfs_entries[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

static int find_entry(const char *name) {
    for (int i = 0; i < DEBUGFS_MAX_ENTRIES; i++) {
        if (!debugfs_entries[i].in_use) continue;
        if (strcmp(debugfs_entries[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* ── Public API ────────────────────────────────────────────────── */

int debugfs_create_file(const char *name,
                        void (*read_fn)(char *buf, int *len)) {
    if (find_entry(name) >= 0) return -1;
    int idx = alloc_entry();
    if (idx < 0) return -1;
    int nlen = (int)strlen(name);
    if (nlen >= DEBUGFS_MAX_NAME) nlen = DEBUGFS_MAX_NAME - 1;
    memcpy(debugfs_entries[idx].name, name, (size_t)nlen);
    debugfs_entries[idx].name[nlen] = '\0';
    debugfs_entries[idx].type = 1; /* callback read */
    debugfs_entries[idx].read_fn = read_fn;
    debugfs_entries[idx].write_fn = NULL;
    debugfs_entries[idx].u32_val = NULL;
    return 0;
}

int debugfs_create_rw_file(const char *name,
                           void (*read_fn)(char *buf, int *len),
                           int (*write_fn)(const char *buf, int len)) {
    if (find_entry(name) >= 0) return -1;
    int idx = alloc_entry();
    if (idx < 0) return -1;
    int nlen = (int)strlen(name);
    if (nlen >= DEBUGFS_MAX_NAME) nlen = DEBUGFS_MAX_NAME - 1;
    memcpy(debugfs_entries[idx].name, name, (size_t)nlen);
    debugfs_entries[idx].name[nlen] = '\0';
    debugfs_entries[idx].type = 3; /* callback rw */
    debugfs_entries[idx].read_fn = read_fn;
    debugfs_entries[idx].write_fn = write_fn;
    debugfs_entries[idx].u32_val = NULL;
    return 0;
}

int debugfs_create_u32(const char *name, uint32_t *val) {
    if (find_entry(name) >= 0) return -1;
    int idx = alloc_entry();
    if (idx < 0) return -1;
    int nlen = (int)strlen(name);
    if (nlen >= DEBUGFS_MAX_NAME) nlen = DEBUGFS_MAX_NAME - 1;
    memcpy(debugfs_entries[idx].name, name, (size_t)nlen);
    debugfs_entries[idx].name[nlen] = '\0';
    debugfs_entries[idx].type = 2; /* u32 */
    debugfs_entries[idx].read_fn = NULL;
    debugfs_entries[idx].write_fn = NULL;
    debugfs_entries[idx].u32_val = val;
    return 0;
}

/* ── VFS operations ────────────────────────────────────────────── */

static int debugfs_vfs_read(void *priv, const char *path, void *buf,
                            uint32_t max_size, uint32_t *out_size) {
    (void)priv;
    /* path will be like /sys/kernel/debug/<name> */
    const char *prefix = "/sys/kernel/debug/";
    int plen = (int)strlen(prefix);
    const char *name = path;
    if (strncmp(path, prefix, (size_t)plen) == 0)
        name = path + plen;

    int idx = find_entry(name);
    if (idx < 0) return -1;

    char *cbuf = (char *)buf;

    if (debugfs_entries[idx].type == 1 && debugfs_entries[idx].read_fn) {
        int len = 0;
        debugfs_entries[idx].read_fn(cbuf, &len);
        if ((size_t)len > max_size) len = (int)max_size;
        if (len < 0) len = 0;
        cbuf[len] = '\0';
        *out_size = (uint32_t)len;
        return 0;
    } else if (debugfs_entries[idx].type == 2 && debugfs_entries[idx].u32_val) {
        /* Format u32 as decimal string */
        uint32_t v = *debugfs_entries[idx].u32_val;
        char tmp[16];
        int pos = 0;
        if (v == 0) { tmp[pos++] = '0'; }
        else {
            char rev[12]; int ri = 0;
            while (v) { rev[ri++] = '0' + (int)(v % 10); v /= 10; }
            while (ri > 0) tmp[pos++] = rev[--ri];
        }
        tmp[pos++] = '\n';
        uint32_t copy = (uint32_t)pos < max_size ? (uint32_t)pos : max_size;
        memcpy(cbuf, tmp, copy);
        *out_size = copy;
        return 0;
    } else if (debugfs_entries[idx].type == 3 && debugfs_entries[idx].read_fn) {
        int len = 0;
        debugfs_entries[idx].read_fn(cbuf, &len);
        if ((size_t)len > max_size) len = (int)max_size;
        if (len < 0) len = 0;
        cbuf[len] = '\0';
        *out_size = (uint32_t)len;
        return 0;
    }
    return -1;
}

static int debugfs_vfs_write(void *priv, const char *path, const void *data, uint32_t size) {
    (void)priv;
    const char *prefix = "/sys/kernel/debug/";
    int plen = (int)strlen(prefix);
    const char *name = path;
    if (strncmp(path, prefix, (size_t)plen) == 0)
        name = path + plen;

    int idx = find_entry(name);
    if (idx < 0) return -1;

    if (debugfs_entries[idx].type == 2 && debugfs_entries[idx].u32_val) {
        /* Parse decimal from data */
        const char *s = (const char *)data;
        uint32_t v = 0;
        for (uint32_t i = 0; i < size; i++) {
            if (s[i] >= '0' && s[i] <= '9')
                v = v * 10 + (uint32_t)(s[i] - '0');
            else if (s[i] == '\n' || s[i] == '\0')
                break;
        }
        *debugfs_entries[idx].u32_val = v;
        return (int)size;
    } else if (debugfs_entries[idx].type == 3 && debugfs_entries[idx].write_fn) {
        int ret = debugfs_entries[idx].write_fn((const char *)data, (int)size);
        if (ret < 0) return ret;
        return (int)size;
    }

    return -1;
}

static int debugfs_vfs_stat(void *priv, const char *path, struct vfs_stat *st) {
    (void)priv;
    const char *prefix = "/sys/kernel/debug/";
    int plen = (int)strlen(prefix);
    const char *name = path;
    if (strncmp(path, prefix, (size_t)plen) == 0)
        name = path + plen;

    /* Root directory */
    if (strcmp(path, "/sys/kernel/debug") == 0 || strcmp(path, "/sys/kernel/debug/") == 0) {
        st->type = 2; st->size = 0; return 0;
    }

    int idx = find_entry(name);
    if (idx < 0) return -1;
    st->type = 1; st->size = 64;
    st->uid = 0; st->gid = 0; st->mode = 0644;
    return 0;
}

static int debugfs_vfs_readdir(void *priv, const char *path) {
    (void)priv;
    const char *prefix = "/sys/kernel/debug";
    if (strcmp(path, prefix) == 0 || strcmp(path, "/sys/kernel/debug/") == 0) {
        for (int i = 0; i < DEBUGFS_MAX_ENTRIES; i++) {
            if (!debugfs_entries[i].in_use) continue;
            const char *t = (debugfs_entries[i].type == 2) ? "D" : "F";
            kprintf("  [%s] %s\n", t, debugfs_entries[i].name);
        }
        return 0;
    }
    return -1;
}

struct vfs_ops debugfs_vfs_ops = {
    .read    = debugfs_vfs_read,
    .write   = debugfs_vfs_write,
    .stat    = debugfs_vfs_stat,
    .create  = NULL,
    .unlink  = NULL,
    .readdir = debugfs_vfs_readdir,
};

/* ── Initialisation ────────────────────────────────────────────── */

#ifdef MODULE
#include "module.h"

/* Module entry point — called by the module ELF loader on insmod */
int init_module(void) {
    if (debugfs_mounted) return 0; /* already initialised */
    debugfs_init(); /* calls the built-in init which mounts VFS */
    return 0;
}

/* Module exit point — called by the module ELF loader on rmmod */
void cleanup_module(void) {
    if (debugfs_mounted) {
        debugfs_mounted = 0;
        for (int i = 0; i < DEBUGFS_MAX_ENTRIES; i++)
            debugfs_entries[i].in_use = 0;
        kprintf("[debugfs] Module unloaded\n");
    }
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Debugfs virtual filesystem — exposes kernel debug data via /sys/kernel/debug");
MODULE_VERSION("1.0");
#else /* !MODULE — built-in, called directly from kernel boot path */

void __init debugfs_init(void) {
    if (debugfs_mounted) return;

    for (int i = 0; i < DEBUGFS_MAX_ENTRIES; i++)
        debugfs_entries[i].in_use = 0;

    /* Mount under /sys/kernel/debug */
    if (vfs_mount("/sys/kernel/debug", &debugfs_vfs_ops, NULL) == 0) {
        kprintf("[OK] debugfs mounted on /sys/kernel/debug\n");
    } else {
        kprintf("[!!] debugfs mount failed\n");
    }

    debugfs_mounted = 1;
}
#endif /* MODULE */

/* ── debugfs_create_dir ────────────────────────────────── */
int debugfs_create_dir(const char *name, void *parent)
{
    (void)parent;
    kprintf("[debugfs] Created dir: %s\n", name);
    return 0;
}
/* ── debugfs_remove ───────────────────────────────────── */
int debugfs_remove(void *entry)
{
    (void)entry;
    kprintf("[debugfs] Removed entry\n");
    return 0;
}
