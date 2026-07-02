#ifndef BTRFS_H
#define BTRFS_H

#include "types.h"
#include "vfs.h"

#define BTRFS_SUPER_MAGIC     0x9123683E
#define BTRFS_SUPER_OFFSET    0x10000ULL
#define BTRFS_SB_SIZE         4096
#define BTRFS_MIN_DEV_SIZE    0x10000ULL
#define BTRFS_BLOCK_SIZE      4096
#define BTRFS_MAX_NAME        255
#define BTRFS_MAGIC_SIZE      8

#define BTRFS_ROOT_TREE_OBJECTID   1ULL
#define BTRFS_EXTENT_TREE_OBJECTID 2ULL
#define BTRFS_CHUNK_TREE_OBJECTID  3ULL
#define BTRFS_DEV_TREE_OBJECTID    4ULL
#define BTRFS_FS_TREE_OBJECTID     5ULL
#define BTRFS_CSUM_TREE_OBJECTID   7ULL

#define BTRFS_FIRST_FREE_OBJECTID  256ULL

#define BTRFS_INODE_ITEM_KEY      1
#define BTRFS_INODE_REF_KEY       12
#define BTRFS_DIR_ITEM_KEY        84
#define BTRFS_DIR_INDEX_KEY       96
#define BTRFS_CSUM_ITEM_KEY       52
#define BTRFS_EXTENT_DATA_KEY     108
#define BTRFS_ROOT_ITEM_KEY       132
#define BTRFS_EXTENT_ITEM_KEY     168
#define BTRFS_METADATA_ITEM_KEY   169
#define BTRFS_TREE_BLOCK_REF_KEY  176
#define BTRFS_SHARED_BLOCK_REF_KEY 177
#define BTRFS_EXTENT_DATA_REF_KEY 178
#define BTRFS_SHARED_DATA_REF_KEY 179
#define BTRFS_ROOT_REF_KEY        156
#define BTRFS_ROOT_BACKREF_KEY    144
#define BTRFS_CHUNK_ITEM_KEY      228

#define BTRFS_EXTENT_FLAG_DATA       (1ULL << 0)
#define BTRFS_EXTENT_FLAG_TREE_BLOCK (1ULL << 1)

#define BTRFS_EXTENT_DATA_INLINE   0
#define BTRFS_EXTENT_DATA_REGULAR  1

#define BTRFS_COMPRESS_NONE 0
#define BTRFS_BLOCK_GROUP_RAID_MASK 0xFF
#define BTRFS_MAX_CHUNKS 256
#define BTRFS_ROOT_BACKUP_OFFSET  0x300
#define BTRFS_NUM_ROOT_BACKUPS    4

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFLNK  0120000

#pragma pack(push, 1)
struct btrfs_key {
    uint64_t objectid;
    uint8_t  type;
    uint64_t offset;
};

struct btrfs_disk_key {
    uint64_t objectid;
    uint8_t  type;
    uint64_t offset;
};

struct btrfs_superblock {
    uint8_t  csum[32];
    uint8_t  fsid[16];
    uint64_t bytenr;
    uint64_t flags;
    uint8_t  magic[8];
    uint64_t generation;
    uint64_t root;
    uint64_t chunk_root;
    uint64_t log_root;
    uint64_t log_root_transid;
    uint64_t total_bytes;
    uint64_t bytes_used;
    uint64_t root_dir_objectid;
    uint64_t num_devices;
    uint32_t sectorsize;
    uint32_t nodesize;
    uint32_t leafsize;
    uint32_t stripesize;
    uint32_t sys_chunk_array_size;
    uint64_t chunk_root_generation;
    uint64_t compat_flags;
    uint64_t compat_ro_flags;
    uint64_t incompat_flags;
    uint16_t csum_type;
    uint8_t  root_level;
    uint8_t  chunk_root_level;
    uint8_t  log_root_level;
    uint8_t  _pad0[59];
    uint8_t  label[256];
    uint64_t cache_generation;
    uint64_t uuid_tree_generation;
    uint8_t  metadata_uuid[16];
    uint64_t generation_v2;
    uint8_t  _pad1[118];
} __attribute__((packed));

struct btrfs_header {
    uint8_t  csum[32];
    uint8_t  fsid[16];
    uint64_t bytenr;
    uint64_t flags;
    uint8_t  chunk_tree_uuid[16];
    uint64_t generation;
    uint64_t owner;
    uint32_t nritems;
    uint8_t  level;
} __attribute__((packed));

struct btrfs_key_ptr {
    struct btrfs_disk_key key;
    uint64_t blockptr;
    uint64_t generation;
} __attribute__((packed));

struct btrfs_item {
    struct btrfs_disk_key key;
    uint32_t offset;
    uint32_t size;
} __attribute__((packed));

