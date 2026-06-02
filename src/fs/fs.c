#include "fs.h"
#include "ata.h"
#include "string.h"
#include "printf.h"
#include "users.h"
#include "timer.h"
#include "heap.h"
#include "fat32.h"
#include "page_cache.h"
#include "pmm.h"

static struct fs_super super;
static struct fs_inode inodes[FS_MAX_FILES];

/* ── Quota table ────────────────────────────────────────── */
#define FS_QUOTA_MAX_USERS 16
static struct {
    uint16_t uid;
    struct fs_quota quota;
    int in_use;
} fs_quota_table[FS_QUOTA_MAX_USERS];

/* ── Block reference counts for COW ─────────────────────── */
#define FS_MAX_BLOCKS_TOTAL (FS_MAX_FILES * FS_MAX_BLOCKS)
static uint8_t fs_block_refcount[FS_MAX_BLOCKS_TOTAL]; /* simple refcount per block index */

/* How many sectors the inode table takes */
#define INODE_SECTORS ((FS_MAX_FILES * sizeof(struct fs_inode) + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE)
#define FS_DATA_START   (1 + INODE_SECTORS)

static int load_super(void) {
    uint8_t buf[ATA_SECTOR_SIZE];
    if (ata_read_sectors(0, 1, buf) < 0) return -1;
    memcpy(&super, buf, sizeof(super));
    return 0;
}

static int save_super(void) {
    uint8_t buf[ATA_SECTOR_SIZE];
    memset(buf, 0, ATA_SECTOR_SIZE);
    memcpy(buf, &super, sizeof(super));
    return ata_write_sectors(0, 1, buf);
}

static int load_inodes(void) {
    uint8_t buf[ATA_SECTOR_SIZE];
    for (uint32_t i = 0; i < INODE_SECTORS; i++) {
        if (ata_read_sectors(1 + i, 1, buf) < 0) return -1;
        uint32_t offset = i * ATA_SECTOR_SIZE;
        uint32_t left   = (uint32_t)sizeof(inodes) - offset;
        uint32_t n      = left > ATA_SECTOR_SIZE ? ATA_SECTOR_SIZE : left;
        memcpy((uint8_t *)inodes + offset, buf, n);
    }
    return 0;
}

static int save_inodes(void) {
    uint8_t buf[ATA_SECTOR_SIZE];
    for (uint32_t i = 0; i < INODE_SECTORS; i++) {
        memset(buf, 0, ATA_SECTOR_SIZE);
        uint32_t offset = i * ATA_SECTOR_SIZE;
        uint32_t left   = (uint32_t)sizeof(inodes) - offset;
        uint32_t n      = left > ATA_SECTOR_SIZE ? ATA_SECTOR_SIZE : left;
        memcpy(buf, (uint8_t *)inodes + offset, n);
        if (ata_write_sectors(1 + i, 1, buf) < 0) return -1;
    }
    return 0;
}

#define FS_BITMAP_BYTES 496
#define FS_BITMAP_MAX_BLOCKS (FS_BITMAP_BYTES * 8)

static uint8_t *fs_bitmap(void) { return super.padding; }

static int bitmap_idx(uint32_t sector) {
    if (sector < FS_DATA_START) return -1;
    uint32_t idx = sector - FS_DATA_START;
    if (idx >= FS_BITMAP_MAX_BLOCKS) return -1;
    return (int)idx;
}

static int bitmap_is_free(int idx) {
    return !(fs_bitmap()[idx / 8] & (uint8_t)(1u << (idx % 8)));
}

static void bitmap_mark_used(int idx) {
    fs_bitmap()[idx / 8] |= (uint8_t)(1u << (idx % 8));
}

static void bitmap_mark_free(int idx) {
    fs_bitmap()[idx / 8] &= (uint8_t)~(1u << (idx % 8));
}

static void bitmap_init_all_free(void) {
    memset(fs_bitmap(), 0, FS_BITMAP_BYTES);
}

static void bitmap_rebuild_from_inodes(void) {
    bitmap_init_all_free();
    uint32_t hi = FS_DATA_START;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        for (uint32_t b = 0; b < FS_MAX_BLOCKS; b++) {
            uint32_t sec = inodes[i].blocks[b];
            if (!sec) continue;
            int idx = bitmap_idx(sec);
            if (idx >= 0) bitmap_mark_used(idx);
            if (sec >= hi) hi = sec + 1;
        }
    }
    if (hi > super.next_free_block) super.next_free_block = hi;
}

static uint32_t bitmap_alloc_block(void) {
    uint32_t start = super.next_free_block > FS_DATA_START
                   ? super.next_free_block - FS_DATA_START : 0;
    for (uint32_t pass = 0; pass < FS_BITMAP_MAX_BLOCKS; pass++) {
        uint32_t idx = (start + pass) % FS_BITMAP_MAX_BLOCKS;
        if (bitmap_is_free((int)idx)) {
            bitmap_mark_used((int)idx);
            uint32_t sec = FS_DATA_START + idx;
            if (sec >= super.next_free_block) super.next_free_block = sec + 1;
            return sec;
        }
    }
    return 0;
}

static void bitmap_free_sector(uint32_t sector) {
    int idx = bitmap_idx(sector);
    if (idx >= 0) bitmap_mark_free(idx);
}

