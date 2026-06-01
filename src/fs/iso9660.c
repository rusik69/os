/*
 * src/fs/iso9660.c — ISO9660 (CDROM) read-only filesystem.
 *
 * Implements a minimal read-only ISO9660 filesystem.
 * Supports primary volume descriptor, directory records, and file reading.
 */

#define KERNEL_INTERNAL
#include "iso9660.h"
#include "blockdev.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"

struct iso9660_priv {
    uint8_t  dev_id;
    uint16_t block_size;       /* usually 2048 */
    uint32_t root_extent;      /* extent location of root dir */
    uint32_t root_size;        /* size of root dir */
};

/* Read a logical block from the ISO image */
static int iso_read_block(struct iso9660_priv *ip, uint32_t lba, uint8_t *buf) {
    uint64_t sector = (uint64_t)lba * (ip->block_size / 512);
    uint32_t sectors = ip->block_size / 512;
    for (uint32_t i = 0; i < sectors; i++) {
        if (blockdev_read(ip->dev_id, sector + i, 1, buf + i * 512) != 0)
            return -1;
    }
    return 0;
}

/* Find the primary volume descriptor */
static int iso9660_find_pvd(struct iso9660_priv *ip) {
    uint8_t buf[2048];
    /* PVD is at sector 16 */
    if (iso_read_block(ip, 16, buf) < 0) return -1;

    struct iso_primary_desc *pvd = (struct iso_primary_desc *)buf;
    if (pvd->type != 1) return -1;
    if (memcmp(pvd->id, "CD001", 5) != 0) return -1;

    ip->block_size = 2048; /* standard */

    /* Parse root directory record (34 bytes at offset 156 in PVD) */
    struct iso_dir_record *root = (struct iso_dir_record *)pvd->root_dir;
    ip->root_extent = root->extent_loc_le;
    ip->root_size   = root->data_length_le;

    kprintf("[iso9660] Root dir at LBA %u, size %u\n",
            ip->root_extent, ip->root_size);
    return 0;
}

/* Read all directory entries from a directory extent */
struct iso_dirent {
    char     name[256];
    uint32_t extent;
    uint32_t size;
    uint8_t  flags;
};

static int iso_read_dir_entries(struct iso9660_priv *ip, uint32_t extent, uint32_t size,
                                 struct iso_dirent *entries, int max) {
    uint8_t buf[2048];
    uint32_t offset = 0;
    int count = 0;

    while (offset < size && count < max) {
        uint32_t lba = extent + offset / ip->block_size;
        if (iso_read_block(ip, lba, buf) < 0) break;

        uint32_t pos = offset % ip->block_size;
        while (pos < ip->block_size && offset < size && count < max) {
            struct iso_dir_record *rec = (struct iso_dir_record *)(buf + pos);
            if (rec->length == 0) { pos++; offset++; continue; }
            if (rec->length < 34) break;

            struct iso_dirent *de = &entries[count];
            de->extent = rec->extent_loc_le;
            de->size   = rec->data_length_le;
            de->flags  = rec->flags;

            uint8_t nlen = rec->name_len;
            if (nlen > 0) {
                memcpy(de->name, rec->name, nlen);
                de->name[nlen] = '\0';
            } else {
                de->name[0] = '\0';
            }

            count++;
            pos += rec->length;
            offset += rec->length;
        }
    }

    return count;
}

/* Resolve path to extent */
static int iso9660_resolve(struct iso9660_priv *ip, const char *path,
                            uint32_t *extent, uint32_t *size) {
    *extent = ip->root_extent;
    *size   = ip->root_size;

    const char *p = path;
    if (*p == '/') p++;
    if (*p == '\0') return 0;

    struct iso_dirent entries[128];
    int n;

    n = iso_read_dir_entries(ip, *extent, *size, entries, 128);
    if (n <= 0) return -1;

    /* Skip leading / */
    while (*p == '/') p++;
    if (!*p) return 0;

    while (*p) {
        const char *end = p;
        while (*end && *end != '/') end++;
        size_t clen = (size_t)(end - p);

        int found = 0;
        for (int i = 0; i < n; i++) {
            size_t nlen = strlen(entries[i].name);
            /* Skip dot entries */
            if (nlen == 1 && entries[i].name[0] == 0) continue; /* system use */
            if (nlen == 1 && entries[i].name[0] == 0) continue;

            /* ISO names often have ;1 suffix for version */
            const char *ename = entries[i].name;
            size_t elen = nlen;
            if (elen > 2 && ename[elen - 2] == ';') elen -= 2;

            if (elen == clen && memcmp(ename, p, clen) == 0) {
                *extent = entries[i].extent;
                *size   = entries[i].size;
                found = 1;
                break;
            }

            /* Also try without version number */
            if (elen > clen && ename[clen] == ';' && memcmp(ename, p, clen) == 0) {
                *extent = entries[i].extent;
                *size   = entries[i].size;
                found = 1;
                break;
            }
        }
        if (!found) return -1;

        p = end;
        while (*p == '/') p++;
        if (!*p) break;

        /* Read next level */
        n = iso_read_dir_entries(ip, *extent, *size, entries, 128);
        if (n <= 0) return -1;
    }

    return 0;
}

