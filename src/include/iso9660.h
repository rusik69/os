#ifndef ISO9660_H
#define ISO9660_H

#include "types.h"
#include "vfs.h"

#define ISO9660_SECTOR_SIZE 2048
#define ISO9660_MAGIC 0x0143443031 /* "CD001" in LE */

/* Primary volume descriptor (type 1) */
struct iso_primary_desc {
    uint8_t  type;            /* 1 = primary */
    char     id[5];           /* "CD001" */
    uint8_t  version;
    uint8_t  unused1;
    char     system_id[32];
    char     volume_id[32];
    uint8_t  unused2[8];
    uint32_t volume_space_size_le;
    uint32_t volume_space_size_be;
    uint8_t  unused3[32];
    uint16_t volume_set_size_le;
    uint16_t volume_set_size_be;
    uint16_t volume_seq_num_le;
    uint16_t volume_seq_num_be;
    uint16_t logical_block_size_le;
    uint16_t logical_block_size_be;
    uint32_t path_table_size_le;
    uint32_t path_table_size_be;
    uint32_t path_table_l_loc_le;
    uint32_t path_table_l_loc_be;
    uint32_t path_table_opt_loc_le;
    uint32_t path_table_opt_loc_be;
    uint8_t  root_dir[34];    /* directory record for root */
    char     volume_set_id[128];
    char     publisher_id[128];
    char     preparer_id[128];
    char     application_id[128];
} __attribute__((packed));

/* Supplementary Volume Descriptor (type 2) — used by Joliet.
 * Same layout as the PVD but uses UCS-2BE filenames and has
 * escape sequences in the unused fields for character set info. */
struct iso_supplementary_desc {
    uint8_t  type;            /* 2 = supplementary */
    char     id[5];           /* "CD001" */
    uint8_t  version;
    uint8_t  flags;           /* bit 0 = Joliet (escape sequences present) */
    char     system_id[32];
    char     volume_id[32];
    uint8_t  unused2[8];
    uint32_t volume_space_size_le;
    uint32_t volume_space_size_be;
    uint8_t  escape_sequences[32]; /* character set escape sequences */
    uint16_t volume_set_size_le;
    uint16_t volume_set_size_be;
    uint16_t volume_seq_num_le;
    uint16_t volume_seq_num_be;
    uint16_t logical_block_size_le;
    uint16_t logical_block_size_be;
    uint32_t path_table_size_le;
    uint32_t path_table_size_be;
    uint32_t path_table_l_loc_le;
    uint32_t path_table_l_loc_be;
    uint32_t path_table_opt_loc_le;
    uint32_t path_table_opt_loc_be;
    uint8_t  root_dir[34];    /* directory record for root (UCS-2BE names) */
    char     volume_set_id[128];
    char     publisher_id[128];
    char     preparer_id[128];
    char     application_id[128];
} __attribute__((packed));

/* Joliet escape sequences (in escape_sequences field, typically at offset 0):
 *   %/@  = UCS-2 level 1 (no combining characters)
 *   %/C  = UCS-2 level 2
 *   %/E  = UCS-2 level 3 (includes combining characters)
 */
#define JOLIET_ESC_LEVEL1_0  0x25  /* '%' */
#define JOLIET_ESC_LEVEL1_1  0x2F  /* '/' */
#define JOLIET_ESC_LEVEL1_2  0x40  /* '@' */
#define JOLIET_ESC_LEVEL2_2  0x43  /* 'C' */
#define JOLIET_ESC_LEVEL3_2  0x45  /* 'E' */

/* Directory record */
struct iso_dir_record {
    uint8_t  length;
    uint8_t  ext_attr_length;
    uint32_t extent_loc_le;
    uint32_t extent_loc_be;
    uint32_t data_length_le;
    uint32_t data_length_be;
    uint8_t  datetime[7];
    uint8_t  flags;
    uint8_t  file_unit_size;
    uint8_t  interleave_gap;
    uint16_t volume_seq_num_le;
    uint16_t volume_seq_num_be;
    uint8_t  name_len;
    char     name[1]; /* variable length */
} __attribute__((packed));

#define ISO_FLAG_DIRECTORY 0x02

/* ── Rock Ridge Interchange Protocol (RRIP) / SUSP structures ──── */

/* SUSP (System Use Sharing Protocol) entry header */
struct susp_entry_header {
    uint8_t  sig[2];     /* two-character signature */
    uint8_t  len;        /* length of this entry (including header) */
    uint8_t  version;    /* version (usually 1) */
} __attribute__((packed));

