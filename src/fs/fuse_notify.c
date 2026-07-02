/*
 * src/fs/fuse_notify.c — FUSE notification handling
 *
 * Handles notifications from the userspace FUSE daemon:
 *   - FUSE_NOTIFY_CODE_INVAL_INODE: invalidate page cache / attributes
 *   - FUSE_NOTIFY_CODE_INVAL_ENTRY: invalidate entry cache
 *   - FUSE_NOTIFY_CODE_DELETE:      invalidate entry + page cache for deleted node
 *
 * Notifications arrive as writes to /dev/fuse with oh.unique == 0.
 * fuse_dev_write in fuse_dev.c detects this and calls fuse_process_notify(),
 * which iterates all active FUSE mounts and calls fuse_notify_mount()
 * for each one.
 *
 * This file implements the per-mount notification dispatch.
 */

#include "types.h"
#include "fuse.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "page_cache.h"

/*
 * ── Per-op notification handlers ──────────────────────────────────────
 */

/*
 * Handle FUSE_NOTIFY_CODE_INVAL_INODE — invalidate inode attributes
 * and/or page cache data for a range.
 *
 * The daemon sends this when the file's data or attributes change
 * on the server side, and the kernel cache should be invalidated.
 *
 * Payload: struct fuse_notify_inval_inode_out
 *   .ino — inode number (nodeid) to invalidate
 *   .off — start offset (-1 = all cached data)
 *   .len — length of range (-1 = all cached data)
 */
static int fuse_notify_inval_inode(struct fuse_mount_info *mnt,
                                    const void *data, int len)
{
	const struct fuse_notify_inval_inode_out *out;

	(void)mnt;

	if (!data || len < (int)sizeof(*out))
		return -EINVAL;

	out = (const struct fuse_notify_inval_inode_out *)data;

	kprintf("[fuse] NOTIFY INVAL_INODE: nodeid=%llu off=%lld len=%lld\n",
		(unsigned long long)out->ino,
		(long long)out->off,
		(long long)out->len);

	if (out->off == -1 || out->len == -1) {
		/* Invalidate ALL cached pages for this inode */
		fuse_page_cache_invalidate_node(out->ino);
		kprintf("[fuse]   Invalidated ALL page cache for node %llu\n",
			(unsigned long long)out->ino);
	} else if (out->len > 0) {
		/* Invalidate specific byte range */
		uint64_t offset = (uint64_t)out->off;
		uint32_t size   = (uint32_t)out->len;
		uint64_t start_block = offset / PAGE_SIZE;
		uint64_t end_block   = (offset + size + PAGE_SIZE - 1) / PAGE_SIZE;

		for (uint64_t b = start_block; b < end_block; b++)
			page_cache_remove(out->ino, b);

		kprintf("[fuse]   Invalidated page cache range "
			"[%llu, %llu) for node %llu\n",
			(unsigned long long)offset,
			(unsigned long long)(offset + size),
			(unsigned long long)out->ino);
	}

	return 0;
}

/*
 * Handle FUSE_NOTIFY_CODE_INVAL_ENTRY — invalidate a directory entry
 * in the per-mount entry cache.
 *
 * Payload: struct fuse_notify_inval_entry_out
 *   .parent   — parent directory nodeid
 *   .namelen  — length of the entry name
 *   .name[]   — entry name (NOT null-terminated in wire format)
 */
static int fuse_notify_inval_entry(struct fuse_mount_info *mnt,
                                    const void *data, int len)
{
	const struct fuse_notify_inval_entry_out *out;
	int nhdr_size;
	uint64_t parent;
	char name[256];
	int i;

	if (!data || len < (int)sizeof(*out))
		return -EINVAL;

	out = (const struct fuse_notify_inval_entry_out *)data;
	parent = out->parent;

	/* Validate namelen */
	nhdr_size = (int)sizeof(*out);
	if (out->namelen == 0 || out->namelen > 255 ||
	    nhdr_size + (int)out->namelen > len)
		return -EINVAL;

	/* Copy the name (NOT null-terminated in wire format) */
	memcpy(name, out->name, out->namelen);
	name[out->namelen] = '\0';

	kprintf("[fuse] NOTIFY INVAL_ENTRY: parent=%llu name='%s'\n",
		(unsigned long long)parent, name);

	/* Invalidate matching entries in the per-mount entry cache */
	for (i = 0; i < FUSE_ENTRY_CACHE_SIZE; i++) {
		struct fuse_entry_cache_entry *e = &mnt->entry_cache[i];

		if (e->nodeid != 0 && e->parent == parent &&
		    strcmp(e->name, name) == 0) {
			e->nodeid = 0;
			kprintf("[fuse]   Entry cache slot %d invalidated "
				"(parent=%llu name='%s')\n",
				i, (unsigned long long)parent, name);
		}
	}

	return 0;
}