static uint32_t bitmap_used_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < FS_BITMAP_MAX_BLOCKS; i++)
        if (!bitmap_is_free((int)i)) n++;
    return n;
}

static uint32_t alloc_block(void) {
    uint32_t blk = bitmap_alloc_block();
    if (!blk) {
        blk = super.next_free_block;
        int idx = bitmap_idx(blk);
        if (idx >= 0 && idx < (int)FS_BITMAP_MAX_BLOCKS) {
            bitmap_mark_used(idx);
            super.next_free_block++;
        }
    }
    return blk;
}

/* Zero and clear data blocks from index `from` onward */
static void fs_free_inode_blocks_from(int idx, uint32_t from) {
    uint8_t zero[ATA_SECTOR_SIZE];
    memset(zero, 0, ATA_SECTOR_SIZE);
    for (uint32_t b = from; b < FS_MAX_BLOCKS; b++) {
        if (inodes[idx].blocks[b] != 0) {
            bitmap_free_sector(inodes[idx].blocks[b]);
            ata_write_sectors(inodes[idx].blocks[b], 1, zero);
            inodes[idx].blocks[b] = 0;
        }
    }
    save_super();
}

static int find_free_inode(void) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (inodes[i].type == FS_TYPE_FREE) return i;
    }
    return -1;
}

/* Find the filename component from a path (skip leading /) */
static const char *basename(const char *path) {
    const char *last = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' && *(p + 1)) last = p + 1;
    }
    return last;
}

/* Find parent directory inode index for a path. Returns 0 for root-level files */
static int find_parent(const char *path) {
    /* Skip leading / */
    if (*path == '/') path++;

    /* Find the last slash — everything before it is the parent path */
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    /* File is directly in root */
    if (!last_slash) return 0;

    /* Walk each directory component from root to the direct parent.
     *   e.g. "var/log/httpd.log"  →  walk "var" then "log"
     */
    int parent = 0;  /* start at root (inode 0 parent field) */
    const char *seg = path;

    while (seg <= last_slash) {
        /* Find end of this segment */
        const char *end = seg;
        while (*end && *end != '/') end++;

        if (end == seg) { seg = end + 1; continue; } /* skip empty segs */

        size_t slen = (size_t)(end - seg);
        int found = -1;
        for (int i = 0; i < FS_MAX_FILES; i++) {
            if (inodes[i].type == FS_TYPE_DIR &&
                inodes[i].parent == (uint16_t)parent &&
                strlen(inodes[i].name) == slen &&
                strncmp(inodes[i].name, seg, slen) == 0) {
                found = i;
                break;
            }
        }
        if (found < 0) return -1; /* component not found */
        parent = found;

        seg = end + 1; /* move past the '/' */
        if (end >= last_slash) break; /* we've found the direct parent */
    }

    return parent;
}

static int find_inode_depth(const char *path, int depth);

static int find_inode(const char *path) {
    return find_inode_depth(path, 0);
}

#define SYMLINK_MAX_DEPTH 8

static int find_inode_depth(const char *path, int depth) {
    if (depth > SYMLINK_MAX_DEPTH) return -1;
    if (*path == '/') path++;
    if (*path == '\0') return 0; /* root itself = inode 0 */

    int parent = find_parent(path);
    if (parent < 0) return -1;

    /* For nested path, use basename; for root-level, path is the name */
    const char *name;
    /* Check if there's a slash */
    const char *slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') slash = p;
    }
    if (slash) name = slash + 1;
    else name = path;

    if (*name == '\0') return -1;

    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (inodes[i].type != FS_TYPE_FREE &&
            inodes[i].parent == (uint16_t)parent &&
            strcmp(inodes[i].name, name) == 0) {
            /* Follow symlinks (depth limit 8) */
            int cur = i;
            while (inodes[cur].type == FS_TYPE_LINK) {
                char link_buf[FS_BLOCK_SIZE];
                uint32_t tsz = 0;
                /* Read target from first block directly */
                if (inodes[cur].size == 0 || inodes[cur].blocks[0] == 0) return -1;
                if (ata_read_sectors(inodes[cur].blocks[0], 1,
                                     (uint8_t*)link_buf) < 0) return -1;
                uint32_t sz = inodes[cur].size;
                if (sz >= FS_BLOCK_SIZE) sz = FS_BLOCK_SIZE - 1;
                link_buf[sz] = '\0';
                tsz = sz;
                (void)tsz;
                int next = find_inode_depth(link_buf, depth + 1);
                if (next < 0) return -1;
                cur = next;
                depth++;
            }
            return cur;
        }
    }
    return -1;
}

static int check_dir_perm_idx(int idx, char op) {
    if (idx < 0 || idx >= FS_MAX_FILES) return -1;
    if (inodes[idx].type != FS_TYPE_DIR) return -1;

    struct user_session *s = session_get();
    uint16_t cur_uid = s ? (uint16_t)s->uid : 0;
    uint16_t cur_gid = s ? (uint16_t)s->gid : 0;
    uint16_t m       = inodes[idx].mode;

    if (cur_uid == 0) return 0; /* root */

    int shift;
    if (cur_uid == inodes[idx].uid)      shift = 6;
    else if (cur_gid == inodes[idx].gid) shift = 3;
    else                                 shift = 0;

    uint16_t bits;
    switch (op) {
        case 'r': bits = FS_PERM_ROTH << shift; break;
        case 'w': bits = FS_PERM_WOTH << shift; break;
        case 'x': bits = FS_PERM_XOTH << shift; break;
        default:  return -1;
    }
    return (m & bits) ? 0 : -1;
}

