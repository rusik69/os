#include "fs.h"
#include "ata.h"
#include "string.h"
#include "printf.h"
#include "users.h"

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
    super.next_free_block = FS_DATA_START;

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
    strncpy(inodes[idx].name, name, FS_MAX_NAME - 1);

    save_inodes();
    return idx;
}

int fs_write_file(const char *path, const void *data, uint32_t size) {
    int idx = find_inode(path);
    if (idx >= 0 && fs_check_perm(path, 'w') < 0) return -3; /* perm denied on existing file */
    if (idx < 0) {
        idx = fs_create(path, FS_TYPE_FILE);
        if (idx < 0) return idx;
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
    if (fs_check_perm(path, 'r') < 0) return -3; /* permission denied */

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

    int parent = inodes[idx].parent;
    if (check_dir_perm_idx(parent, 'w') < 0 || check_dir_perm_idx(parent, 'x') < 0) return -3;

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

    if (check_dir_perm_idx(parent, 'r') < 0 || check_dir_perm_idx(parent, 'x') < 0) return -3;

    int count = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (inodes[i].type != FS_TYPE_FREE &&
            inodes[i].parent == (uint16_t)parent && i != parent) {
            char mstr[10];
            fs_mode_str(inodes[i].mode, mstr);
            char type_char = inodes[i].type == FS_TYPE_DIR ? 'd' : '-';
            kprintf("%c%s %u:%u\t%u\t%s\n",
                    type_char, mstr,
                    (uint64_t)inodes[i].uid, (uint64_t)inodes[i].gid,
                    (uint64_t)inodes[i].size,
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
    if (size) *size = inodes[idx].size;
    if (type) *type = inodes[idx].type;
    return 0;
}

int fs_stat_ex(const char *path, uint32_t *size, uint8_t *type,
               uint16_t *uid, uint16_t *gid, uint16_t *mode) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    if (size) *size = inodes[idx].size;
    if (type) *type = inodes[idx].type;
    if (uid)  *uid  = inodes[idx].uid;
    if (gid)  *gid  = inodes[idx].gid;
    if (mode) *mode = inodes[idx].mode;
    return 0;
}

int fs_chmod(const char *path, uint16_t mode) {
    int idx = find_inode(path);
    if (idx < 0) return -1;

    struct user_session *s = session_get();
    uint16_t cur_uid = s ? (uint16_t)s->uid : 0;
    /* Only owner or root may chmod */
    if (cur_uid != 0 && cur_uid != inodes[idx].uid) return -2;

    inodes[idx].mode = mode & 0777;
    save_inodes();
    return 0;
}

int fs_chown(const char *path, uint16_t uid, uint16_t gid) {
    int idx = find_inode(path);
    if (idx < 0) return -1;

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

    struct user_session *s = session_get();
    uint16_t cur_uid = s ? (uint16_t)s->uid : 0;
    uint16_t cur_gid = s ? (uint16_t)s->gid : 0;
    uint16_t m       = inodes[idx].mode;

    /* Root bypasses permission checks */
    if (cur_uid == 0) return 0;

    int shift;
    if (cur_uid == inodes[idx].uid)      shift = 6; /* owner */
    else if (cur_gid == inodes[idx].gid) shift = 3; /* group */
    else                                 shift = 0; /* other */

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
    out[9] = '\0';
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
