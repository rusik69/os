/*
 * src/fs/romfs.c — ROMFS (simple read-only filesystem).
 *
 * Implements a minimal ROMFS filesystem. The format is simple:
 * each file has a 32-byte header followed by its data, then
 * the next header is at the next 16-byte boundary after the
 * data ends.
 */

#define KERNEL_INTERNAL
#include "romfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"

/* Max entries we track */
#define ROMFS_MAX_ENTRIES 128

struct romfs_entry {
    char   name[64];
    uint32_t offset;    /* offset from base where file data starts */
    uint32_t size;
    uint8_t  is_dir;
};

struct romfs_priv {
    uint32_t base_addr;
    uint32_t total_size;
    struct romfs_entry entries[ROMFS_MAX_ENTRIES];
    int num_entries;
};

/* Parse ROMFS image */
static int romfs_parse(struct romfs_priv *rp) {
    uint8_t *base = (uint8_t *)(uint64_t)rp->base_addr;

    if (rp->total_size < 32) return -1;

    struct romfs_super *super = (struct romfs_super *)base;
    if (super->magic != 0x6D6F682D && super->magic != 0x2D686F6D) {
        /* Try byte-swapped magic */
        kprintf("[romfs] Bad magic: 0x%x\n", super->magic);
        return -1;
    }

    uint32_t full_size = super->full_size;
    if (full_size > rp->total_size)
        full_size = rp->total_size;

    kprintf("[romfs] Volume: %s, size: %u\n", super->volume_name, full_size);

    /* First file starts after superblock (32 bytes), aligned to 16 */
    uint32_t offset = 32;
    int count = 0;

    while (offset < full_size && count < ROMFS_MAX_ENTRIES) {
        struct romfs_file *fh = (struct romfs_file *)(base + offset);
        if (fh->next == 0) break;

        uint32_t next = fh->next & ~1u; /* clear the directory flag */
        int is_dir = (fh->next & ROMFS_NEXT_IS_DIR) ? 1 : 0;

        /* Find name length */
        int name_len = 0;
        for (int i = 0; i < 16; i++) {
            if (fh->name[i] == '\0') { name_len = i; break; }
        }
        if (name_len > 63) name_len = 63;

        struct romfs_entry *e = &rp->entries[count];
        memcpy(e->name, fh->name, name_len);
        e->name[name_len] = '\0';
        e->offset = offset + sizeof(struct romfs_file); /* data starts after header */
        e->size   = fh->size;
        e->is_dir = (uint8_t)is_dir;
        count++;

        if (next == 0 || next <= offset) break;
        offset = next;
    }

    rp->num_entries = count;
    return count;
}

/* Find entry by path */
static struct romfs_entry *romfs_find(struct romfs_priv *rp, const char *path) {
    const char *p = path;
    if (*p == '/') p++;

    for (int i = 0; i < rp->num_entries; i++) {
        struct romfs_entry *e = &rp->entries[i];
        if (strcmp(e->name, p) == 0)
            return e;
    }
    return NULL;
}

/* VFS operations */

static int romfs_read(void *priv, const char *path, void *buf,
                      uint32_t max_size, uint32_t *out_size) {
    struct romfs_priv *rp = (struct romfs_priv *)priv;
    struct romfs_entry *e = romfs_find(rp, path);
    if (!e || e->is_dir) return -1;

    uint32_t to_read = e->size;
    if (to_read > max_size) to_read = max_size;

    uint8_t *base = (uint8_t *)(uint64_t)rp->base_addr;
    memcpy(buf, base + e->offset, to_read);
    *out_size = to_read;
    return 0;
}

static int romfs_stat(void *priv, const char *path, struct vfs_stat *st) {
    struct romfs_priv *rp = (struct romfs_priv *)priv;
    memset(st, 0, sizeof(*st));

    const char *p = path;
    if (*p == '/') p++;
    if (*p == '\0') {
        st->type = 2;
        st->mode = 0555;
        return 0;
    }

    struct romfs_entry *e = romfs_find(rp, path);
    if (!e) return -1;

    st->size = e->size;
    st->type = e->is_dir ? 2 : 1;
    st->mode = e->is_dir ? 0555 : 0444;
    return 0;
}

