#ifndef FAT32_H
#define FAT32_H

#include "types.h"

struct vfs_ops;

#define FAT32_MAX_NAME 256

/* Mount point: which disk to use for FAT32 */
typedef enum {
    FAT32_DISK_ATA  = 0,
    FAT32_DISK_AHCI = 1,
    FAT32_DISK_USB0 = 2,
} fat32_disk_t;

int  fat32_mount(fat32_disk_t disk, uint32_t part_lba); /* 0 = auto-detect partition */
int  fat32_is_mounted(void);

/* Read a file; returns bytes read or negative on error */
int  fat32_read_file(const char *path, void *buf, uint32_t max_size);

/* List directory; names[] filled with up to max entries, returns count */
int  fat32_list_dir(const char *path, char names[][FAT32_MAX_NAME], int max);

/* Returns file size in bytes, or -1 if not found */
int  fat32_file_size(const char *path);

/* Write/create file (8.3 path components); returns bytes written or negative */
int  fat32_write_file(const char *path, const void *data, uint32_t size);

/* Flush FAT copies to disk */
int  fat32_sync(void);

/* Positioned read: read from a file at an arbitrary byte offset,
 * following fragmented cluster chains.  Returns bytes read or negative. */
int  fat32_pread(const char *path, void *buf, uint32_t size, uint32_t offset);

/* Positioned write: write to a file at an arbitrary byte offset.
 * Extends the cluster chain if needed.  Returns bytes written or negative. */
int  fat32_pwrite(const char *path, const void *data, uint32_t size, uint32_t offset);

/* Truncate a file to the specified size.  Returns 0 on success or negative. */
int  fat32_truncate_file(const char *path, uint32_t new_size);

/* Walk N steps forward in a fragmented cluster chain; returns the cluster
 * at that depth or 0 if the chain ends before the target step. */
uint32_t fat32_chain_walk(uint32_t start, uint32_t steps);

/* Count the number of clusters in a chain starting at 'start'. */
uint32_t fat32_chain_length(uint32_t start);

/* Append 'num' clusters to an existing chain.  Returns 0 on success. */
int  fat32_chain_extend(uint32_t cluster, uint32_t num);

/* Create directory (8.3 path components) */
int  fat32_mkdir(const char *path);

/* Remove file (not directories) */
int  fat32_unlink(const char *path);

/* Remove an empty subdirectory (frees cluster chain, removes parent entry) */
int  fat32_rmdir(const char *path);

/* Get volume label (returns 0 on success, -1 on error). */
int  fat32_get_volume_label(char *buf, int max);

/* Set volume label (1-11 chars, uppercase recommended, returns 0 on success). */
int  fat32_set_volume_label(const char *label);

extern struct vfs_ops fat32_vfs_ops;

/* ── VFAT Long File Name API (fat32_lfn.c) ─────────────────────────── */

/* Compute VFAT checksum for an 8.3 name (8-byte name + 3-byte ext) */
uint8_t vfat_checksum(const char name83_8[8], const char name83_3[3]);

/* Determine if a filename needs VFAT long filename entries (1=yes, 0=no) */
int vfat_needs_lfn(const char *name);

/* Count VFAT LFN directory entries needed for a given name (0-20) */
int vfat_count_lfn_entries(const char *name);

/* Build a single VFAT LFN entry in a 32-byte buffer */
void vfat_build_entry(void *entry_out, int ordinal, int is_last,
                       const char *name, int name_offset,
                       uint8_t checksum);

/* Insert all VFAT LFN entries into a sector buffer at a given index */
int vfat_insert_entries(uint8_t *sector_buf, int *entry_idx,
                         const char *leaf,
                         const char name83_8[8], const char name83_3[3]);

/* Check if a directory entry is a VFAT LFN entry */
int vfat_is_lfn_entry(const void *entry);

/* Check if a directory entry is marked as deleted (0xE5) */
int vfat_is_deleted_entry(const void *entry);

/* Check if a directory entry is the end-of-directory marker (0x00) */
int vfat_is_end_of_dir(const void *entry);

/* Compare an entry's 8.3 name against a reference (case-insensitive) */
int vfat_compare_83_name(const void *entry,
                          const char name83_8[8], const char name83_3[3]);

/* Extract the 8.3 name from a directory entry */
void vfat_get_83_name(const void *entry, char name83_8[8], char name83_3[3]);

/* Get the checksum from a VFAT LFN directory entry */
uint8_t vfat_get_lfn_checksum(const void *entry);

/* Delete VFAT LFN entries matching a checksum in a sector buffer */
int vfat_delete_by_checksum(uint8_t *sector_buf, int start_idx, int end_idx,
                             uint8_t checksum);

/* Mark a single directory entry as deleted */
int vfat_mark_entry_deleted(void *entry);

/* Build an 8.3 short name from a long filename (space-padded output) */
void vfat_build_83_name(const char *long_name, char out_name[8], char out_ext[3]);

/* Reconstruct a long filename from VFAT LFN directory entries.
 * Entries are in on-disk physical order (highest ordinal first).
 * Returns the name length on success, negative errno on error. */
int vfat_reconstruct_name(const void *entries, int count,
                           char *out, int out_max);

/* Reconstruct a long filename with checksum validation against the
 * associated 8.3 short name. Returns the name length on success,
 * -EILSEQ if the checksum does not match, negative errno on error. */
int vfat_reconstruct_name_checked(const void *entries, int count,
                                   const char name83_8[8],
                                   const char name83_3[3],
                                   char *out, int out_max);

#endif
