#ifndef MDADM_H
#define MDADM_H

#include "types.h"

#define RAID_SUPER_MAGIC 0xA0B0C0D0

/* RAID superblock (placed at end of each disk in the array) */
struct raid_super {
    uint32_t magic;         /* RAID_SUPER_MAGIC */
    uint32_t level;         /* RAID level (0 = RAID0, 1 = RAID1) */
    uint32_t num_disks;     /* number of member disks */
    uint32_t chunk_size;    /* stripe chunk size in sectors (RAID0) */
    uint64_t disk_sectors;  /* total sectors on this disk */
    uint8_t  uuid[16];      /* array UUID (unique per array) */
    uint32_t checksum;      /* simple XOR checksum of preceding fields */
} __attribute__((packed));

/* Write RAID superblock to a buffer at the given sector offset (end of disk).
   Returns 0 on success, -1 on error. */
int raid_write_super(uint8_t *buffer, uint64_t sector_offset,
                     uint32_t level, uint32_t num_disks,
                     uint32_t chunk_size, uint64_t disk_sectors,
                     const uint8_t *uuid);

/* Read and validate a RAID superblock from a buffer.
   Returns 0 on success, -1 on error (bad magic/checksum). */
int raid_read_super(const uint8_t *buffer, uint64_t sector_offset,
                    struct raid_super *super);

/* Compute the simple XOR checksum of a superblock. */
uint32_t raid_super_checksum(const struct raid_super *super);

/* ── RAID0 helpers (mdadm_ext.c) ───────────────────────────────────── */

int raid_set_level_raid0(struct raid_super *super);
int raid_is_raid0(const struct raid_super *super);
int raid_create_raid0(struct raid_super *super, uint32_t num_disks,
                      uint32_t chunk_size, uint64_t disk_sectors,
                      const uint8_t *uuid);

/* ── RAID1 (mirroring) ─────────────────────────────────────────────── */

#define RAID1_MAX_MEMBERS 8
#define MD_BLOCKDEV_BASE 14  /* block device id base for MD arrays */

/* Array state flags */
#define RAID_ARRAY_CLEAN    0
#define RAID_ARRAY_DEGRADED 1  /* at least one member failed, still operational */
#define RAID_ARRAY_FAILED   2  /* all members failed — I/O will error */

/* Per-member state */
#define RAID_MEMBER_ACTIVE  0
#define RAID_MEMBER_FAILED  1

struct raid_member {
    int      dev_id;       /* member block device id */
    int      state;        /* RAID_MEMBER_ACTIVE or RAID_MEMBER_FAILED */
    uint64_t sector_count; /* cached sector count of this member */
};

/* ── RAID1 array ──────────────────────────────────────────────────── */

struct raid1_array {
    int    array_id;        /* MD block device id */
    int    state;           /* RAID_ARRAY_CLEAN / DEGRADED / FAILED */
    int    num_members;     /* number of member disks */
    struct raid_member members[RAID1_MAX_MEMBERS];
    uint64_t array_sectors; /* total sectors (all members have at least this many) */
    uint8_t uuid[16];       /* array UUID */
};

/* Create a RAID1 array from a list of member block device IDs.
 * Returns the MD block device id on success, -1 on error.
 * The block device is registered as md0, md1, etc. */
int  raid1_create(const int *member_dev_ids, int num_members);

/* Destroy a RAID1 array (unregister block device). */
void raid1_destroy(int array_id);

/* Check if a device is a member of a RAID1 array and mark it failed.
 * Returns 0 on success, -1 if the device is not a member. */
int  raid1_member_failed(int array_id, int dev_id);

/* Query RAID1 array status. Returns 0 on success, -1 if not found. */
int  raid1_status(int array_id, int *state_out, int *num_members_out,
                  uint64_t *sectors_out);

/* ── RAID0 (striping) ──────────────────────────────────────────────── */

#define RAID0_MAX_DISKS 16
#define RAID0_DEFAULT_CHUNK_SECT 64  /* 32 KB default chunk */

struct raid0_array {
    int    array_id;        /* MD block device id */
    int    state;           /* RAID_ARRAY_CLEAN / DEGRADED / FAILED */
    int    num_disks;       /* number of member disks */
    struct raid_member disks[RAID0_MAX_DISKS];
    uint32_t chunk_size;    /* stripe chunk in sectors */
    uint64_t stripe_size;   /* chunk_size * num_disks in sectors (full stripe) */
    uint64_t disk_sectors;  /* sectors per member (min of all) */
    uint64_t array_sectors; /* total array sectors = disk_sectors * num_disks */
    uint8_t uuid[16];       /* array UUID */
};

/* Create a RAID0 array (striping). chunk_size in sectors (0 = default). */
int  raid0_create(const int *member_dev_ids, int num_disks,
                  uint32_t chunk_size, const uint8_t *uuid);

/* Destroy a RAID0 array. */
void raid0_destroy(int array_id);

/* Query RAID0 array status. */
int  raid0_status(int array_id, int *state_out, int *num_disks_out,
                  uint64_t *sectors_out, uint32_t *chunk_out);

/* ── RAID10 (stripe of mirrors, RAID1+0) ─────────────────────────── */

#define RAID10_MAX_MIRRORS  8   /* max mirror pairs (16 disks total) */
#define RAID10_MAX_DISKS    16

struct raid10_mirror_pair {
    struct raid_member primary;    /* first member of mirror */
    struct raid_member secondary;  /* second member of mirror */
};

struct raid10_array {
    int    array_id;        /* MD block device id */
    int    state;           /* RAID_ARRAY_CLEAN / DEGRADED / FAILED */
    int    num_pairs;       /* number of mirror pairs */
    struct raid10_mirror_pair pairs[RAID10_MAX_MIRRORS];
    uint32_t chunk_size;    /* stripe chunk in sectors */
    uint64_t stripe_size;   /* chunk_size * num_pairs in sectors */
    uint64_t pair_sectors;  /* sectors per pair (min of both members across all pairs) */
    uint64_t array_sectors; /* total array sectors = pair_sectors * num_pairs */
    uint8_t uuid[16];       /* array UUID */
};

/* Create a RAID10 array. member_dev_ids must have an even number of entries.
 * chunk_size in sectors (0 = default). Pairs are (dev[0],dev[1]), (dev[2],dev[3]), ... */
int  raid10_create(const int *member_dev_ids, int num_disks,
                   uint32_t chunk_size, const uint8_t *uuid);

/* Destroy a RAID10 array. */
void raid10_destroy(int array_id);

/* Query RAID10 array status. */
int  raid10_status(int array_id, int *state_out, int *num_pairs_out,
                   uint64_t *sectors_out, uint32_t *chunk_out);

/* ── Common helpers ────────────────────────────────────────────────── */

/* Mark a member device as failed in any array type.
 * Scans all registered arrays (RAID0, RAID1, RAID10). */
void md_member_failed(int array_id, int dev_id, int level);

/* Initialize the MD subsystem (RAID0/1/10). Called once during boot. */
void raid_md_init(void);

#endif /* MDADM_H */
