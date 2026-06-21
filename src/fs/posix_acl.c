/*
 * src/fs/posix_acl.c — POSIX ACL implementation (S150)
 *
 * Implements access control lists for filesystem objects.
 * ACLs are stored in the "system.posix_acl_access" extended attribute.
 *
 * Access check algorithm:
 *   1. If uid matches file owner → check ACL_USER_OBJ entry
 *   2. If uid matches any ACL_USER entry → check that entry (AND with ACL_MASK)
 *   3. If gid matches file group → check ACL_GROUP_OBJ entry (AND with ACL_MASK)
 *   4. If gid matches any ACL_GROUP entry → check that entry (AND with ACL_MASK)
 *   5. Otherwise → check ACL_OTHER entry
 *
 * Entry types: USER_OBJ, USER, GROUP_OBJ, GROUP, MASK, OTHER
 */

#define KERNEL_INTERNAL
#include "vfs.h"
#include "xattr.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"

/* ── ACL serialization format ────────────────────────────────────────
 *
 * The system.posix_acl_access xattr stores ACL entries in a compact
 * binary format:
 *
 *   struct posix_acl_xattr_entry {
 *       uint16_t tag;   // ACL_USER_OBJ, ACL_USER, etc.
 *       uint16_t perm;  // permission bits (r/w/x)
 *       uint32_t id;    // user/group ID
 *   } __attribute__((packed));
 *
 * The xattr value is a sequence of these entries, preceded by a count:
 *   uint32_t count;
 *   struct posix_acl_xattr_entry entries[count];
 */

#define ACL_XATTR_ACCESS "system.posix_acl_access"
#define ACL_XATTR_DEFAULT "system.posix_acl_default"

/* On-disk ACL entry format */
struct posix_acl_xattr_entry {
    uint16_t tag;
    uint16_t perm;
    uint32_t id;
} __attribute__((packed));

/* ── Convert xattr binary data to in-memory ACL ───────────────────── */

static int posix_acl_from_xattr(const void *xattr_value, int xattr_size,
                                 struct posix_acl *acl)
{
    if (!xattr_value || xattr_size < 4 || !acl)
        return -EINVAL;

    const uint8_t *data = (const uint8_t *)xattr_value;
    uint32_t count;
    memcpy(&count, data, sizeof(count));

    if (count == 0 || count > POSIX_ACL_MAX_ENTRIES)
        return -EINVAL;

    if ((uint32_t)xattr_size < 4 + count * sizeof(struct posix_acl_xattr_entry))
        return -EINVAL;

    const struct posix_acl_xattr_entry *xentries =
        (const struct posix_acl_xattr_entry *)(data + 4);

    for (uint32_t i = 0; i < count; i++) {
        acl->entries[i].tag = xentries[i].tag;
        acl->entries[i].perm = xentries[i].perm;
        acl->entries[i].id = xentries[i].id;
    }
    acl->count = (int)count;

    return 0;
}

/* ── Convert in-memory ACL to xattr binary data ───────────────────── */

static int posix_acl_to_xattr(const struct posix_acl *acl,
                               void *xattr_value, int xattr_size)
{
    if (!acl || !xattr_value)
        return -EINVAL;

    uint32_t count = (uint32_t)acl->count;
    int needed = 4 + count * (int)sizeof(struct posix_acl_xattr_entry);
    if (xattr_size < needed)
        return -ERANGE;

    uint8_t *data = (uint8_t *)xattr_value;
    memcpy(data, &count, sizeof(count));

    struct posix_acl_xattr_entry *xentries =
        (struct posix_acl_xattr_entry *)(data + 4);

    for (uint32_t i = 0; i < count; i++) {
        xentries[i].tag = acl->entries[i].tag;
        xentries[i].perm = acl->entries[i].perm;
        xentries[i].id = acl->entries[i].id;
    }

    return needed;
}

/* ── Public API: set ACL via xattr ─────────────────────────────────── */

int posix_acl_set(const char *path, struct posix_acl *acl)
{
    if (!path || !acl)
        return -EINVAL;

    /* Serialize ACL to xattr format */
    uint8_t buf[256];
    int ret = posix_acl_to_xattr(acl, buf, sizeof(buf));
    if (ret < 0)
        return ret;

    /* Store as system.posix_acl_access xattr */
    return vfs_setxattr(path, ACL_XATTR_ACCESS, buf, ret);
}

/* ── Public API: get ACL from xattr ─────────────────────────────────── */

int posix_acl_get(const char *path, struct posix_acl *acl)
{
    if (!path || !acl)
        return -EINVAL;

    /* Read the system.posix_acl_access xattr */
    uint8_t buf[256];
    int ret = vfs_getxattr(path, ACL_XATTR_ACCESS, buf, sizeof(buf));
    if (ret < 0) {
        /* If no ACL exists, return a default "allow all" ACL */
        if (ret == -ENOENT || ret == -ENODATA) {
            acl->count = 0;
            return 0;
        }
        return ret;
    }

    return posix_acl_from_xattr(buf, ret, acl);
}