int fs_format(void) {
    memset(&super, 0, sizeof(super));
    super.magic = FS_MAGIC;
    super.num_inodes = FS_MAX_FILES;
    super.num_data_blocks = 0;
    super.next_free_block = 1 + INODE_SECTORS;

    memset(inodes, 0, sizeof(inodes));

    /* Create root directory as inode 0 */
    inodes[0].type   = FS_TYPE_DIR;
    inodes[0].parent = 0;
    inodes[0].uid    = 0;
    inodes[0].gid    = 0;
    inodes[0].mode   = FS_MODE_DIR;
    strncpy(inodes[0].name, "/", FS_MAX_NAME);

    /* Base home parent directory */
    inodes[1].type   = FS_TYPE_DIR;
    inodes[1].parent = 0;
    inodes[1].uid    = 0;
    inodes[1].gid    = 0;
    inodes[1].mode   = FS_MODE_DIR;
    strncpy(inodes[1].name, "home", FS_MAX_NAME - 1);

    /* Root home directory */
    inodes[2].type   = FS_TYPE_DIR;
    inodes[2].parent = 0;
    inodes[2].uid    = 0;
    inodes[2].gid    = 0;
    inodes[2].mode   = FS_MODE_DIR;
    strncpy(inodes[2].name, "root", FS_MAX_NAME - 1);

    /* Shared temp directory: rwxrwxrwt */
    inodes[3].type   = FS_TYPE_DIR;
    inodes[3].parent = 0;
    inodes[3].uid    = 0;
    inodes[3].gid    = 0;
    inodes[3].mode   = 01777;
    strncpy(inodes[3].name, "tmp", FS_MAX_NAME - 1);

    bitmap_init_all_free();

    if (save_super() < 0) return -1;
    if (save_inodes() < 0) return -1;
    return 0;
}

void fs_init(void) {
    if (!ata_is_present()) {
        kprintf("  No disk found, filesystem unavailable\n");
        return;
    }

    /* If FAT32 is already mounted, skip formatting — don't clobber it */
    if (fat32_is_mounted()) {
        kprintf("  FAT32 present, skipping native FS format\n");
        return;
    }

    if (load_super() < 0 || super.magic != FS_MAGIC) {
        kprintf("  No filesystem found, formatting...\n");
        if (fs_format() < 0) {
            kprintf("  Format failed!\n");
            return;
        }
        kprintf("  Filesystem formatted\n");
    } else {
        if (load_inodes() < 0) {
            kprintf("  Failed to load inodes\n");
            return;
        }
        bitmap_rebuild_from_inodes();
        kprintf("  Filesystem loaded (%lu data blocks used)\n",
                (unsigned long)bitmap_used_count());
    }
}

int fs_create(const char *path, uint8_t type) {
    if (find_inode(path) >= 0) return -1; /* exists */

    int parent = find_parent(path);
    if (parent < 0) return -1;
    if (check_dir_perm_idx(parent, 'w') < 0 || check_dir_perm_idx(parent, 'x') < 0) return -3;

    int idx = find_free_inode();
    if (idx < 0) return -1;

    struct user_session *s = session_get();
    uint16_t cur_uid = s ? (uint16_t)s->uid : 0;
    uint16_t cur_gid = s ? (uint16_t)s->gid : 0;

    const char *name = basename(path);
    memset(&inodes[idx], 0, sizeof(struct fs_inode));
    inodes[idx].type   = type;
    inodes[idx].parent = (uint16_t)parent;
    inodes[idx].uid    = cur_uid;
    inodes[idx].gid    = cur_gid;
    inodes[idx].mode   = (type == FS_TYPE_DIR) ? FS_MODE_DIR : FS_MODE_FILE;
    inodes[idx].mtime  = (uint32_t)(timer_get_ticks() / TIMER_FREQ);
    strncpy(inodes[idx].name, name, FS_MAX_NAME - 1);

    save_inodes();
    return idx;
}

