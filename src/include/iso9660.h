#ifndef ISO9660_H
#define ISO9660_H

#include "types.h"
#include "vfs.h"

#define ISO9660_SECTOR_SIZE 2048
#define ISO9660_MAGIC 0x0143443031 /* "CD001" in LE */

/* Primary volume descriptor */
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

int iso9660_mount(const char *mountpoint, uint8_t dev_id);
int iso9660_init(void);

#endif /* ISO9660_H */