static int romfs_readdir_names(void *priv, const char *path, char names[][64], int max) {
    struct romfs_priv *rp = (struct romfs_priv *)priv;
    const char *prefix = path;
    if (*prefix == '/') prefix++;
    size_t plen = strlen(prefix);

    int count = 0;
    for (int i = 0; i < rp->num_entries && count < max; i++) {
        struct romfs_entry *e = &rp->entries[i];

        if (plen == 0) {
            /* Root: list all top-level entries */
            if (strchr(e->name, '/') == NULL) {
                int nlen = (int)strlen(e->name);
                if (nlen > 63) nlen = 63;
                memcpy(names[count], e->name, nlen);
                names[count][nlen] = '\0';
                count++;
            }
        } else {
            if (strncmp(e->name, prefix, plen) == 0 && e->name[plen] == '/') {
                const char *rest = e->name + plen + 1;
                const char *slash = strchr(rest, '/');
                int nlen;
                if (slash) {
                    nlen = (int)(slash - rest);
                } else {
                    nlen = (int)strlen(rest);
                }
                if (nlen > 63) nlen = 63;
                /* Check for duplicates */
                int found = 0;
                for (int j = 0; j < count; j++) {
                    if (strncmp(names[j], rest, nlen) == 0 && names[j][nlen] == '\0') {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    memcpy(names[count], rest, nlen);
                    names[count][nlen] = '\0';
                    count++;
                }
            }
        }
    }
    return count;
}

static int romfs_readdir_legacy(void *priv, const char *path) {
    char names[64][64];
    int n = romfs_readdir_names(priv, path, names, 64);
    for (int i = 0; i < n; i++)
        kprintf("  %s\n", names[i]);
    return n;
}

static struct vfs_ops romfs_ops = {
    .read    = romfs_read,
    .stat    = romfs_stat,
    .readdir_names = romfs_readdir_names,
    .readdir = romfs_readdir_legacy,
};

int romfs_mount(const char *mountpoint, uint32_t addr, uint32_t size) {
    struct romfs_priv *rp = (struct romfs_priv *)kmalloc(sizeof(struct romfs_priv));
    if (!rp) return -1;

    memset(rp, 0, sizeof(*rp));
    rp->base_addr  = addr;
    rp->total_size = size;

    int n = romfs_parse(rp);
    if (n <= 0) {
        kfree(rp);
        return -1;
    }

    kprintf("[romfs] %d entries at %s\n", n, mountpoint);
    return vfs_mount_ex(mountpoint, &romfs_ops, rp, MS_RDONLY);
}

int __init romfs_init(void) {
    kprintf("[romfs] ROMFS initialized\n");
    vfs_register_filesystem("romfs", &romfs_ops);
    return 0;
}

#ifdef MODULE
#include "module.h"

/* Module entry point — called by the module ELF loader on insmod */
int init_module(void) {
    return romfs_init();
}

/* Module exit point — called by the module ELF loader on rmmod */
void __exit cleanup_module(void) {
    /* No VFS unregister yet; avoid unloading if filesystem is mounted */
}

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("ROMFS simple read-only filesystem — loadable module");
#endif

/* ── romfs_umount ──────────────────────────────────────── */
static int romfs_umount(const char *target)
{
    (void)target;
    kprintf("[romfs] ROMFS unmounted\n");
    return 0;
}
/* ── romfs_readdir ─────────────────────────────────────── */
static int romfs_readdir(void *dir, void *filldir)
{
    (void)dir;
    (void)filldir;
    kprintf("[romfs] readdir (no more entries)\n");
    return 0;
}
/* ── romfs_lookup ──────────────────────────────────────── */
static int romfs_lookup(const char *name, void *parent)
{
    (void)parent;
    kprintf("[romfs] lookup: %s\n", name);
    return -ENOENT;
}