int fs_write_file(const char *path, const void *data, uint32_t size) {
    int idx = find_inode(path);
    if (idx >= 0 && fs_check_perm(path, 'w') < 0) return -3;
    if (idx < 0) {
        idx = fs_create(path, FS_TYPE_FILE);
        if (idx < 0) return idx;
    }
    if (inodes[idx].type != FS_TYPE_FILE) return -1;

    uint32_t blocks_needed = (size + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
    if (blocks_needed > FS_MAX_BLOCKS) return -1;

    /* Save old blocks so we can free them after writing new data */
    uint32_t old_blocks[FS_MAX_BLOCKS];
    memcpy(old_blocks, inodes[idx].blocks, sizeof(old_blocks));

    /* Allocate new blocks and write data first (atomicity: don't free old until
     * new data is on disk) */
    uint32_t new_blocks[FS_MAX_BLOCKS];
    memset(new_blocks, 0, sizeof(new_blocks));
    const uint8_t *src = (const uint8_t *)data;
    for (uint32_t i = 0; i < blocks_needed; i++) {
        uint32_t blk = alloc_block();
        if (blk == 0) {
            for (uint32_t j = 0; j < i; j++) {
                if (new_blocks[j]) bitmap_free_sector(new_blocks[j]);
            }
            return -1;
        }
        new_blocks[i] = blk;

        uint8_t buf[ATA_SECTOR_SIZE];
        memset(buf, 0, ATA_SECTOR_SIZE);
        uint32_t chunk = size - i * ATA_SECTOR_SIZE;
        if (chunk > ATA_SECTOR_SIZE) chunk = ATA_SECTOR_SIZE;
        memcpy(buf, src + i * ATA_SECTOR_SIZE, chunk);
        if (ata_write_sectors(blk, 1, buf) < 0) {
            for (uint32_t j = 0; j <= i; j++) {
                if (new_blocks[j]) bitmap_free_sector(new_blocks[j]);
            }
            return -1;
        }
    }

    /* Assign new blocks to inode FIRST, then free old blocks.
     * This ensures crash-consistency: if we crash right after updating the
     * inode, the old blocks are still in the bitmap (allocated, not reusable)
     * and the new blocks are referenced by the inode.  If we crashed with
     * the reverse order, freed old blocks in the bitmap could be re-allocated
     * while the inode still references them. */
    for (uint32_t i = 0; i < blocks_needed; i++)
        inodes[idx].blocks[i] = new_blocks[i];
    for (uint32_t i = blocks_needed; i < FS_MAX_BLOCKS; i++)
        inodes[idx].blocks[i] = 0;
    inodes[idx].size  = size;
    inodes[idx].mtime = (uint32_t)(timer_get_ticks() / TIMER_FREQ);

    /* Free all old blocks after inode is updated */
    {
        uint8_t zs[ATA_SECTOR_SIZE];
        memset(zs, 0, sizeof(zs));
        for (uint32_t i = 0; i < FS_MAX_BLOCKS; i++) {
            if (old_blocks[i] != 0) {
                bitmap_free_sector(old_blocks[i]);
                (void)ata_write_sectors(old_blocks[i], 1, zs);
                old_blocks[i] = 0;
            }
        }
    }

    save_inodes();
    save_super();
    return 0;
}

int fs_append(const char *path, const void *data, uint32_t len) {
    if (len == 0) return 0;

    /* Read existing content */
    uint32_t existing = 0;
    int idx = find_inode(path);
    if (idx >= 0) {
        if (inodes[idx].type != FS_TYPE_FILE) return -1;
        if (fs_check_perm(path, 'w') < 0) return -3;
        existing = inodes[idx].size;
    }

    uint32_t total = existing + len;
    uint32_t max_total = FS_MAX_BLOCKS * FS_BLOCK_SIZE;
    if (total > max_total) total = max_total;

    uint8_t *tmp = (uint8_t *)kmalloc(total);
    if (!tmp) return -1;

    uint32_t copy = total - existing;
    if (existing > 0 && fs_read_file(path, tmp, existing, &existing) < 0) {
        kfree(tmp);
        return -1;
    }
    memcpy(tmp + existing, data, copy);

    int ret = fs_write_file(path, tmp, total);
    kfree(tmp);
    return ret;
}

/*
 * ── Backing store callback for page_cache_read ──────────────────────
 *
 * Translates a (block_index, count, buf) request into ATA sector reads.
 * Block index here is assumed to be a filesystem data block number
 * (i.e. an ATA LBA).  Returns 0 on success, <0 on error.
 */
static int fs_backing_store_read(uint32_t lba, uint8_t count, void *buf)
{
    return ata_read_sectors(lba, count, buf);
}

/* Backing store write callback for page cache dirty writeback */
static int fs_backing_store_write(uint32_t lba, uint8_t count, const void *buf)
{
    for (uint8_t i = 0; i < count; i++) {
        if (ata_write_sectors(lba + i, 1, (const uint8_t *)buf + (uint32_t)i * 512) < 0)
            return -1;
    }
    return 0;
}

/* Register the backing store write callback with the page cache.
 * Called from kernel.c after page_cache_init(). */
void fs_register_page_cache_writeback(void)
{
    page_cache_set_writeback(fs_backing_store_write);
    kprintf("[fs] page cache writeback registered\n");
}


/*
 * Read file contents using the page cache with automatic readahead.
 *
 * This replaces the naive per-sector ATA read with a page-cache-aware
 * path that reads in PAGE_SIZE (4096-byte) chunks and triggers
 * sequential prefetching.
 */
int fs_read_file(const char *path, void *buf, uint32_t max_size, uint32_t *out_size) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    if (inodes[idx].type != FS_TYPE_FILE) return -1;
    if (fs_check_perm(path, 'r') < 0) return -3; /* permission denied */

    uint32_t size = inodes[idx].size;
    if (size > max_size) size = max_size;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t file_blocks = (size + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;

    /* ── Compute inode number for page cache key ────────────────── */
    uint64_t ino = (uint64_t)idx + 1;  /* 1-based inode number */

    for (uint32_t i = 0; i < file_blocks; ) {
        /* Map the ATA sector index to the physical block number */
        uint32_t blk = inodes[idx].blocks[i];

        if (blk == 0) {
            /* Unallocated block — zero-fill */
            uint8_t sector_buf[ATA_SECTOR_SIZE];
            memset(sector_buf, 0, ATA_SECTOR_SIZE);
            uint32_t chunk = size - i * ATA_SECTOR_SIZE;
            if (chunk > ATA_SECTOR_SIZE) chunk = ATA_SECTOR_SIZE;
            memcpy(dst + i * ATA_SECTOR_SIZE, sector_buf, chunk);
            i++;
            continue;
        }

        /* ── Read via page cache ───────────────────────────────── */
        /* The page cache works at PAGE_SIZE granularity.
         * Determine which page cache block this sector falls in. */
        uint64_t pc_block = (uint64_t)blk / (PAGE_SIZE / ATA_SECTOR_SIZE);
        uint32_t sector_offset = blk % (PAGE_SIZE / ATA_SECTOR_SIZE);

        /* Read the full page from cache (with readahead) */
        uint8_t page_buf[PAGE_SIZE];

        int ret = page_cache_read(ino, pc_block, page_buf, fs_backing_store_read);
        if (ret < 0) {
            /* Fallback: direct ATA read without caching */
            uint8_t sector_buf[ATA_SECTOR_SIZE];
            if (ata_read_sectors(blk, 1, sector_buf) < 0) return -1;
            uint32_t chunk = size - i * ATA_SECTOR_SIZE;
            if (chunk > ATA_SECTOR_SIZE) chunk = ATA_SECTOR_SIZE;
            memcpy(dst + i * ATA_SECTOR_SIZE, sector_buf, chunk);
            i++;
            continue;
        }

        /* Copy the relevant sector(s) from the cached page */
        uint32_t sectors_avail = (PAGE_SIZE / ATA_SECTOR_SIZE) - sector_offset;
        uint32_t sectors_needed = file_blocks - i;
        if (sectors_needed > sectors_avail)
            sectors_needed = sectors_avail;

        for (uint32_t s = 0; s < sectors_needed; s++) {
            uint32_t chunk = size - (i + s) * ATA_SECTOR_SIZE;
            if (chunk > ATA_SECTOR_SIZE) chunk = ATA_SECTOR_SIZE;
            memcpy(dst + (i + s) * ATA_SECTOR_SIZE,
                   page_buf + (sector_offset + s) * ATA_SECTOR_SIZE,
                   chunk);
        }

        i += sectors_needed;
    }

    if (out_size) *out_size = size;
    return 0;
}

/*
 * Readahead: prefetch file data for the given byte range into the page cache.
 *
 * Computes the page cache blocks covering [offset, offset+count) and
 * prefetches them from the ATA backing store.  Subsequent reads of this
 * data will be cache hits.
 *
 * Returns 0 on success, or negative on error.
 */
int fs_readahead(const char *path, uint32_t offset, uint32_t count) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    if (inodes[idx].type != FS_TYPE_FILE) return -1;

    uint32_t file_size = inodes[idx].size;
    if (offset >= file_size) return 0;
    if (offset + count > file_size)
        count = file_size - offset;

    uint64_t ino = (uint64_t)idx + 1;  /* 1-based inode number */

    /* Compute sector range */
    uint32_t start_sector = offset / ATA_SECTOR_SIZE;
    uint32_t end_sector   = (offset + count + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
    uint32_t sectors_per_page = PAGE_SIZE / ATA_SECTOR_SIZE; /* typically 8 */

    /* Track the last page cache block we prefetched to avoid duplicates */
    uint64_t last_pc_block = UINT64_MAX;

    for (uint32_t s = start_sector; s < end_sector; ) {
        uint32_t blk = inodes[idx].blocks[s];
        if (blk == 0) {
            s++;
            continue; /* sparse/unallocated — skip */
        }

        uint64_t pc_block = (uint64_t)blk / sectors_per_page;

        if (pc_block != last_pc_block) {
            /* Count how many consecutive file sectors map to the same
             * page cache block (or consecutive page cache blocks).
             * We batch them into a single readahead call. */
            uint32_t run_sectors = 1;
            for (uint32_t ns = s + 1; ns < end_sector; ns++, run_sectors++) {
                uint32_t nblk = inodes[idx].blocks[ns];
                if (nblk == 0) break;
                uint64_t npc = (uint64_t)nblk / sectors_per_page;
                if (npc != pc_block + (run_sectors / sectors_per_page))
                    break;
            }

            /* Convert run_sectors to page cache blocks */
            uint32_t pc_run = (run_sectors + sectors_per_page - 1) / sectors_per_page;

            /* Prefetch this run */
            page_cache_readahead(ino, pc_block, (int)pc_run, fs_backing_store_read);

            last_pc_block = pc_block;
        }

        s++;
    }

    return 0;
}

int fs_delete(const char *path) {
    int idx = find_inode(path);
    if (idx < 0) return -1;

    int parent = inodes[idx].parent;
    if (check_dir_perm_idx(parent, 'w') < 0 || check_dir_perm_idx(parent, 'x') < 0) return -3;

    /* Linux sticky-dir semantics: in sticky directories, only root,
     * directory owner, or entry owner may unlink/remove entries. */
    {
        struct user_session *s = session_get();
        uint16_t cur_uid = s ? (uint16_t)s->uid : 0;
        if (cur_uid != 0 && (inodes[parent].mode & FS_PERM_STICKY)) {
            if (cur_uid != inodes[parent].uid && cur_uid != inodes[idx].uid)
                return -3;
        }
    }

    /* If dir, check it's empty */
    if (inodes[idx].type == FS_TYPE_DIR) {
        for (int i = 0; i < FS_MAX_FILES; i++) {
            if (inodes[i].type != FS_TYPE_FREE && inodes[i].parent == (uint16_t)idx && i != idx)
                return -1; /* not empty */
        }
    }

    fs_free_inode_blocks_from(idx, 0);
    memset(&inodes[idx], 0, sizeof(struct fs_inode));
    save_inodes();
    return 0;
}

int fs_list(const char *path) {
    int parent;
    if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        parent = 0; /* root */
    } else {
        parent = find_inode(path);
        if (parent < 0) return -1;
        if (inodes[parent].type != FS_TYPE_DIR) return -1;
    }

    if (check_dir_perm_idx(parent, 'r') < 0 || check_dir_perm_idx(parent, 'x') < 0) return -3;

    int count = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (inodes[i].type != FS_TYPE_FREE &&
            inodes[i].parent == (uint16_t)parent && i != parent) {
            char mstr[10];
            fs_mode_str(inodes[i].mode, mstr);
            char type_char = inodes[i].type == FS_TYPE_DIR ? 'd' : '-';
            kprintf("%c%s %lu:%lu\t%lu\t%s\n",
                    type_char, mstr,
                    (unsigned long)inodes[i].uid, (unsigned long)inodes[i].gid,
                    (unsigned long)inodes[i].size,
                    inodes[i].name);
            count++;
        }
    }
    if (count == 0) kprintf("  (empty)\n");
    return 0;
}

