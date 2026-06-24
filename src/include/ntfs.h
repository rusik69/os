#ifndef NTFS_H
#define NTFS_H

#include "types.h"
#include "vfs.h"

#define NTFS_MAGIC       0x202020205346544EULL  /* "NTFS    " */
#define NTFS_SECTOR_SIZE 512
#define NTFS_MFT_RECORD_SIZE 1024

/* MFT attribute types */
#define AT_STANDARD_INFORMATION   0x10
#define AT_ATTRIBUTE_LIST         0x20
#define AT_FILE_NAME              0x30
#define AT_OBJECT_ID              0x40
#define AT_VOLUME_NAME            0x60
#define AT_VOLUME_INFORMATION     0x70
#define AT_DATA                   0x80
#define AT_INDEX_ROOT             0x90
#define AT_INDEX_ALLOCATION       0xA0
#define AT_BITMAP                 0xB0
#define AT_EA_INFORMATION         0xD0
#define AT_EA                     0xE0
#define AT_LOGGED_UTILITY_STREAM  0x100

/* Attribute flags */
#define ATTR_FLAG_COMPRESSED    (1U << 0)
#define ATTR_FLAG_ENCRYPTED     (1U << 1)
#define ATTR_FLAG_SPARSE        (1U << 2)

/* File attribute flags */
#define FILE_ATTR_READONLY      0x0001
#define FILE_ATTR_HIDDEN        0x0002
#define FILE_ATTR_SYSTEM        0x0004
#define FILE_ATTR_DIRECTORY     0x0010
#define FILE_ATTR_ARCHIVE       0x0020
#define FILE_ATTR_DEVICE        0x0040
#define FILE_ATTR_NORMAL        0x0080
#define FILE_ATTR_TEMPORARY     0x0100
#define FILE_ATTR_SPARSE_FILE   0x0200
#define FILE_ATTR_REPARSE_POINT 0x0400
#define FILE_ATTR_COMPRESSED    0x0800
#define FILE_ATTR_OFFLINE       0x1000
#define FILE_ATTR_NOT_INDEXED   0x2000
#define FILE_ATTR_ENCRYPTED     0x4000

/* MFT record flags */
#define MFT_RECORD_IN_USE   (1U << 0)
#define MFT_RECORD_DIR      (1U << 1)

/* MFT reference numbers */
#define MFT_FILE_MFT       0
#define MFT_FILE_MFTMIRR   1
#define MFT_FILE_LOGFILE   2
#define MFT_FILE_VOLUME    3
#define MFT_FILE_ATTRDEF   4
#define MFT_FILE_ROOT      5
#define MFT_FILE_BITMAP    6
#define MFT_FILE_BOOT      7
#define MFT_FILE_BADCLUS   8

/* NTFS file type for char conversion */
#define NTFS_NAMELEN 255

#pragma pack(push, 1)

/* Boot sector / VBR */
struct ntfs_bpb {
    uint8_t  jmp_boot[3];
    char     oem_id[8];          /* "NTFS    " */
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  unused1[3];
    uint16_t unused2;
    uint8_t  media_descriptor;
    uint16_t unused3;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t unused4;
    uint8_t  unused5[4];
    uint64_t total_sectors;
    uint64_t mft_lcn;            /* cluster containing MFT */
    uint64_t mft_mirr_lcn;
    uint8_t  clusters_per_mft_record;   /* signed: neg = size = 2^(-val) * 512 */
    uint8_t  unused6[3];
    uint8_t  clusters_per_index_record; /* signed: neg = power of 2 */
    uint8_t  unused7[3];
    uint64_t volume_serial;
    uint32_t checksum;
    uint8_t  bootstrap[426];
} __attribute__((packed));

/* MFT record header */
struct ntfs_mft_rec {
    uint32_t magic;          /* "FILE" */
    uint16_t usa_ofs;        /* offset to update sequence array */
    uint16_t usa_count;      /* number of entries in USA */
    uint64_t lsn;
    uint16_t seqno;
    uint16_t link_count;
    uint16_t attr_offset;    /* offset to first attribute */
    uint16_t flags;
    uint32_t bytes_in_use;
    uint32_t bytes_allocated;
    uint64_t base_mft_rec;
    uint16_t next_attr_id;
    uint16_t usa_ofs2;       /* for alignment */
} __attribute__((packed));

/* Attribute header (non-resident) */
struct ntfs_attr_hdr {
    uint32_t type;
    uint32_t length;
    uint8_t  non_resident;
    uint8_t  name_length;
    uint16_t name_offset;
    uint16_t flags;
    uint16_t instance;
    /* Resident fields */
    uint32_t value_length;
    uint16_t value_offset;
    uint8_t  reserved1[2];
    /* Non-resident fields (overlay) */
    uint64_t lowest_vcn;
    uint64_t highest_vcn;
    uint16_t mapping_pairs_offset;
    uint8_t  compression_unit;
    uint8_t  reserved2[5];
    uint64_t alloc_size;
    uint64_t real_size;
    uint64_t initial_size;
} __attribute__((packed));

/* Standard Information attribute */
struct ntfs_std_info {
    uint64_t creation_time;
    uint64_t modification_time;
    uint64_t mft_modification_time;
    uint64_t access_time;
    uint32_t file_attributes;
    uint32_t max_versions;
    uint32_t version;
    uint32_t class_id;
    uint32_t owner_id;
    uint32_t security_id;
    uint64_t quota_charged;
    uint64_t usn;
} __attribute__((packed));

/* File Name attribute */
struct ntfs_file_name {
    uint64_t parent_directory;
    uint64_t creation_time;
    uint64_t modification_time;
    uint64_t mft_modification_time;
    uint64_t access_time;
    uint64_t alloc_size;
    uint64_t real_size;
    uint32_t file_attributes;
    uint16_t ea_size;
    uint16_t name_length;
    uint8_t  name_type;         /* 0=POSIX, 1=Win32, 2=DOS, 3=Win32+DOS */
    uint16_t name[255];         /* UTF-16LE */
} __attribute__((packed));

/* Run (extent) for non-resident data */
struct ntfs_run {
    uint64_t lcn;   /* -1 = sparse */
    uint64_t length; /* in clusters */
};

#pragma pack(pop)

/* Private mount data */
struct ntfs_priv {
    uint8_t  dev_id;
    uint32_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint32_t cluster_size;
    int64_t  clusters_per_mft_record; /* positive or negative */
    int64_t  clusters_per_index_record;
    uint64_t mft_lcn;          /* cluster of MFT */
    uint64_t mft_mirr_lcn;
    uint32_t mft_record_size;
    uint32_t index_record_size;

    /* MFT read buffer */
    uint8_t *mft_buf;
};

int ntfs_probe(uint8_t dev_id);
int ntfs_init(void);
#endif
