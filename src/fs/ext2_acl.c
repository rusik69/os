/*
 * src/fs/ext2_acl.c — ext2 POSIX ACL get/set via extended attributes.
 *
 * Implements POSIX Access Control List operations for ext2 filesystems.
 * ACLs are serialized into the "system.posix_acl_access" and
 * "system.posix_acl_default" extended attributes stored in the ext2
 * extended attribute block (i_file_acl).
 *
 * The on-disk format matches the Linux POSIX ACL xattr format:
 *   uint32_t count;
 *   struct { le16 tag; le16 perm; le32 id; } a_entries[count];
 *
 * Supported ACL tags: ACL_USER_OBJ, ACL_USER, ACL_GROUP_OBJ,
 * ACL_GROUP, ACL_MASK, ACL_OTHER
 */

#define KERNEL_INTERNAL
#include "ext2.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"
#include "vfs.h"

/* ── ACL serialization format ─────────────────────────────────────────
 *
 * The system.posix_acl_access and system.posix_acl_default extended
 * attributes store ACLs in a compact binary format compatible with
 * the Linux POSIX ACL representation:
 *
 *   uint32_t count;
 *   struct posix_acl_xattr_entry entries[count];
 *
 * Each entry:
 *   uint16_t tag;   ACL_USER_OBJ (1), ACL_USER (2), etc.
 *   uint16_t perm;  permission bits (r/w/x)
 *   uint32_t id;    user/group ID (for ACL_USER / ACL_GROUP)
 *
 * This matches the format used by src/fs/posix_acl.c so ACLs stored
 * through either path are compatible.
 */

#define ACL_XATTR_ACCESS  "system.posix_acl_access"
#define ACL_XATTR_DEFAULT "system.posix_acl_default"

/* On-disk ACL xattr entry format (packed) */
struct ext2_acl_xattr_entry {
	uint16_t tag;
	uint16_t perm;
	uint32_t id;
} __attribute__((packed));

/* Maximum ACL entry count (USER_OBJ + USER + GROUP_OBJ + GROUP + MASK + OTHER) */
#define EXT2_ACL_MAX_ENTRIES 6

/* ── Serialize an in-memory posix_acl to xattr binary format ───────── */

static int ext2_acl_serialize(const struct posix_acl *acl,
                               void *buf, size_t buf_size)
{
	uint32_t count;
	size_t needed;
	uint8_t *data;
	struct ext2_acl_xattr_entry *entries;

	if (!acl || !buf)
		return -EINVAL;

	count = (uint32_t)acl->count;
	if (count == 0 || count > EXT2_ACL_MAX_ENTRIES)
		return -EINVAL;

	needed = sizeof(count) + count * sizeof(struct ext2_acl_xattr_entry);
	if (buf_size < needed)
		return -ERANGE;

	data = (uint8_t *)buf;
	memcpy(data, &count, sizeof(count));

	entries = (struct ext2_acl_xattr_entry *)(data + sizeof(count));
	for (uint32_t i = 0; i < count; i++) {
		entries[i].tag = acl->entries[i].tag;
		entries[i].perm = acl->entries[i].perm;
		entries[i].id = acl->entries[i].id;
	}

	return (int)needed;
}

/* ── Deserialize xattr binary data to an in-memory posix_acl ──────── */

static int ext2_acl_deserialize(const void *data, size_t data_size,
                                 struct posix_acl *acl)
{
	uint32_t count;
	size_t needed;
	const struct ext2_acl_xattr_entry *entries;

	if (!data || !acl)
		return -EINVAL;

	if (data_size < sizeof(uint32_t))
		return -EINVAL;

	memcpy(&count, data, sizeof(count));
	if (count == 0 || count > EXT2_ACL_MAX_ENTRIES)
		return -EINVAL;

	needed = sizeof(count) + count * sizeof(struct ext2_acl_xattr_entry);
	if (data_size < needed)
		return -EINVAL;

	entries = (const struct ext2_acl_xattr_entry *)
		((const uint8_t *)data + sizeof(count));