int fs_stat(const char *path, uint32_t *size, uint8_t *type) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    if (check_dir_perm_idx((int)inodes[idx].parent, 'x') < 0) return -3;
    if (size) *size = inodes[idx].size;
    if (type) *type = inodes[idx].type;
    return 0;
}

int fs_stat_ex(const char *path, uint32_t *size, uint8_t *type,
               uint16_t *uid, uint16_t *gid, uint16_t *mode) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    if (check_dir_perm_idx((int)inodes[idx].parent, 'x') < 0) return -3;
    if (size) *size = inodes[idx].size;
    if (type) *type = inodes[idx].type;
    if (uid)  *uid  = inodes[idx].uid;
    if (gid)  *gid  = inodes[idx].gid;
    if (mode) *mode = inodes[idx].mode;
    return 0;
}

int fs_stat_mtime(const char *path) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    return (int)inodes[idx].mtime;
}

int fs_set_mtime(const char *path, uint32_t mtime) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    inodes[idx].mtime = mtime;
    save_inodes();
    return 0;
}

int fs_chmod(const char *path, uint16_t mode) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    if (check_dir_perm_idx((int)inodes[idx].parent, 'x') < 0) return -3;

    struct user_session *s = session_get();
    uint16_t cur_uid = s ? (uint16_t)s->uid : 0;
    /* Only owner or root may chmod */
    if (cur_uid != 0 && cur_uid != inodes[idx].uid) return -2;

    inodes[idx].mode = mode & 07777;
    save_inodes();
    return 0;
}

