#ifndef SWAP_H
#define SWAP_H

#include "types.h"
#include "vfs.h"
#include "spinlock.h"

/*
 * ── Swap subsystem — block device swap (Item 223) ──────────────────
 *
 * Provides swapon/swapoff and the infrastructure for page-level
 * swap-out/swap-in to a block device.  Each swap area has a superblock
 * (first page) with magic + metadata, followed by a slot bitmap and
 * data pages.
 *
 * Block device I/O uses blk_submit_sync() at sector granularity.
 * Each swap slot is one 4K page = 8 sectors (assuming 512B sectors).
 *
 * Usage:
 *   1. Prepare a swap area on a block device using mkswap.
 *   2. Call swapon("sda2") or swapon("/dev/sda2") to activate.
 *   3. swap_out() and swap_in() move pages to/from the swap device.
 */

/* ── Constants ──────────────────────────────────────────────────────── */

#define SWAP_MAGIC          0x534D415053504157ULL   /* "SWAPSPACE" (little-endian) */
#define SWAP_VERSION        1

#define SWAP_SLOT_SIZE      4096                    /* bytes per swap slot        */
#define SWAP_BITMAP_SIZE(slots)  (((slots) + 63) / 64)  /* bitmap in uint64_t count */
#define SWAP_SUPERBLOCK_PAGES   1                    /* page 0 = superblock       */

#define SWAP_MAX_DEVICES    4                       /* maximum swap devices      */
#define SWAP_NAME_MAX       64                      /* device name length        */

/* ── Swap area superblock (page 0 on the device) ──────────────────── */

struct __attribute__((packed)) swap_superblock {
    uint64_t  magic;               /* SWAP_MAGIC                                */
    uint32_t  version;             /* SWAP_VERSION                              */
    uint32_t  total_slots;         /* how many 4K data slots the area provides  */
    uint32_t  used_slots;          /* how many slots are currently allocated    */
    uint32_t  flags;               /* reserved flags (SWAP_FLAG_*)             */
    uint32_t  checksum;            /* simple xor checksum of this superblock    */
    uint8_t   reserved[4056];      /* zero-filled padding to 4096 bytes         */
};

/* Superblock flags */
#define SWAP_FLAG_ENABLED   0x00000001  /* swap device is active             */
#define SWAP_FLAG_PREFER    0x00000002  /* preferred device (for priority)   */

/* ── Swap device descriptor ────────────────────────────────────────── */

struct swap_device {
    char      path[SWAP_NAME_MAX];    /* block device name (e.g., "sda")    */
    int       blockdev_id;            /* block device ID from blockdev layer */
    uint32_t  total_slots;            /* total data slots available          */
    uint32_t  used_slots;             /* slots currently in use              */
    uint64_t *bitmap;                 /* allocated bitmap (1 = used)         */
    uint32_t  bitmap_slots;           /* number of uint64_t in bitmap        */
    uint32_t  flags;                  /* device flags (SWAP_FLAG_*)          */
    int       active;                 /* 1 = swap device is active           */
};

/* ── Public API ────────────────────────────────────────────────────── */

/** Initialize the swap subsystem.  Called once at boot. */
void swap_init(void);

/**
 * swapon - Activate a swap area on a block device.
 * @name:  block device name (e.g., "sda2" or "/dev/sda2").
 * Returns 0 on success, negative errno on failure.
 *
 * Validates the swap superblock, reads the slot bitmap, and marks the
 * device as active.  Future swap_out() calls will use this device.
 */
int swap_swapon(const char *name);

/**
 * swapoff - Deactivate a swap area.
 * @name:  name of the swap device to deactivate.
 * Returns 0 on success, negative errno on failure.
 *
 * Fails with -EBUSY if any swap slots are still in use.  Writes the
 * updated bitmap back to the device before deactivating.
 */
int swap_swapoff(const char *name);

/**
 * swap_out - Write a physical page to a swap slot.
 * @phys_addr:  physical address of the 4K page to write.
 * @out_slot:   on success, receives the allocated swap slot index.
 * @out_dev:    on success, receives the swap device index.
 * Returns 0 on success, negative errno on failure (-ENOSPC if full).
 */
int swap_out(uint64_t phys_addr, uint32_t *out_slot, int *out_dev);

/**
 * swap_in - Read a page from a swap slot back into physical memory.
 * @dev_idx:    index of the swap device.
 * @slot:       swap slot index (returned by a prior swap_out).
 * @phys_addr:  physical address to read the page into.
 * Returns 0 on success, negative errno on failure.
 */
int swap_in(int dev_idx, uint32_t slot, uint64_t phys_addr);

/**
 * swap_free_slot - Mark a swap slot as free (page swapped back in).
 * @dev_idx:  swap device index.
 * @slot:     slot to free.
 * Returns 0 on success, negative errno on failure.
 */
int swap_free_slot(int dev_idx, uint32_t slot);

/**
 * swap_stats - Return number of active swap devices and total/used slots.
 */
void swap_stats(int *out_devices, uint32_t *out_total, uint32_t *out_used);

/** Human-readable device path for a swap device index. */
const char *swap_device_path(int dev_idx);

#endif /* SWAP_H */