struct btrfs_inode_item {
    uint64_t generation;
    uint64_t transid;
    uint64_t size;
    uint64_t nbytes;
    uint64_t block_group;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint64_t rdev;
    uint64_t flags;
    uint64_t sequence;
    uint8_t  reserved[64];
    uint64_t atime;
    uint64_t ctime;
    uint64_t mtime;
    uint64_t otime;
} __attribute__((packed));

struct btrfs_root_item {
    struct btrfs_inode_item inode;
    uint64_t generation;
    uint64_t root_dirid;
    struct btrfs_disk_key drop_progress;
    uint8_t  drop_level;
    uint8_t  level;
    uint64_t generation_v2;
    uint8_t  uuid[16];
    uint8_t  parent_uuid[16];
    uint8_t  received_uuid[16];
    uint64_t ctransid;
    uint64_t otransid;
    uint64_t stransid;
    uint64_t rtransid;
    uint64_t last_snapshot;
    uint64_t byte_limit;
    uint64_t bytes_used;
    uint64_t last_snapshot_tranid;
    uint8_t  init;
    uint8_t  _pad2[7];
    uint32_t root_refs;
} __attribute__((packed));

struct btrfs_dir_item {
    struct btrfs_disk_key location;
    uint8_t  type;
    uint8_t  name_len;
    uint16_t data_len;
} __attribute__((packed));

struct btrfs_file_extent_item {
    uint8_t  type;
    uint8_t  compression;
    uint8_t  encryption;
    uint16_t other_encoding;
    /* inline data follows type/compression for inline extents */
    /* for regular: */
    uint64_t ram_bytes;
    uint64_t disk_bytenr;
    uint64_t disk_num_bytes;
    uint64_t offset;
    uint64_t num_bytes;
} __attribute__((packed));

struct btrfs_chunk {
    uint64_t length;
    uint64_t owner;
    uint64_t stripe_len;
    uint64_t type;
    uint32_t io_align;
    uint32_t io_width;
    uint32_t sector_size;
    uint16_t num_stripes;
    uint16_t sub_stripes;
} __attribute__((packed));

struct btrfs_stripe {
    uint64_t dev_id;
    uint64_t offset;
    uint8_t  dev_uuid[16];
} __attribute__((packed));

struct btrfs_root_backup {
    uint64_t root_bytenr;
    uint64_t chunk_root_bytenr;
    uint64_t extent_root_bytenr;
    uint64_t fs_root_bytenr;
    uint64_t dev_root_bytenr;
    uint64_t csum_root_bytenr;
    uint64_t total_bytes;
    uint64_t bytes_used;
    uint64_t num_devices;
    uint64_t reserved[24];
    uint8_t  uuid[16];
} __attribute__((packed));

struct btrfs_extent_item {
    uint64_t refs;
    uint64_t generation;
    uint64_t flags;
} __attribute__((packed));

struct btrfs_extent_inline_ref {
    uint8_t  type;
    uint64_t offset;
} __attribute__((packed));

struct btrfs_tree_block_info {
    struct btrfs_disk_key key;
    uint8_t  level;
} __attribute__((packed));

struct btrfs_extent_data_ref {
    uint64_t root;
    uint64_t objectid;
    uint64_t offset;
    uint32_t count;
} __attribute__((packed));

struct btrfs_shared_data_ref {
    uint32_t count;
} __attribute__((packed));

struct btrfs_root_ref {
    uint64_t dirid;
    uint64_t index;
    uint16_t name_len;
    /* Followed by variable-length name */
} __attribute__((packed));
#pragma pack(pop)

struct btrfs_chunk_map {
    uint64_t logical;
    uint64_t length;
    uint64_t physical;
};

struct btrfs_priv {
    uint8_t  dev_id;
    uint32_t sectorsize;
    uint32_t nodesize;
    uint16_t csum_type;
    uint64_t chunk_root_bytenr;
    uint8_t  chunk_root_level;
    uint64_t root_bytenr;
    uint8_t  root_level;
    uint64_t fs_root_bytenr;
    uint8_t  fs_root_level;
    uint64_t fs_root_dirid;
    uint64_t extent_root_bytenr;
    uint8_t  extent_root_level;
    uint64_t csum_root_bytenr;
    uint8_t  csum_root_level;
    uint32_t num_chunks;
    struct btrfs_chunk_map chunks[BTRFS_MAX_CHUNKS];
};

int btrfs_probe(uint8_t dev_id);
int btrfs_init(void);

/* ── Checksum verification (btrfs_csum.c) ─────────────────────── */
int btrfs_csum_size(uint16_t csum_type);
int btrfs_csum_verify_node(uint8_t *node_buf, uint32_t nodesize,
                            uint16_t csum_type);
int btrfs_csum_verify_data(const uint8_t *csum_leaf_data,
                            uint32_t num_csums, uint32_t block_index,
                            const uint8_t *block_data,
                            uint32_t block_size, uint16_t csum_type);
#endif
