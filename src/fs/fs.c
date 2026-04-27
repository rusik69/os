#include "fs.h"
#include "ata.h"
#include "string.h"
#include "printf.h"

static struct fs_super super;
static struct fs_inode inodes[FS_MAX_FILES];

/* How many sectors the inode table takes */
#define INODE_SECTORS ((FS_MAX_FILES * sizeof(struct fs_inode) + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE)

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
    uint8_t buf[INODE_SECTORS * ATA_SECTOR_SIZE];
    for (uint32_t i = 0; i < INODE_SECTORS; i++) {
        if (ata_read_sectors(1 + i, 1, buf + i * ATA_SECTOR_SIZE) < 0) return -1;
    }
    memcpy(inodes, buf, sizeof(inodes));
    return 0;
}

static int save_inodes(void) {
    uint8_t buf[INODE_SECTORS * ATA_SECTOR_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, inodes, sizeof(inodes));
    for (uint32_t i = 0; i < INODE_SECTORS; i++) {
        if (ata_write_sectors(1 + i, 1, buf + i * ATA_SECTOR_SIZE) < 0) return -1;
    }
    return 0;
}

static uint32_t alloc_block(void) {
    uint32_t blk = super.next_free_block;
    super.next_free_block++;
    return blk;
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

    /* Count slashes to find depth */
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    if (!last_slash) return 0; /* directly in root */

    /* Extract parent dir name */
    size_t len = last_slash - path;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (inodes[i].type == FS_TYPE_DIR &&
            inodes[i].parent == 0 &&
            strlen(inodes[i].name) == len &&
            strncmp(inodes[i].name, path, len) == 0)
            return i;
    }
    return -1;
}

static int find_inode(const char *path) {
    if (*path == '/') path++;
    if (*path == '\0') return -1; /* root itself */

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
            strcmp(inodes[i].name, name) == 0)
            return i;
    }
    return -1;
}

int fs_format(void) {
    memset(&super, 0, sizeof(super));
    super.magic = FS_MAGIC;
    super.num_inodes = FS_MAX_FILES;
    super.num_data_blocks = 0;
    super.next_free_block = FS_DATA_START;

    memset(inodes, 0, sizeof(inodes));

    /* Create root directory as inode 0 */
    inodes[0].type = FS_TYPE_DIR;
    inodes[0].parent = 0;
    strncpy(inodes[0].name, "/", FS_MAX_NAME);

    if (save_super() < 0) return -1;
    if (save_inodes() < 0) return -1;
    return 0;
}

void fs_init(void) {
    if (!ata_is_present()) {
        kprintf("  No disk found, filesystem unavailable\n");
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
        kprintf("  Filesystem loaded (%u data blocks)\n",
                (uint64_t)(super.next_free_block - FS_DATA_START));
    }
}

int fs_create(const char *path, uint8_t type) {
    if (find_inode(path) >= 0) return -1; /* exists */

    int parent = find_parent(path);
    if (parent < 0) return -1;

    int idx = find_free_inode();
    if (idx < 0) return -1;

    const char *name = basename(path);
    memset(&inodes[idx], 0, sizeof(struct fs_inode));
    inodes[idx].type = type;
    inodes[idx].parent = (uint16_t)parent;
    strncpy(inodes[idx].name, name, FS_MAX_NAME - 1);

    save_inodes();
    return idx;
}

int fs_write_file(const char *path, const void *data, uint32_t size) {
    int idx = find_inode(path);
    if (idx < 0) {
        idx = fs_create(path, FS_TYPE_FILE);
        if (idx < 0) return -1;
    }
    if (inodes[idx].type != FS_TYPE_FILE) return -1;

    uint32_t blocks_needed = (size + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
    if (blocks_needed > FS_MAX_BLOCKS) return -1;

    const uint8_t *src = (const uint8_t *)data;
    for (uint32_t i = 0; i < blocks_needed; i++) {
        uint32_t blk = alloc_block();
        inodes[idx].blocks[i] = blk;

        uint8_t buf[ATA_SECTOR_SIZE];
        memset(buf, 0, ATA_SECTOR_SIZE);
        uint32_t chunk = size - i * ATA_SECTOR_SIZE;
        if (chunk > ATA_SECTOR_SIZE) chunk = ATA_SECTOR_SIZE;
        memcpy(buf, src + i * ATA_SECTOR_SIZE, chunk);
        if (ata_write_sectors(blk, 1, buf) < 0) return -1;
    }

    inodes[idx].size = size;
    save_inodes();
    save_super();
    return 0;
}

int fs_read_file(const char *path, void *buf, uint32_t max_size, uint32_t *out_size) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    if (inodes[idx].type != FS_TYPE_FILE) return -1;

    uint32_t size = inodes[idx].size;
    if (size > max_size) size = max_size;

    uint32_t blocks = (size + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
    uint8_t *dst = (uint8_t *)buf;

    for (uint32_t i = 0; i < blocks; i++) {
        uint8_t sector_buf[ATA_SECTOR_SIZE];
        if (ata_read_sectors(inodes[idx].blocks[i], 1, sector_buf) < 0) return -1;
        uint32_t chunk = size - i * ATA_SECTOR_SIZE;
        if (chunk > ATA_SECTOR_SIZE) chunk = ATA_SECTOR_SIZE;
        memcpy(dst + i * ATA_SECTOR_SIZE, sector_buf, chunk);
    }

    if (out_size) *out_size = size;
    return 0;
}

int fs_delete(const char *path) {
    int idx = find_inode(path);
    if (idx < 0) return -1;

    /* If dir, check it's empty */
    if (inodes[idx].type == FS_TYPE_DIR) {
        for (int i = 0; i < FS_MAX_FILES; i++) {
            if (inodes[i].type != FS_TYPE_FREE && inodes[i].parent == (uint16_t)idx && i != idx)
                return -1; /* not empty */
        }
    }

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

    int count = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (inodes[i].type != FS_TYPE_FREE && inodes[i].parent == (uint16_t)parent && i != parent) {
            const char *type_str = inodes[i].type == FS_TYPE_DIR ? "DIR " : "FILE";
            kprintf("  %s  %u\t%s\n", type_str, (uint64_t)inodes[i].size, inodes[i].name);
            count++;
        }
    }
    if (count == 0) kprintf("  (empty)\n");
    return 0;
}

int fs_stat(const char *path, uint32_t *size, uint8_t *type) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    if (size) *size = inodes[idx].size;
    if (type) *type = inodes[idx].type;
    return 0;
}

void fs_get_usage(uint32_t *used_inodes, uint32_t *total_inodes,
                  uint32_t *used_blocks, uint32_t *data_start) {
    uint32_t ui = 0;
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (inodes[i].type != FS_TYPE_FREE) ui++;
    if (used_inodes)  *used_inodes  = ui;
    if (total_inodes) *total_inodes = FS_MAX_FILES;
    if (used_blocks)  *used_blocks  = super.next_free_block - FS_DATA_START;
    if (data_start)   *data_start   = FS_DATA_START;
}