/*
 * Handle FUSE_NOTIFY_CODE_DELETE — notify kernel that an entry was
 * deleted on the server side.
 *
 * Payload: struct fuse_notify_delete_out
 *   .parent   — parent directory nodeid
 *   .child    — child (deleted entry) nodeid
 *   .namelen  — length of the entry name
 *   .name[]   — entry name (NOT null-terminated in wire format)
 */
static int fuse_notify_delete(struct fuse_mount_info *mnt,
                               const void *data, int len)
{
	const struct fuse_notify_delete_out *out;
	int nhdr_size;
	uint64_t parent;
	uint64_t child;
	char name[256];
	int i;

	if (!data || len < (int)sizeof(*out))
		return -EINVAL;

	out = (const struct fuse_notify_delete_out *)data;
	parent = out->parent;
	child  = out->child;

	/* Validate namelen */
	nhdr_size = (int)sizeof(*out);
	if (out->namelen == 0 || out->namelen > 255 ||
	    nhdr_size + (int)out->namelen > len)
		return -EINVAL;

	/* Copy the name */
	memcpy(name, out->name, out->namelen);
	name[out->namelen] = '\0';

	kprintf("[fuse] NOTIFY DELETE: parent=%llu child=%llu name='%s'\n",
		(unsigned long long)parent,
		(unsigned long long)child, name);

	/* Invalidate page cache for the deleted child node */
	fuse_page_cache_invalidate_node(child);

	/* Invalidate entry cache entries for this (parent, name) */
	for (i = 0; i < FUSE_ENTRY_CACHE_SIZE; i++) {
		struct fuse_entry_cache_entry *e = &mnt->entry_cache[i];

		if (e->nodeid != 0 && e->parent == parent &&
		    strcmp(e->name, name) == 0) {
			e->nodeid = 0;
		}
	}

	/* Also invalidate any entries whose parent was the deleted node */
	for (i = 0; i < FUSE_ENTRY_CACHE_SIZE; i++) {
		struct fuse_entry_cache_entry *e = &mnt->entry_cache[i];

		if (e->nodeid != 0 && e->parent == child)
			e->nodeid = 0;
	}

	kprintf("[fuse]   Invalidated cache for deleted node %llu (%s)\n",
		(unsigned long long)child, name);

	return 0;
}

/*
 * ── Public API: per-mount notification dispatch ───────────────────────
 */

int fuse_notify_mount(struct fuse_mount_info *mnt,
                       uint32_t notify_op,
                       const void *data, int len)
{
	if (!mnt || !mnt->active)
		return -EINVAL;

	switch (notify_op) {
	case FUSE_NOTIFY_CODE_INVAL_INODE:
		return fuse_notify_inval_inode(mnt, data, len);

	case FUSE_NOTIFY_CODE_INVAL_ENTRY:
		return fuse_notify_inval_entry(mnt, data, len);

	case FUSE_NOTIFY_CODE_DELETE:
		return fuse_notify_delete(mnt, data, len);

	case FUSE_NOTIFY_CODE_POLL:
		/* Poll notification not yet supported; silently consume */
		kprintf("[fuse] NOTIFY POLL (unsupported, consumed)\n");
		return 0;

	case FUSE_NOTIFY_CODE_STORE:
		/* Store notification not yet supported; silently consume */
		kprintf("[fuse] NOTIFY STORE (unsupported, consumed)\n");
		return 0;

	case FUSE_NOTIFY_CODE_RETRIEVE:
		/* Retrieve notification not yet supported; silently consume */
		kprintf("[fuse] NOTIFY RETRIEVE (unsupported, consumed)\n");
		return 0;

	default:
		kprintf("[fuse] Unknown notification opcode %u\n",
			(unsigned int)notify_op);
		return -EINVAL;
	}
}

#include "module.h"
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("FUSE notification handling — inode/entry invalidation");
