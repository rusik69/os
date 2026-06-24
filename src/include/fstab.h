#ifndef FSTAB_H
#define FSTAB_H

#include "types.h"

/* ── fstab entry ────────────────────────────────────────────────────── */
#define FSTAB_MAX_ENTRIES 16
#define FSTAB_PATH_MAX    64
#define FSTAB_FSNAME_MAX  16
#define FSTAB_OPTS_MAX    64

struct fstab_entry {
    char device[FSTAB_PATH_MAX];      /* e.g. "rootfs", "proc", "/dev/ata0" */
    char mountpoint[FSTAB_PATH_MAX];  /* e.g. "/", "/proc", "/mnt/usb" */
    char fstype[FSTAB_FSNAME_MAX];    /* e.g. "smfs", "proc", "tmpfs", "ext2", "fat32" */
    char options[FSTAB_OPTS_MAX];     /* comma-separated: ro,noexec,nosuid,nodev */
    int  dump;                        /* dump order (0=skip) */
    int  pass;                        /* fsck order (0=skip, 1=root, 2=other) */
};

/* ── Parsed options bitmask ─────────────────────────────────────────── */
#define FSTAB_OPT_RO      (1U << 0)   /* read-only mount */
#define FSTAB_OPT_RW      (1U << 1)   /* read-write mount */
#define FSTAB_OPT_NOEXEC  (1U << 2)   /* disallow program execution */
#define FSTAB_OPT_NOSUID  (1U << 3)   /* block suid/sgid bits */
#define FSTAB_OPT_NODEV   (1U << 4)   /* disallow device access via this mount */

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * fstab_load() - Read /etc/fstab and populate the internal fstab table.
 * Returns number of entries parsed, or 0 if /etc/fstab doesn't exist.
 */
int fstab_load(void);

/**
 * fstab_get_entry() - Get the i-th entry (0-based).
 * Returns NULL if index is out of range.
 */
const struct fstab_entry *fstab_get_entry(int index);

/**
 * fstab_get_count() - Return number of valid fstab entries.
 */
int fstab_get_count(void);

/**
 * fstab_mount_all() - Mount all filesystems listed in /etc/fstab.
 * Calls fstab_load() first if not already loaded.
 * Skips entries where pass == 0 (root already mounted, virtual fs already set up).
 * Returns number of successfully mounted entries.
 */
int fstab_mount_all(void);

/**
 * fstab_parse_options() - Parse a comma-separated options string into a bitmask.
 */
int fstab_parse_options(const char *options);

/**
 * fstab_mount_entry() - Mount a single fstab entry using VFS.
 * Returns 0 on success, negative on error.
 */
int fstab_mount_entry(const struct fstab_entry *ent);

#endif /* FSTAB_H */