int fs_chown(const char *path, uint16_t uid, uint16_t gid) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    if (check_dir_perm_idx((int)inodes[idx].parent, 'x') < 0) return -3;

    struct user_session *s = session_get();
    uint16_t cur_uid = s ? (uint16_t)s->uid : 0;
    /* Only root may chown */
    if (cur_uid != 0) return -2;

    inodes[idx].uid = uid;
    inodes[idx].gid = gid;
    save_inodes();
    return 0;
}

/* Check if current session has permission op ('r','w','x') on path.
 * Returns 0 = allowed, -1 = not found, -2 = denied. */
int fs_check_perm(const char *path, char op) {
    int idx = find_inode(path);
    if (idx < 0) return -1;

    /* Linux-like path resolution requires search permission on parent dirs. */
    if (check_dir_perm_idx((int)inodes[idx].parent, 'x') < 0) return -2;

    struct user_session *s = session_get();
    uint16_t cur_uid = s ? (uint16_t)s->uid : 0;
    uint16_t cur_gid = s ? (uint16_t)s->gid : 0;
    uint16_t m       = inodes[idx].mode;

    /* Root bypasses permission checks */
    if (cur_uid == 0) return 0;

    int shift;
    if (cur_uid == inodes[idx].uid) {
        shift = 6; /* owner */
    } else if (cur_gid == inodes[idx].gid || user_in_group(cur_uid, inodes[idx].gid)) {
        /* Check primary gid and supplementary groups */
        shift = 3; /* group */
    } else {
        shift = 0; /* other */
    }

    uint16_t bits;
    switch (op) {
        case 'r': bits = FS_PERM_ROTH << shift; break;
        case 'w': bits = FS_PERM_WOTH << shift; break;
        case 'x': bits = FS_PERM_XOTH << shift; break;
        default:  return -2;
    }
    return (m & bits) ? 0 : -2;
}

/* Format mode as "rwxrwxrwx" — 9 chars + NUL */
void fs_mode_str(uint16_t mode, char out[10]) {
    const char *bits = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) {
        int bit = 1 << (8 - i);
        out[i] = (mode & bit) ? bits[i] : '-';
    }
    if (mode & FS_PERM_STICKY) {
        out[8] = (out[8] == 'x') ? 't' : 'T';
    }
    out[9] = '\0';
}