	for (uint32_t i = 0; i < count; i++) {
		acl->entries[i].tag = entries[i].tag;
		acl->entries[i].perm = entries[i].perm;
		acl->entries[i].id = entries[i].id;
	}
	acl->count = (int)count;

	return 0;
}

/* ── ext2_acl_set ─────────────────────────────────────────────────────
 *
 * Set a POSIX ACL on an ext2 inode by serializing it and storing it
 * in the ext2 extended attribute block.
 *
 * @ep:    ext2 private data
 * @ino:   inode number
 * @inode: ext2 inode (will be updated if EA block allocated)
 * @name:  xattr name (ACL_XATTR_ACCESS or ACL_XATTR_DEFAULT)
 * @acl:   POSIX ACL to store
 *
 * Returns 0 on success, negative errno on failure.
 */

int ext2_acl_set(struct ext2_priv *ep, uint32_t ino,
                  struct ext2_inode *inode,
                  const char *name, const struct posix_acl *acl)
{
	uint8_t buf[256];
	int ret;

	if (!ep || !inode || !name || !acl)
		return -EINVAL;

	/* Validate the xattr name */
	if (strcmp(name, ACL_XATTR_ACCESS) != 0 &&
	    strcmp(name, ACL_XATTR_DEFAULT) != 0)
		return -EINVAL;

	/* Serialize the ACL to binary format */
	ret = ext2_acl_serialize(acl, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	/* Store via the ext2 EA mechanism */
	return ext2_ea_set(ep, ino, inode, name, buf, (size_t)ret);
}

/* ── ext2_acl_get ─────────────────────────────────────────────────────
 *
 * Get a POSIX ACL from an ext2 inode by reading the extended attribute
 * block and deserializing it.
 *
 * @ep:    ext2 private data
 * @ino:   inode number
 * @inode: ext2 inode to read ACL from
 * @name:  xattr name (ACL_XATTR_ACCESS or ACL_XATTR_DEFAULT)
 * @acl:   output: populated POSIX ACL
 *
 * Returns 0 on success, -ENODATA if no ACL exists, negative errno on error.
 */

int ext2_acl_get(struct ext2_priv *ep, uint32_t ino,
                  struct ext2_inode *inode,
                  const char *name, struct posix_acl *acl)
{
	uint8_t buf[256];
	int ret;

	if (!ep || !inode || !name || !acl)
		return -EINVAL;

	/* Validate the xattr name */
	if (strcmp(name, ACL_XATTR_ACCESS) != 0 &&
	    strcmp(name, ACL_XATTR_DEFAULT) != 0)
		return -EINVAL;

	/* No EA block — no ACL */
	if (inode->i_file_acl == 0) {
		acl->count = 0;
		return -ENODATA;
	}

	/* Read the EA value from the ext2 EA block */
	ret = ext2_ea_get(ep, ino, inode, name, buf, sizeof(buf));
	if (ret < 0) {
		if (ret == -ENODATA)
			acl->count = 0;
		return ret;
	}

	/* Deserialize the binary data into a posix_acl */
	return ext2_acl_deserialize(buf, (size_t)ret, acl);
}

/* ── ext2_acl_remove ───────────────────────────────────────────────────
 *
 * Remove a POSIX ACL (either access or default) from an ext2 inode.
 *
 * @ep:    ext2 private data
 * @ino:   inode number
 * @inode: ext2 inode (updated if EA block freed)
 * @name:  xattr name (ACL_XATTR_ACCESS or ACL_XATTR_DEFAULT)
 *
 * Returns 0 on success, -ENODATA if not present, negative errno on error.
 */

int ext2_acl_remove(struct ext2_priv *ep, uint32_t ino,
                     struct ext2_inode *inode,
                     const char *name)
{
	if (!ep || !inode || !name)
		return -EINVAL;

	if (strcmp(name, ACL_XATTR_ACCESS) != 0 &&
	    strcmp(name, ACL_XATTR_DEFAULT) != 0)
		return -EINVAL;

	return ext2_ea_remove(ep, ino, inode, name);
}

/* ── ext2 generic permission check with ACL support ────────────────────
 *
 * Checks if uid/gid has the requested permission on the given ext2 inode.
 * Consults the POSIX ACL (if present) before falling back to the
 * traditional mode-bit check.
 *
 * @ep:       ext2 private data
 * @path:     filesystem path (for VFS-level fallback, may be NULL)
 * @ino:      inode number
 * @inode:    ext2 inode
 * @uid:      requesting user's UID
 * @gid:      requesting user's GID
 * @mode:     file mode bits (from inode->i_mode)
 * @file_uid: file owner UID
 * @file_gid: file owner GID
 * @op:       required permission (4=read, 2=write, 1=execute)
 *
 * Returns 0 on success (granted), -EACCES on denial.
 */

int ext2_acl_permission(struct ext2_priv *ep, const char *path,
                         uint32_t ino, struct ext2_inode *inode,
                         uint16_t uid, uint16_t gid,
                         uint16_t mode, uint16_t file_uid,
                         uint16_t file_gid, uint16_t op)
{
	struct posix_acl acl;
	int ret;
	uint16_t owner_perm;
	uint16_t user_perm;
	int user_match;
	uint16_t group_perm;
	int group_match;
	uint16_t other_perm;
	uint16_t mask_perm;
	int has_mask;
	uint16_t effective;
	uint16_t perm_bits;

	(void)path;

	/* Root can do anything */
	if (uid == 0)
		return 0;

	/* Try to load ACL from the ext2 EA block */
	ret = ext2_acl_get(ep, ino, inode, ACL_XATTR_ACCESS, &acl);
	if (ret == 0 && acl.count > 0) {
		/* Parse ACL entries */
		owner_perm = 0;
		user_perm = 0;
		user_match = 0;
		group_perm = 0;
		group_match = 0;
		other_perm = 0;
		mask_perm = 7;
		has_mask = 0;

		for (int i = 0; i < acl.count; i++) {
			switch (acl.entries[i].tag) {
			case ACL_USER_OBJ:
				owner_perm = acl.entries[i].perm;
				break;
			case ACL_USER:
				if (acl.entries[i].id == uid) {
					user_perm = acl.entries[i].perm;
					user_match = 1;
				}
				break;
			case ACL_GROUP_OBJ:
				group_perm = acl.entries[i].perm;
				group_match = 1;
				break;
			case ACL_GROUP:
				if (acl.entries[i].id == gid) {
					group_perm = acl.entries[i].perm;
					group_match = 1;
				}
				break;
			case ACL_MASK:
				mask_perm = acl.entries[i].perm;
				has_mask = 1;
				break;
			case ACL_OTHER:
				other_perm = acl.entries[i].perm;
				break;
			default:
				return -EINVAL;
			}
		}

		/* Execute access check in POSIX order */

		/* Step 1: file owner */
		if (acl.entries[0].tag == ACL_USER_OBJ) {
			if (owner_perm & op)
				return 0;
			return -EACCES;
		}

		/* Step 2: matching named user (AND with mask if present) */
		if (user_match) {
			effective = has_mask ? (user_perm & mask_perm) : user_perm;
			if (effective & op)
				return 0;
			return -EACCES;
		}

		/* Step 3: owning group / matching named group (AND with mask) */
		if (group_match) {
			effective = has_mask ? (group_perm & mask_perm) : group_perm;
			if (effective & op)
				return 0;
			return -EACCES;
		}

		/* Step 4: other */
		if (other_perm & op)
			return 0;

		return -EACCES;
	}

	/* No ACL — traditional Unix permission check */
	if (uid == file_uid) {
		perm_bits = (mode >> 6) & 7;
	} else if (gid == file_gid) {
		perm_bits = (mode >> 3) & 7;
	} else {
		perm_bits = mode & 7;
	}

	return (perm_bits & op) ? 0 : -EACCES;
}

#ifdef MODULE
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("ext2 POSIX ACL get/set via extended attributes");
MODULE_AUTHOR("OS Kernel Team");
#endif