/* VFS operations */

static int iso9660_read(void *priv, const char *path, void *buf,
                         uint32_t max_size, uint32_t *out_size) {
    struct iso9660_priv *ip = (struct iso9660_priv *)priv;
    uint32_t extent, size;
    if (iso9660_resolve(ip, path, &extent, &size) < 0) return -1;

    uint32_t to_read = size;
    if (to_read > max_size) to_read = max_size;

    uint32_t offset = 0;
    while (offset < to_read) {
        uint32_t lba = extent + offset / ip->block_size;
        uint8_t block[2048];
        if (iso_read_block(ip, lba, block) < 0) break;

        uint32_t chunk = to_read - offset;
        if (chunk > ip->block_size) chunk = ip->block_size;
        memcpy((uint8_t *)buf + offset, block, chunk);
        offset += chunk;
    }

    *out_size = offset;
    return 0;
}

static int iso9660_stat(void *priv, const char *path, struct vfs_stat *st) {
    struct iso9660_priv *ip = (struct iso9660_priv *)priv;
    uint32_t extent, size;
    if (iso9660_resolve(ip, path, &extent, &size) < 0) return -1;

    memset(st, 0, sizeof(*st));
    st->size = size;

    /* Determine if directory by checking root path */
    const char *p = path;
    if (*p == '/') p++;
    if (*p == '\0') {
        st->type = 2; /* dir */
        st->mode = 0555;
        return 0;
    }

    /* Read directory entry to check flags */
    struct iso_dirent entries[128];
    /* Find parent dir */
    const char *slash = NULL;
    for (const char *s = p; *s; s++) { if (*s == '/') slash = s; }

    uint32_t pextent = ip->root_extent;
    uint32_t psize   = ip->root_size;

    if (slash) {
        /* Resolve parent */
        char parent[256];
        size_t plen = (size_t)(slash - p);
        memcpy(parent, p, plen);
        parent[plen] = '\0';
        uint32_t dummy_ext, dummy_sz;
        if (iso9660_resolve(ip, parent, &dummy_ext, &dummy_sz) < 0) return -1;
        pextent = dummy_ext;
        psize = dummy_sz;
    }

    int n = iso_read_dir_entries(ip, pextent, psize, entries, 128);
    const char *name = slash ? slash + 1 : p;
    size_t nlen = strlen(name);

    for (int i = 0; i < n; i++) {
        size_t elen = strlen(entries[i].name);
        if (elen == nlen && memcmp(entries[i].name, name, nlen) == 0) {
            st->type = (entries[i].flags & ISO_FLAG_DIRECTORY) ? 2 : 1;
            st->mode = (entries[i].flags & ISO_FLAG_DIRECTORY) ? 0555 : 0444;
            return 0;
        }
    }

    st->type = 1;
    st->mode = 0444;
    return 0;
}

static int iso9660_readdir_entries(void *priv, const char *path, char names[][64], int max) {
    struct iso9660_priv *ip = (struct iso9660_priv *)priv;
    uint32_t extent, size;
    if (iso9660_resolve(ip, path, &extent, &size) < 0) return -1;

    struct iso_dirent entries[256];
    int n = iso_read_dir_entries(ip, extent, size, entries, max < 256 ? max : 256);
    int count = 0;

    for (int i = 0; i < n && count < max; i++) {
        size_t elen = strlen(entries[i].name);
        if (elen == 0) continue;
        if (elen == 1 && entries[i].name[0] == 0) continue;

        /* Strip version number */
        if (elen > 2 && entries[i].name[elen - 2] == ';')
            elen -= 2;

        if (elen > 63) elen = 63;
        memcpy(names[count], entries[i].name, elen);
        names[count][elen] = '\0';
        count++;
    }

    return count;
}

static int iso9660_readdir_legacy(void *priv, const char *path) {
    char names[64][64];
    int n = iso9660_readdir_entries(priv, path, names, 64);
    for (int i = 0; i < n; i++)
        kprintf("  %s\n", names[i]);
    return n;
}

static struct vfs_ops iso9660_ops = {
    .read    = iso9660_read,
    .stat    = iso9660_stat,
    .readdir_names = iso9660_readdir_entries,
    .readdir = iso9660_readdir_legacy,
};

int iso9660_mount(const char *mountpoint, uint8_t dev_id) {
    struct iso9660_priv *ip = (struct iso9660_priv *)kmalloc(sizeof(struct iso9660_priv));
    if (!ip) return -1;

    memset(ip, 0, sizeof(*ip));
    ip->dev_id = dev_id;
    ip->block_size = 2048;

    if (iso9660_find_pvd(ip) < 0) {
        kprintf("[iso9660] No primary volume descriptor found\n");
        kfree(ip);
        return -1;
    }

    kprintf("[iso9660] Mounted at %s (block_size=%u)\n", mountpoint, ip->block_size);
    return vfs_mount_ex(mountpoint, &iso9660_ops, ip, MS_RDONLY);
}

int iso9660_init(void) {
    kprintf("[iso9660] ISO9660 CDROM filesystem initialized\n");
    vfs_register_filesystem("iso9660", &iso9660_ops);
    return 0;
}