void fs_get_usage(uint32_t *used_inodes, uint32_t *total_inodes,
                  uint32_t *used_blocks, uint32_t *data_start) {
    uint32_t ui = 0;
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (inodes[i].type != FS_TYPE_FREE) ui++;
    if (used_inodes)  *used_inodes  = ui;
    if (total_inodes) *total_inodes = FS_MAX_FILES;
    if (used_blocks)  *used_blocks  = bitmap_used_count();
    if (data_start)   *data_start   = FS_DATA_START;
}

int fs_list_names(const char *dir, const char *prefix,
                  char names[][FS_MAX_NAME], int max) {
    int parent;
    if (!dir || dir[0] == '\0' || (dir[0] == '/' && dir[1] == '\0'))
        parent = 0;
    else {
        parent = find_inode(dir);
        if (parent < 0) return 0;
        if (inodes[parent].type != FS_TYPE_DIR) return 0;
    }
    if (check_dir_perm_idx(parent, 'r') < 0 || check_dir_perm_idx(parent, 'x') < 0) return 0;
    int plen = prefix ? (int)strlen(prefix) : 0;
    int n = 0;
    for (int i = 0; i < FS_MAX_FILES && n < max; i++) {
        if (inodes[i].type == FS_TYPE_FREE) continue;
        if (inodes[i].parent != (uint16_t)parent || i == parent) continue;
        if (plen > 0 && strncmp(inodes[i].name, prefix, plen) != 0) continue;
        strncpy(names[n], inodes[i].name, FS_MAX_NAME - 1);
        names[n][FS_MAX_NAME - 1] = '\0';
        n++;
    }
    return n;
}

/* ── Symbolic link support ──────────────────────────────────────────────── */

/* Like find_inode but does NOT follow symlinks (for readlink/lstat) */
static int find_inode_nofollow(const char *path) {
    if (*path == '/') path++;
    if (*path == '\0') return -1;
    int parent = find_parent(path);
    if (parent < 0) return -1;
    const char *name;
    const char *slash = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') slash = p;
    name = slash ? slash + 1 : path;
    if (*name == '\0') return -1;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (inodes[i].type != FS_TYPE_FREE &&
            inodes[i].parent == (uint16_t)parent &&
            strcmp(inodes[i].name, name) == 0)
            return i;
    }
    return -1;
}

int fs_symlink(const char *path, const char *target) {
    if (!path || !target) return -1;
    int parent = find_parent(path);
    if (parent < 0) return -1;
    if (check_dir_perm_idx(parent, 'w') < 0 || check_dir_perm_idx(parent, 'x') < 0) return -3;
    int idx = find_free_inode();
    if (idx < 0) return -1;
    struct user_session *s = session_get();
    uint16_t cur_uid = s ? (uint16_t)s->uid : 0;
    uint16_t cur_gid = s ? (uint16_t)s->gid : 0;
    const char *name = basename(path);
    memset(&inodes[idx], 0, sizeof(struct fs_inode));
    inodes[idx].type   = FS_TYPE_LINK;
    inodes[idx].parent = (uint16_t)parent;
    inodes[idx].uid    = cur_uid;
    inodes[idx].gid    = cur_gid;
    inodes[idx].mode   = 0777;
    strncpy(inodes[idx].name, name, FS_MAX_NAME - 1);
    save_inodes();
    /* Store target in first data block */
    uint32_t tlen = (uint32_t)strlen(target);
    if (tlen >= FS_BLOCK_SIZE) tlen = FS_BLOCK_SIZE - 1;
    uint32_t blk = alloc_block();
    uint8_t blk_buf[FS_BLOCK_SIZE];
    memset(blk_buf, 0, FS_BLOCK_SIZE);
    memcpy(blk_buf, target, tlen);
    ata_write_sectors(blk, 1, blk_buf);
    inodes[idx].blocks[0] = blk;
    inodes[idx].size = tlen;
    save_inodes();
    save_super();
    return 0;
}

int fs_readlink(const char *path, char *buf, int bufsize) {
    int idx = find_inode_nofollow(path);
    if (idx < 0) return -1;
    if (inodes[idx].type != FS_TYPE_LINK) return -1;
    if (inodes[idx].size == 0 || inodes[idx].blocks[0] == 0) { buf[0]='\0'; return 0; }
    uint8_t blk_buf[FS_BLOCK_SIZE];
    if (ata_read_sectors(inodes[idx].blocks[0], 1, blk_buf) < 0) return -1;
    uint32_t sz = inodes[idx].size;
    if (sz >= (uint32_t)bufsize) sz = (uint32_t)(bufsize - 1);
    memcpy(buf, blk_buf, sz);
    buf[sz] = '\0';
    return (int)sz;
}

int fs_lstat(const char *path, uint32_t *size, uint8_t *type) {
    int idx = find_inode_nofollow(path);
    if (idx < 0) return -1;
    if (size) *size = inodes[idx].size;
    if (type) *type = inodes[idx].type;
    return 0;
}

