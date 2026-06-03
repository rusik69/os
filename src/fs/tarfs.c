/*
 * src/fs/tarfs.c — Read-only tar filesystem.
 *
 * Mounts a tar archive (in memory) as a read-only filesystem
 * via the VFS layer. Supports ustar format.
 */

#define KERNEL_INTERNAL
#include "tarfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"

#ifdef MODULE
#include "module.h"
#endif

/* Maximum entries we can track from a tar archive */
#define TARFS_MAX_ENTRIES 128

struct tarfs_entry {
    char   name[256];
    uint32_t offset;   /* byte offset in archive where file data starts */
    uint32_t size;
    uint8_t  type;     /* 1=file, 2=dir */
};

struct tarfs_priv {
    uint32_t base_addr;        /* start of tar archive in memory */
    uint32_t total_size;       /* size of tar archive */
    struct tarfs_entry entries[TARFS_MAX_ENTRIES];
    int num_entries;
};

/* Parse octal number from field */
static uint32_t parse_octal(const char *s, int len) {
    uint32_t v = 0;
    for (int i = 0; i < len && s[i]; i++) {
        if (s[i] >= '0' && s[i] <= '7')
            v = (v << 3) + (uint32_t)(s[i] - '0');
        else
            break;
    }
    return v;
}

/* Parse the tar archive and populate entry table */
static int tarfs_parse(struct tarfs_priv *priv) {
    uint8_t *base = (uint8_t *)(uint64_t)priv->base_addr;
    uint32_t offset = 0;
    int count = 0;

    while (offset + TAR_BLOCK_SIZE <= priv->total_size && count < TARFS_MAX_ENTRIES) {
        struct tar_header *hdr = (struct tar_header *)(base + offset);

        /* Check for end-of-archive (empty block) */
        if (hdr->name[0] == '\0')
            break;

        /* Check magic */
        if (memcmp(hdr->magic, TAR_MAGIC, 5) != 0 &&
            memcmp(hdr->magic, TAR_MAGIC_OLD, 7) != 0)
            break;

        uint32_t file_size = parse_octal(hdr->size, 12);
        uint32_t data_off  = offset + TAR_BLOCK_SIZE;

        /* Store entry */
        struct tarfs_entry *e = &priv->entries[count];
        memset(e, 0, sizeof(*e));

        /* Copy name (handle prefix for long names) */
        if (hdr->prefix[0]) {
            int plen = (int)strlen(hdr->prefix);
            if (plen > 155) plen = 155;
            memcpy(e->name, hdr->prefix, plen);
            e->name[plen] = '/';
            int nlen = (int)strlen(hdr->name);
            if (nlen > 100) nlen = 100;
            memcpy(e->name + plen + 1, hdr->name, nlen);
            e->name[plen + 1 + nlen] = '\0';
        } else {
            int nlen = (int)strlen(hdr->name);
            if (nlen > 100) nlen = 100;
            memcpy(e->name, hdr->name, nlen);
            e->name[nlen] = '\0';
        }

        e->offset = data_off;
        e->size   = file_size;

        if (hdr->typeflag == TAR_TYPE_DIR || hdr->name[strlen(hdr->name) - 1] == '/')
            e->type = 2; /* dir */
        else
            e->type = 1; /* file */

        count++;

        /* Advance to next header (rounded to block size) */
        uint32_t data_blocks = (file_size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        offset = data_off + data_blocks * TAR_BLOCK_SIZE;
    }

    priv->num_entries = count;
    return count;
}

/* Find entry by path */
static struct tarfs_entry *tarfs_find(struct tarfs_priv *priv, const char *path) {
    const char *p = path;
    if (*p == '/') p++;
    if (*p == '\0') p = "";  /* root */

    for (int i = 0; i < priv->num_entries; i++) {
        struct tarfs_entry *e = &priv->entries[i];
        if (strcmp(e->name, p) == 0)
            return e;
        /* Also handle trailing slash for dirs */
        size_t elen = strlen(e->name);
        if (elen > 0 && e->name[elen - 1] == '/') {
            char tmp[256];
            memcpy(tmp, e->name, elen - 1);
            tmp[elen - 1] = '\0';
            if (strcmp(tmp, p) == 0)
                return e;
        }
    }
    return NULL;
}

/* VFS operations */

static int tarfs_read(void *priv, const char *path, void *buf,
                      uint32_t max_size, uint32_t *out_size) {
    struct tarfs_priv *tp = (struct tarfs_priv *)priv;
    struct tarfs_entry *e = tarfs_find(tp, path);
    if (!e || e->type != 1) return -1;

    uint32_t to_read = e->size;
    if (to_read > max_size) to_read = max_size;

    uint8_t *base = (uint8_t *)(uint64_t)tp->base_addr;
    memcpy(buf, base + e->offset, to_read);
    *out_size = to_read;
    return 0;
}

static int tarfs_stat(void *priv, const char *path, struct vfs_stat *st) {
    struct tarfs_priv *tp = (struct tarfs_priv *)priv;
    memset(st, 0, sizeof(*st));

    /* Handle root */
    const char *p = path;
    if (*p == '/') p++;
    if (*p == '\0') {
        st->size = 0;
        st->type = 2; /* dir */
        st->mode = 0555;
        return 0;
    }

    struct tarfs_entry *e = tarfs_find(tp, path);
    if (!e) return -1;

    st->size = e->size;
    st->type = e->type;
    st->mode = e->type == 2 ? 0555 : 0444;
    st->uid  = 0;
    st->gid  = 0;
    return 0;
}

static int tarfs_readdir(void *priv, const char *path, char names[][64], int max) {
    struct tarfs_priv *tp = (struct tarfs_priv *)priv;

    /* Normalize path — remove leading slash */
    const char *prefix = path;
    if (*prefix == '/') prefix++;
    size_t plen = strlen(prefix);
    if (plen > 0 && prefix[plen - 1] == '/') plen--;

    int count = 0;
    for (int i = 0; i < tp->num_entries && count < max; i++) {
        struct tarfs_entry *e = &tp->entries[i];
        const char *name = e->name;

        /* Check if this entry is a direct child of the directory */
        if (plen == 0) {
            /* Root: find top-level entries */
            const char *slash = strchr(name, '/');
            if (slash) {
                /* It's a subdirectory entry — extract top-level dir name */
                size_t dlen = (size_t)(slash - name);
                if (dlen > 63) dlen = 63;

                /* Check if already added */
                int found = 0;
                for (int j = 0; j < count; j++) {
                    if (strncmp(names[j], name, dlen) == 0 && names[j][dlen] == '\0') {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    memcpy(names[count], name, dlen);
                    names[count][dlen] = '\0';
                    count++;
                }
            } else {
                /* Top-level file */
                int nlen = (int)strlen(name);
                if (nlen > 63) nlen = 63;
                memcpy(names[count], name, nlen);
                names[count][nlen] = '\0';
                count++;
            }
        } else {
            /* Non-root: check prefix match */
            if (strncmp(name, prefix, plen) == 0 && name[plen] == '/') {
                const char *rest = name + plen + 1;
                const char *slash = strchr(rest, '/');
                if (slash) {
                    /* Subdirectory: extract first component */
                    size_t dlen = (size_t)(slash - rest);
                    if (dlen > 63) dlen = 63;
                    int found = 0;
                    for (int j = 0; j < count; j++) {
                        if (strncmp(names[j], rest, dlen) == 0 && names[j][dlen] == '\0') {
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        memcpy(names[count], rest, dlen);
                        names[count][dlen] = '\0';
                        count++;
                    }
                } else {
                    int nlen = (int)strlen(rest);
                    if (nlen > 63) nlen = 63;
                    memcpy(names[count], rest, nlen);
                    names[count][nlen] = '\0';
                    count++;
                }
            }
        }
    }
    return count;
}

static int tarfs_readdir_legacy(void *priv, const char *path) {
    char names[64][64];
    int n = tarfs_readdir(priv, path, names, 64);
    for (int i = 0; i < n; i++) {
        kprintf("  %s\n", names[i]);
    }
    return n;
}

static struct vfs_ops tarfs_ops = {
    .read    = tarfs_read,
    .stat    = tarfs_stat,
    .readdir_names = tarfs_readdir,
    .readdir = tarfs_readdir_legacy,
};

int tarfs_mount(const char *mountpoint, uint32_t addr, uint32_t size) {
    struct tarfs_priv *priv = (struct tarfs_priv *)kmalloc(sizeof(struct tarfs_priv));
    if (!priv) return -1;

    memset(priv, 0, sizeof(*priv));
    priv->base_addr  = addr;
    priv->total_size = size;

    int n = tarfs_parse(priv);
    if (n <= 0) {
        kfree(priv);
        return -1;
    }

    kprintf("[tarfs] %d entries at %s\n", n, mountpoint);
    return vfs_mount_ex(mountpoint, &tarfs_ops, priv, MS_RDONLY);
}

int tarfs_init(void) {
    kprintf("[tarfs] Read-only tar filesystem initialized\n");
    vfs_register_filesystem("tarfs", &tarfs_ops);
    return 0;
}

#ifdef MODULE
/* Module entry point — called by the module ELF loader on insmod */
int init_module(void) {
    return tarfs_init();
}

/* Module exit point — called by the module ELF loader on rmmod */
void cleanup_module(void) {
    /* No cleanup needed for read-only tarfs */
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Read-only tar archive filesystem — mounts in-memory tar archives via VFS");
MODULE_VERSION("1.0");
#endif /* MODULE */
