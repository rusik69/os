#ifndef LOOP_H
#define LOOP_H

#include "types.h"
#include "blockdev.h"

/*
 * Loop device — block-device-backed virtual block device with
 * multi-segment I/O support (Item 221).
 *
 * Each loop device maps to a contiguous range of an underlying
 * "backing" block device.  I/O requests to the loop device are
 * translated (LBA += offset) and forwarded to the backing device.
 *
 * Multi-segment requests are split by the block layer's existing
 * bio splitting (max_transfer) — each segment gets its own
 * blk_submit_sync call so that drivers see appropriately-sized
 * single-buffer requests.
 *
 * Up to BLOCKDEV_LOOP_MAX (4) loop devices are supported.
 * They occupy block device IDs BLOCKDEV_LOOP0 .. BLOCKDEV_LOOP3.
 */

/* Maximum sectors per loop device (128 MB with 512B sectors) */
#define LOOP_MAX_SECTORS    (256ULL * 1024)

/* ---- Public API ---- */

/*
 * loop_create — create a loop device backed by an existing block device.
 *
 * @backing_dev_id:  The block device ID to use as backing store.
 * @backing_offset:  LBA offset into the backing device (0 = start).
 * @sectors:         Number of sectors this loop device should expose.
 *                   Must not exceed LOOP_MAX_SECTORS or backing device size.
 *
 * Returns the loop device's block device ID (BLOCKDEV_LOOP0..3),
 * or -1 on failure.
 */
int  loop_create(int backing_dev_id, uint64_t backing_offset, uint64_t sectors);

/*
 * loop_destroy — tear down a previously created loop device.
 *
 * @loop_dev_id:  The block device ID returned by loop_create().
 * Returns 0 on success, -1 if the ID is not a valid loop device.
 */
int  loop_destroy(int loop_dev_id);

/*
 * loop_get_backing — query the backing device for a loop device.
 *
 * @loop_dev_id:  Loop block device ID.
 * @out_backing_dev_id:  Set to the backing block device ID.
 * @out_offset:          Set to the LBA offset within the backing device.
 *
 * Returns 0 on success, -1 if the ID is not a loop device.
 */
int  loop_get_backing(int loop_dev_id, int *out_backing_dev_id,
                      uint64_t *out_offset);

#endif /* LOOP_H */