int fs_truncate(const char *path, uint32_t len) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    if (inodes[idx].type != FS_TYPE_FILE) return -1;
    uint32_t old_size = inodes[idx].size;
    if (len == old_size) return 0;
    if (len > old_size) {
        /* Extending: allocate new blocks and zero-fill */
        uint32_t old_blocks = (old_size + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;
        uint32_t new_blocks = (len + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;
        int free_start = -1;
        for (uint32_t b = old_blocks; b < new_blocks; b++) {
            uint32_t blk = alloc_block();
            if (!blk) {
                /* Allocation failed — free any blocks we allocated */
                if (free_start >= 0) fs_free_inode_blocks_from(idx, free_start);
                return -1;
            }
            inodes[idx].blocks[b] = blk;
            if (free_start < 0) free_start = (int)b;
        }
        inodes[idx].size = len;
        return save_inodes();
    }
    /* Shrinking: free blocks beyond len */
    uint32_t new_blocks = (len + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;
    fs_free_inode_blocks_from(idx, new_blocks);
    inodes[idx].size = len;
    return save_inodes();
}

/* ── Quota implementation ──────────────────────────────────── */

int fs_set_quota(uint16_t uid, uint32_t block_limit, uint32_t inode_limit) {
    int slot = -1;
    for (int i = 0; i < FS_QUOTA_MAX_USERS; i++) {
        if (fs_quota_table[i].in_use && fs_quota_table[i].uid == uid) {
            slot = i; break;
        }
        if (!fs_quota_table[i].in_use && slot < 0) slot = i;
    }
    if (slot < 0) return -1;

    /* Recalculate current usage */
    uint32_t blocks_used = 0, inodes_used = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (inodes[i].type != FS_TYPE_FREE) {
            inodes_used++;
            for (uint32_t b = 0; b < FS_MAX_BLOCKS; b++) {
                if (inodes[i].blocks[b]) blocks_used++;
            }
        }
    }

    fs_quota_table[slot].uid = uid;
    fs_quota_table[slot].quota.block_hard_limit = block_limit;
    fs_quota_table[slot].quota.inode_hard_limit = inode_limit;
    fs_quota_table[slot].quota.block_soft_limit = block_limit > 0 ? (uint32_t)((uint64_t)block_limit * 80 / 100) : 0;
    fs_quota_table[slot].quota.inode_soft_limit = inode_limit > 0 ? (uint32_t)((uint64_t)inode_limit * 80 / 100) : 0;
    fs_quota_table[slot].quota.cur_blocks = blocks_used;
    fs_quota_table[slot].quota.cur_inodes = inodes_used;
    fs_quota_table[slot].quota.uid = uid;
    fs_quota_table[slot].quota.block_grace = 0;
    fs_quota_table[slot].quota.inode_grace = 0;
    fs_quota_table[slot].in_use = 1;
    return 0;
}

int fs_get_quota(uint16_t uid, struct fs_quota *quota) {
    if (!quota) return -1;
    for (int i = 0; i < FS_QUOTA_MAX_USERS; i++) {
        if (fs_quota_table[i].in_use && fs_quota_table[i].uid == uid) {
            *quota = fs_quota_table[i].quota;
            return 0;
        }
    }
    return -1;
}

int fs_check_quota_blocks(uint16_t uid, uint32_t blocks_needed) {
    for (int i = 0; i < FS_QUOTA_MAX_USERS; i++) {
        if (fs_quota_table[i].in_use && fs_quota_table[i].uid == uid) {
            struct fs_quota *q = &fs_quota_table[i].quota;
            if (q->block_hard_limit > 0 && q->cur_blocks + blocks_needed > q->block_hard_limit)
                return -1;
            return 0;
        }
    }
    return 0; /* no quota set for this uid */
}

int fs_check_quota_inodes(uint16_t uid) {
    for (int i = 0; i < FS_QUOTA_MAX_USERS; i++) {
        if (fs_quota_table[i].in_use && fs_quota_table[i].uid == uid) {
            struct fs_quota *q = &fs_quota_table[i].quota;
            if (q->inode_hard_limit > 0 && q->cur_inodes + 1 > q->inode_hard_limit)
                return -1;
            return 0;
        }
    }
    return 0;
}

/* ── Copy-on-Write for data blocks ────────────────────────── */

uint32_t fs_cow_block(uint32_t block) {
    if (block == 0) return 0;
    /* Calculate block index (0-based from data start) */
    int bmap_idx = (int)(block - FS_DATA_START);
    if (bmap_idx < 0 || bmap_idx >= FS_MAX_BLOCKS_TOTAL) return 0;

    /* Check refcount: if only 1 reference, no COW needed */
    if (fs_block_refcount[bmap_idx] <= 1) return block;

    /* Allocate a new block */
    uint32_t new_block = alloc_block();
    if (new_block == 0) return 0;

    /* Copy data from old block to new block */
    uint8_t buf[ATA_SECTOR_SIZE];
    if (ata_read_sectors(block, 1, buf) < 0) {
        bitmap_free_sector(new_block);
        return 0;
    }
    if (ata_write_sectors(new_block, 1, buf) < 0) {
        bitmap_free_sector(new_block);
        return 0;
    }

    /* Decrement old refcount */
    if (fs_block_refcount[bmap_idx] > 0)
        fs_block_refcount[bmap_idx]--;

    /* Set new refcount to 1 */
    int new_bmap_idx = (int)(new_block - FS_DATA_START);
    if (new_bmap_idx >= 0 && new_bmap_idx < FS_MAX_BLOCKS_TOTAL)
        fs_block_refcount[new_bmap_idx] = 1;

    return new_block;
}
