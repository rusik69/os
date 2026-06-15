#ifndef HFSPLUS_H
#define HFSPLUS_H

#include "types.h"
#include "vfs.h"

#define HFSPLUS_SECTOR_SIZE  512
#define HFSPLUS_SIG_HFSPLUS  0x482B   /* "H+" */
#define HFSPLUS_SIG_HFSX     0x4858   /* "HX" */
#define HFSPLUS_VOL_HEADER_SECTOR 2
#define HFSPLUS_MAX_NAME      255

/* Catalog record types */
#define HFSPLUS_FOLDER_REC   1
#define HFSPLUS_FILE_REC     2
#define HFSPLUS_FOLDER_THREAD_REC 3
#define HFSPLUS_FILE_THREAD_REC   4

/* File type/mode flags */
#define HFSPLUS_FILE_MASK    0x3FFF

/* Permissions */
#define HFSPLUS_PERM_RUSR  0x0100
#define HFSPLUS_PERM_WUSR  0x0080
#define HFSPLUS_PERM_XUSR  0x0040
#define HFSPLUS_PERM_RGRP  0x0020
#define HFSPLUS_PERM_WGRP  0x0010
#define HFSPLUS_PERM_XGRP  0x0008
#define HFSPLUS_PERM_ROTH  0x0004
#define HFSPLUS_PERM_WOTH  0x0002
#define HFSPLUS_PERM_XOTH  0x0001

/* Finder flags */
#define HFSPLUS_ISDIR      0x4000  /* kIsDirectory */
#define HFSPLUS_ISFILE     0x2000  /* kIsFile */

/* Extent descriptor */
#define HFSPLUS_EXTENT_COUNT 8

/* B-tree node types */
#define BT_LEAF_NODE    -1
#define BT_INDEX_NODE   0
#define BT_HEADER_NODE  1
#define BT_MAP_NODE     2

/* Key compare */
#define HFSPLUS_CASE_SENSITIVE   0xEF
#define HFSPLUS_CASE_INSENSITIVE 0xCF

#pragma pack(push, 1)

/* Volume header */
struct hfsplus_vh {
    uint16_t signature;           /* "H+" or "HX" */
    uint16_t version;
    uint32_t attributes;
    uint32_t last_mount_version;
    uint32_t journal_info_block;
    uint32_t create_date;
    uint32_t modify_date;
    uint32_t backup_date;
    uint32_t checked_date;
    uint32_t file_count;
    uint32_t folder_count;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t next_allocation_id;
    uint32_t rtree_clump_size;
    uint32_t alloc_clump_size;
    uint32_t total_files;
    uint32_t total_folders;
    uint32_t found_count;
    uint32_t found_count_backup;
    uint8_t  vh_reserved[8];
    uint32_t encodings_bmp[8];
    uint8_t  finder_info[32];
    /* Extents overflow file info */
    uint8_t  extents_file[80];
    /* Catalog file info */
    uint8_t  catalog_file[80];
    /* Attributes file info */
    uint8_t  attributes_file[80];
    /* Startup file info */
    uint8_t  startup_file[80];
} __attribute__((packed));

/* Fork data (in volume header) */
struct hfsplus_fork_data {
    uint64_t total_size;
    uint32_t clump_size;
    uint32_t total_blocks;
    /* 8 extents */
    struct hfsplus_extent {
        uint32_t start_block;
        uint32_t block_count;
    } extents[8];
} __attribute__((packed));

/* B-tree node descriptor */
struct hfsplus_btnode_descriptor {
    uint32_t fLink;
    uint32_t bLink;
    int8_t   kind;
    uint8_t  height;
    uint16_t num_recs;
    uint16_t reserved;
} __attribute__((packed));

/* B-tree header record (after descriptor in header node) */
struct hfsplus_btree_header {
    uint16_t tree_depth;
    uint32_t root_node;
    uint32_t leaf_records;
    uint32_t first_leaf;
    uint32_t last_leaf;
    uint16_t node_size;
    uint16_t max_key_len;
    uint32_t total_nodes;
    uint16_t free_nodes;
    uint16_t clump_size_id;  /* reserved2 in some docs */
    uint8_t  btree_type;
    uint8_t  key_compare;
    uint32_t attributes;
    uint32_t reserved3[16];
} __attribute__((packed));

/* HFS+ catalog key */
struct hfsplus_cat_key {
    uint16_t key_length;        /* includes this field */
    uint32_t parent_id;
    uint16_t node_name_length;
    /* followed by node_name[] (UTF-16BE) */
} __attribute__((packed));

/* HFS+ catalog folder record */
struct hfsplus_cat_folder {
    uint16_t record_type;       /* 1 */
    uint16_t flags;
    uint32_t valence;           /* number of items in folder */
    uint32_t folder_id;
    uint32_t create_date;
    uint32_t modify_date;
    uint32_t backup_date;
    uint8_t  finder_info[16];
    uint8_t  text_encoding[4];
    uint16_t reserved;
} __attribute__((packed));

/* HFS+ catalog file record */
struct hfsplus_cat_file {
    uint16_t record_type;       /* 2 */
    uint16_t flags;
    uint32_t reserved1;
    uint32_t file_id;
    uint32_t create_date;
    uint32_t modify_date;
    uint32_t backup_date;
    uint8_t  finder_info[16];
    uint8_t  text_encoding[4];
    uint16_t reserved2;
    /* Data fork info */
    struct hfsplus_fork_data data_fork;
    /* Resource fork info */
    struct hfsplus_fork_data resource_fork;
} __attribute__((packed));

/* Thread record (used for hard links) */
struct hfsplus_thread_rec {
    uint16_t record_type;
    uint16_t reserved;
    uint32_t parent_id;
    uint16_t node_name_length;
} __attribute__((packed));

/* HFS+ extent key */
struct hfsplus_ext_key {
    uint16_t key_length;
    uint8_t  fork_type;        /* 0x00 = data fork, 0xFF = resource fork */
    uint8_t  pad;
    uint32_t file_id;
    uint32_t start_block;
} __attribute__((packed));

/* HFS+ extent record */
struct hfsplus_extent_record {
    struct hfsplus_extent extents[8];
} __attribute__((packed));

#pragma pack(pop)

/* Private mount data */
struct hfsplus_priv {
    uint8_t  dev_id;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;

    /* Catalog B-tree info */
    uint32_t cat_node_size;
    uint32_t cat_root_node;
    uint8_t  cat_key_compare;
    uint64_t cat_start_offset;    /* byte offset in device */

    /* Extents overflow B-tree info */
    uint32_t ext_node_size;
    uint32_t ext_root_node;
    uint64_t ext_start_offset;

    /* Attributes B-tree info */
    uint32_t attr_node_size;
    uint32_t attr_root_node;
    uint64_t attr_start_offset;
};

int hfsplus_probe(uint8_t dev_id);
int hfsplus_init(void);
#endif