/* SUSP SP (Signature Processing) entry — must be first in root's system use */
struct susp_sp_entry {
    struct susp_entry_header hdr;  /* sig = "SP", len = 7, version = 1 */
    uint8_t  magic_byte1;          /* 0xBE */
    uint8_t  magic_byte2;          /* 0xEF */
    uint8_t  skip_bytes;           /* bytes to skip before SUSP entries */
} __attribute__((packed));

/* SUSP CE (Continuation Entry) — more SUSP entries in another location */
struct susp_ce_entry {
    struct susp_entry_header hdr;  /* sig = "CE", len = 28, version = 1 */
    uint32_t block_loc_le;         /* logical block location of continuation */
    uint32_t block_loc_be;
    uint32_t offset_le;            /* byte offset within that block */
    uint32_t offset_be;
    uint32_t cont_len_le;          /* length of continuation data */
    uint32_t cont_len_be;
} __attribute__((packed));

/* RRIP PX (POSIX Attributes) entry */
struct rrip_px_entry {
    struct susp_entry_header hdr;  /* sig = "PX", len = 44, version = 1 */
    uint32_t mode_le;              /* file mode (S_IXXX flags) */
    uint32_t mode_be;
    uint32_t nlink_le;             /* number of hard links */
    uint32_t nlink_be;
    uint32_t uid_le;               /* user ID */
    uint32_t uid_be;
    uint32_t gid_le;               /* group ID */
    uint32_t gid_be;
    uint32_t atime_le;             /* access time */
    uint32_t atime_be;
    uint32_t mtime_le;             /* modification time */
    uint32_t mtime_be;
    uint32_t ctime_le;             /* creation time */
    uint32_t ctime_be;
} __attribute__((packed));

/* RRIP NM (Alternative Name) entry */
struct rrip_nm_entry {
    struct susp_entry_header hdr;  /* sig = "NM", version = 1 */
    uint8_t  flags;                /* 1 = CONTINUE, 2 = CURRENT */
    char     name[1];              /* variable-length name */
} __attribute__((packed));

/* RRIP SL (Symbolic Link) entry */
struct rrip_sl_entry {
    struct susp_entry_header hdr;  /* sig = "SL", version = 1 */
    uint8_t  flags;                /* 0 = no continue, 1 = continue */
    char     link_data[1];         /* variable-length link components */
} __attribute__((packed));

/* SL component header */
struct rrip_sl_component {
    uint8_t flags;                 /* 0 = plain, 1 = ".", 2 = "..", 8 = root */
    uint8_t len;                   /* length of this component name */
    char    name[1];               /* variable-length name */
} __attribute__((packed));

/* RRIP PN (POSIX Device Node) entry */
struct rrip_pn_entry {
    struct susp_entry_header hdr;  /* sig = "PN", len = 20, version = 1 */
    uint32_t dev_high_le;          /* device number high (major) */
    uint32_t dev_high_be;
    uint32_t dev_low_le;           /* device number low (minor) */
    uint32_t dev_low_be;
} __attribute__((packed));

/* RRIP flags for what we've parsed */
#define RRIP_HAS_PX   0x01
#define RRIP_HAS_NM   0x02
#define RRIP_HAS_SL   0x04
#define RRIP_HAS_CL   0x08
#define RRIP_HAS_PL   0x10
#define RRIP_HAS_RE   0x20
#define RRIP_HAS_TF   0x40
#define RRIP_HAS_PN   0x0100

/* RRIP TF (Timestamps) entry flags — which timestamps are present */
#define RRIP_TF_CREATE  0x01  /* creation time */
#define RRIP_TF_MODIFY  0x02  /* modification time */
#define RRIP_TF_ACCESS  0x04  /* access time */
#define RRIP_TF_ATTRIB  0x08  /* attribute change time */
#define RRIP_TF_BACKUP  0x10  /* backup time */
#define RRIP_TF_EXPIRE  0x20  /* expiration time */
#define RRIP_TF_EFFECT  0x40  /* effective time */

/* RRIP TF (Timestamps) SUSP entry (sig = "TF") */
struct rrip_tf_entry {
    struct susp_entry_header hdr;  /* sig = "TF", version = 1 */
    uint8_t  flags;                /* RRIP_TF_* bitmask — which timestamps follow */
    uint8_t  timestamps[1];        /* variable-length 7-byte ISO 9660 timestamps */
} __attribute__((packed));