/* ── VFS-level ACL wrappers ────────────────────────────────────────── */

int vfs_set_acl(const char *path, struct posix_acl *acl)
{
    return posix_acl_set(path, acl);
}

int vfs_get_acl(const char *path, struct posix_acl *acl)
{
    return posix_acl_get(path, acl);
}

/* ── ACL-based permission check (S150) ───────────────────────────────
 *
 * Algorithm per POSIX 1003.1e draft:
 *   1. If uid == file_owner:
 *        use ACL_USER_OBJ permissions (no mask AND)
 *   2. Else if uid matches an ACL_USER entry:
 *        check (entry.perm & mask_perm) against required
 *   3. Else if gid == file_group or matches an ACL_GROUP entry:
 *        check (group_entry.perm & mask_perm) against required
 *   4. Else:
 *        check ACL_OTHER permissions
 *
 * Returns 0 (allowed) or -EACCES (denied).
 */

int posix_acl_permission(const char *path, uint16_t uid, uint16_t gid,
                          uint16_t req_perm)
{
    /* Root can do anything */
    if (uid == 0)
        return 0;

    struct posix_acl acl;
    int ret = posix_acl_get(path, &acl);
    if (ret < 0)
        return ret;

    /* No ACL — fall back to traditional permission check */
    if (acl.count == 0)
        return -ENOENT;  /* Signal: caller should check traditional mode */

    /* Parse ACL entries */
    uint16_t owner_perm = 0;
    uint16_t user_perm = 0;
    int user_match = 0;
    uint16_t group_perm = 0;
    int group_match = 0;
    uint16_t other_perm = 0;
    uint16_t mask_perm = 7;  /* default: no restriction */
    int has_mask = 0;

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
        }
    }

    /* Access check in order */
    if (acl.entries[0].tag == ACL_USER_OBJ) {
        /* Step 1: file owner */
        if (owner_perm & req_perm)
            return 0;
        return -EACCES;
    }

    /* Check matching user entries */
    if (user_match) {
        uint16_t effective = has_mask ? (user_perm & mask_perm) : user_perm;
        if (effective & req_perm)
            return 0;
        return -EACCES;
    }

    /* Check matching group entries */
    if (group_match) {
        uint16_t effective = has_mask ? (group_perm & mask_perm) : group_perm;
        if (effective & req_perm)
            return 0;
        return -EACCES;
    }

    /* Check other */
    if (other_perm & req_perm)
        return 0;

    return -EACCES;
}

/* ── VFS generic_permission (S150) ────────────────────────────────────
 *
 * Called from VFS operations to check access to an inode.
 * Checks ACL first, then falls back to traditional mode bits.
 *
 * @path:      absolute path to the inode
 * @uid:       requesting user's UID
 * @gid:       requesting user's GID
 * @mode:      file's mode bits (from inode)
 * @file_uid:  file owner's UID
 * @file_gid:  file owner's GID
 * @op:        operation being performed (4=read, 2=write, 1=execute)
 *
 * Returns 0 on success (allowed), -EACCES on denial.
 */

int generic_permission(const char *path, uint16_t uid, uint16_t gid,
                        uint16_t mode, uint16_t file_uid, uint16_t file_gid,
                        uint16_t op)
{
    /* Root always allowed */
    if (uid == 0)
        return 0;

    /* First try ACL */
    struct posix_acl acl;
    int ret = posix_acl_get(path, &acl);
    if (ret == 0 && acl.count > 0) {
        ret = posix_acl_permission(path, uid, gid, op);
        if (ret == 0 || ret == -EACCES)
            return ret;
        /* On other errors, fall through to traditional check */
    }

    /* Traditional Unix permission check */
    uint16_t perm_bits;

    if (uid == file_uid) {
        /* Owner: check owner bits */
        perm_bits = (mode >> 6) & 7;
    } else if (gid == file_gid) {
        /* Group: check group bits */
        perm_bits = (mode >> 3) & 7;
    } else {
        /* Other: check other bits */
        perm_bits = mode & 7;
    }

    return (perm_bits & op) ? 0 : -EACCES;
}

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Stub: posix_acl_create ────────────────────────── */
int posix_acl_create(const char *path, uint16_t mode, struct posix_acl *acl)
{
    (void)path;
    (void)mode;
    (void)acl;
    kprintf("[posix_acl] posix_acl_create: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: posix_acl_chmod ─────────────────────────── */
int posix_acl_chmod(const char *path, uint16_t mode)
{
    (void)path;
    (void)mode;
    kprintf("[posix_acl] posix_acl_chmod: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: posix_acl_default ───────────────────────── */
int posix_acl_default(const char *path, struct posix_acl *acl)
{
    (void)path;
    (void)acl;
    kprintf("[posix_acl] posix_acl_default: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: posix_acl_access ────────────────────────── */
int posix_acl_access(const char *path, struct posix_acl *acl)
{
    (void)path;
    (void)acl;
    kprintf("[posix_acl] posix_acl_access: not yet implemented\n");
    return -ENOSYS;
}