/* Rock Ridge enhanced directory entry (fully parsed) */
struct iso_rrip_entry {
    uint32_t extent;
    uint32_t size;
    uint8_t  flags;
    /* Rock Ridge fields */
    uint32_t rr_mode;      /* POSIX mode from PX */
    uint32_t rr_uid;
    uint32_t rr_gid;
    uint32_t rr_nlink;     /* number of hard links from PX */
    uint32_t rr_atime;     /* access time (Unix time_t) from PX or TF */
    uint32_t rr_mtime;     /* modification time (Unix time_t) from PX or TF */
    uint32_t rr_ctime;     /* attribute change time (Unix time_t) from PX or TF */
    uint32_t rr_btime;     /* birth/creation time (Unix time_t) from TF bit 0 or PX */
    char     rr_name[256]; /* long file name from NM */
    char     rr_symlink[256]; /* symlink target from SL */
    /* Rock Ridge device node fields from PN */
    uint32_t rr_dev_major;   /* device major number */
    uint32_t rr_dev_minor;   /* device minor number */
    uint16_t rr_flags;       /* RRIP_HAS_* bitmask */
    /* ISO interleaving fields (ISO 9660 §7.4.5) */
    uint8_t  file_unit_size;    /* blocks per interleave unit (0 = not interleaved) */
    uint8_t  interleave_gap;    /* blocks of other files between units */
    /* ISO fields */
    char     iso_name[256];
};

/* VFS stat mode conversion */
#define RR_S_IFMT     0170000
#define RR_S_IFDIR    0040000
#define RR_S_IFREG    0100000
#define RR_S_IFLNK    0120000
#define RR_S_IFBLK    0060000
#define RR_S_IFCHR    0020000
#define RR_S_IRWXU    00700
#define RR_S_IRUSR    00400
#define RR_S_IWUSR    00200
#define RR_S_IXUSR    00100
#define RR_S_IRWXG    00070
#define RR_S_IRGRP    00040
#define RR_S_IWGRP    00020
#define RR_S_IXGRP    00010
#define RR_S_IRWXO    00007
#define RR_S_IROTH    00004
#define RR_S_IWOTH    00002
#define RR_S_IXOTH    00001

/* Helper: check SUSP entry signature */
static inline int susp_sig_match(const struct susp_entry_header *hdr,
                                  const char sig0, const char sig1)
{
    return hdr->sig[0] == (uint8_t)sig0 && hdr->sig[1] == (uint8_t)sig1;
}

/* ── Multi-session support ──────────────────────────────────────── */

#define ISO9660_MAX_SESSIONS 16

/* Information about one session's volume descriptor set.
 * Each multi-session CD has its own PVD and optional Joliet SVD. */
struct iso_session_info {
    uint32_t session_lba;           /* LBA where this session's VDS begins */
    uint32_t root_extent;           /* root dir extent from PVD */
    uint32_t root_size;             /* root dir size from PVD */
    int      has_joliet;            /* Joliet SVD found in this session */
    uint32_t joliet_root_extent;    /* root dir from Joliet SVD */
    uint32_t joliet_root_size;
};

int iso9660_mount(const char *mountpoint, uint8_t dev_id);
int iso9660_init(void);

/* ── Rock Ridge extension helpers (iso9660_rr.c) ──────────────── */

/* Decode an ISO 9660 7-byte timestamp to Unix time_t (seconds since epoch).
 * Returns 0 on success, -1 if the timestamp is invalid. */
int iso9660_rr_decode_timestamp(const uint8_t iso_time[7], uint32_t *out_time);

/* Parse TF (Timestamps) SUSP entry and extract timestamps.
 * @tf       Pointer to TF entry
 * @tf_len   Total length of the TF entry including header
 * @atime    Output: access time
 * @mtime    Output: modification time
 * @ctime    Output: attribute change time
 * @btime    Output: birth/creation time (TF bit 0), may be NULL
 * Returns bitmask of RRIP_TF_* flags indicating which timestamps were parsed. */
uint8_t iso9660_rr_parse_tf(const struct rrip_tf_entry *tf, uint32_t tf_len,
                             uint32_t *atime, uint32_t *mtime, uint32_t *ctime,
                             uint32_t *btime);

/* Apply Rock Ridge PX attributes to a vfs_stat structure.
 * Fills in mode, uid, gid, nlink from the rrip entry if PX was present.
 * Returns 0 if PX was applied, -1 if no PX data available. */
int iso9660_rr_apply_px(const struct iso_rrip_entry *de, struct vfs_stat *st);

/* Apply Rock Ridge PN (POSIX Device Node) entry to a vfs_stat structure.
 * Fills in dev_major and dev_minor if PN was present. */
void iso9660_rr_apply_pn(const struct iso_rrip_entry *de, struct vfs_stat *st);

#endif /* ISO9660_H */
