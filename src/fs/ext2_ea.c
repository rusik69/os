/*
 * src/fs/ext2_ea.c — Ext2 extended attribute block operations.
 *
 * Implements reading, writing, listing, and removing extended attributes
 * stored in the on-disk EA block format for ext2 filesystems.
 *
 * Extended attributes are stored in a separate block pointed to by the
 * i_file_acl field of the ext2 inode.  The EA block format:
 *
 *   [ext2_ext_attr_header]   — 32-byte header with magic 0xEA020000
 *   [entry1][name1][pad]     — packed at the start of the data area
 *   [entry2][name2][pad]
 *   ...
 *   [free space]
 *   ...
 *   [pad][value2]            — values packed backwards from block end
 *   [pad][value1]
 *
 * Entry names are stored WITHOUT the namespace prefix; the namespace is
 * encoded in the e_name_index field (user=0, system=1, security=2, ...).
 *
 * Supported namespaces: user, system, security.
 */

#define KERNEL_INTERNAL
#include "ext2.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"

/* ── Namespace helpers ─────────────────────────────────────────────── */

/* Convert a namespace prefix string to an EXT2_XATTR_INDEX_* value.
 * Returns the index on success, -1 if unknown. */
static int ext2_ea_name_to_index(const char *prefix)
{
    if (!prefix) return -1;
    if (strcmp(prefix, "user.") == 0)
        return EXT2_XATTR_INDEX_USER;
    if (strcmp(prefix, "system.") == 0)
        return EXT2_XATTR_INDEX_SYSTEM;
    if (strcmp(prefix, "security.") == 0)
        return EXT2_XATTR_INDEX_SECURITY;
    if (strcmp(prefix, "trusted.") == 0)
        return EXT2_XATTR_INDEX_TRUSTED;
    return -1;
}

/* Convert an EXT2_XATTR_INDEX_* value to its namespace prefix string.
 * Returns a pointer to a static string, or NULL for unknown indices. */
static const char *ext2_ea_index_to_prefix(int index)
{
    switch (index) {
    case EXT2_XATTR_INDEX_USER:
        return "user.";
    case EXT2_XATTR_INDEX_SYSTEM:
        return "system.";
    case EXT2_XATTR_INDEX_SECURITY:
        return "security.";
    case EXT2_XATTR_INDEX_TRUSTED:
        return "trusted.";
    default:
        return NULL;
    }
}

/* Compute a simple DJB hash for an EA entry name (namespace index + name). */
static uint32_t ext2_ea_hash(uint8_t name_index, const char *name, uint8_t name_len)
{
    uint32_t hash = (uint32_t)name_index;
    for (uint8_t i = 0; i < name_len; i++)
        hash = (hash << 5) - hash + (uint8_t)name[i];
    return hash;
}

/* ── EA block iteration helper ─────────────────────────────────────── */

/* Locate an entry by name within an EA block.
 * @block: pointer to the EA block data
 * @block_size: size of the block in bytes
 * @name_index: namespace index to match (EXT2_XATTR_INDEX_*)
 * @short_name: the name (without namespace prefix)
 * @short_len: length of short_name
 * @out_entry: set to the entry pointer if found (optional)
 * @out_prev: set to the previous entry pointer (for removal) (optional)
 * Returns 0 if found, -ENODATA if not found, negative errno on error. */
static int ext2_ea_find_entry(const uint8_t *block, uint32_t block_size,
                               uint8_t name_index,
                               const char *short_name, uint8_t short_len,
                               struct ext2_ext_attr_entry **out_entry,
                               struct ext2_ext_attr_entry **out_prev)
{
    const struct ext2_ext_attr_header *hdr =
        (const struct ext2_ext_attr_header *)block;

    if (block_size < sizeof(struct ext2_ext_attr_header))
        return -EFSCORRUPTED;
    if (hdr->h_magic != EXT2_EXT_ATTR_MAGIC)
        return -EFSCORRUPTED;

    struct ext2_ext_attr_entry *entry =
        (struct ext2_ext_attr_entry *)(uintptr_t)(block + sizeof(*hdr));
    struct ext2_ext_attr_entry *prev = NULL;

    while ((uint8_t *)entry + sizeof(*entry) <= block + block_size) {
        /* Check if the entry fits */
        uint32_t entry_len = EXT2_EXT_ATTR_ENTRY_LEN(entry->e_name_len);
        if ((uint8_t *)entry + entry_len > block + block_size)
            break;

        /* Check for end marker (empty entry with zero name length) */
        if (entry->e_name_len == 0 && entry->e_name_index == 0)
            break;

        /* Check if this entry matches */
        if (entry->e_name_index == name_index &&
            entry->e_name_len == short_len) {
            const uint8_t *ename = (const uint8_t *)(entry + 1);
            if (memcmp(ename, short_name, short_len) == 0) {
                if (out_entry)
                    *out_entry = entry;
                if (out_prev)
                    *out_prev = prev;
                return 0;
            }
        }

        prev = entry;
        entry = EXT2_EXT_ATTR_NEXT(entry);
    }

    return -ENODATA;
}

/* ── ext2_ea_get ───────────────────────────────────────────────────── */

int ext2_ea_get(struct ext2_priv *ep, uint32_t ino,
                struct ext2_inode *inode,
                const char *name, void *buf, size_t size)
{
    (void)ino;

    if (!ep || !inode || !name || !buf)
        return -EINVAL;

    /* No EA block — no attributes */
    if (inode->i_file_acl == 0)
        return -ENODATA;

    uint32_t block_size = ep->block_size;
    if (block_size > 4096)
        return -EINVAL;

    /* Read the EA block */
    uint8_t ea_block[4096];
    memset(ea_block, 0, sizeof(ea_block));
    int ret = ext2_read_block(ep, inode->i_file_acl, ea_block);
    if (ret < 0)
        return ret;

    /* Parse namespace from full name (e.g. "user.myattr") */
    const char *dot = strchr(name, '.');
    if (!dot || dot == name)
        return -EINVAL;

    /* Extract namespace prefix */
    char prefix[16];
    size_t prefix_len = (size_t)(dot - name);
    if (prefix_len >= sizeof(prefix))
        return -EINVAL;
    memcpy(prefix, name, prefix_len);
    prefix[prefix_len] = '\0';

    int name_index = ext2_ea_name_to_index(prefix);
    if (name_index < 0)
        return -EINVAL;

    const char *short_name = dot + 1;
    uint8_t short_len = (uint8_t)strlen(short_name);
    if (short_len == 0)
        return -EINVAL;

    /* Find the entry */
    struct ext2_ext_attr_entry *entry = NULL;
    ret = ext2_ea_find_entry(ea_block, block_size,
                              (uint8_t)name_index,
                              short_name, short_len,
                              &entry, NULL);
    if (ret < 0)
        return ret;

    /* Check that the value location is valid */
    if (entry->e_value_block != 0)
        return -ENODATA;  /* value in another block — not supported */

    uint32_t value_offs = entry->e_value_offs;
    uint32_t value_size = entry->e_value_size;

    if (value_offs + value_size > block_size)
        return -EFSCORRUPTED;

    /* Copy value to output buffer */
    size_t copy_sz = (size < (size_t)value_size) ? size : (size_t)value_size;
    memcpy(buf, ea_block + value_offs, copy_sz);

    return (int)copy_sz;
}

/* ── ext2_ea_set ───────────────────────────────────────────────────── */

int ext2_ea_set(struct ext2_priv *ep, uint32_t ino,
                struct ext2_inode *inode,
                const char *name, const void *value, size_t size)
{
    if (!ep || !inode || !name || !value)
        return -EINVAL;

    uint32_t block_size = ep->block_size;
    if (block_size > 4096)
        return -EINVAL;

    /* Parse namespace from full name */
    const char *dot = strchr(name, '.');
    if (!dot || dot == name)
        return -EINVAL;

    char prefix[16];
    size_t prefix_len = (size_t)(dot - name);
    if (prefix_len >= sizeof(prefix))
        return -EINVAL;
    memcpy(prefix, name, prefix_len);
    prefix[prefix_len] = '\0';

    int name_index = ext2_ea_name_to_index(prefix);
    if (name_index < 0)
        return -EINVAL;

    const char *short_name = dot + 1;
    uint8_t short_len = (uint8_t)strlen(short_name);
    if (short_len == 0)
        return -EINVAL;

    if (size > block_size - sizeof(struct ext2_ext_attr_header))
        return -ENOSPC;

    /* Buffer for the EA block */
    uint8_t ea_block[4096];
    memset(ea_block, 0, sizeof(ea_block));

    uint32_t ea_block_num = inode->i_file_acl;
    int block_allocated = 0;

    if (ea_block_num != 0) {
        /* Read existing EA block */
        int ret = ext2_read_block(ep, ea_block_num, ea_block);
        if (ret < 0)
            return ret;
    } else {
        /* No EA block yet — create a fresh one with header */
        struct ext2_ext_attr_header *hdr =
            (struct ext2_ext_attr_header *)ea_block;
        hdr->h_magic = EXT2_EXT_ATTR_MAGIC;
        hdr->h_refcount = 1;
        hdr->h_blocks = 1;
        hdr->h_hash = 0;
        hdr->h_checksum = 0;
    }

    struct ext2_ext_attr_header *hdr =
        (struct ext2_ext_attr_header *)ea_block;

    /* Validate existing header */
    if (ea_block_num != 0 && hdr->h_magic != EXT2_EXT_ATTR_MAGIC)
        return -EFSCORRUPTED;

    /* Check if entry already exists — if so, replace value in-place */
    {
        struct ext2_ext_attr_entry *entry = NULL;
        int ret = ext2_ea_find_entry(ea_block, block_size,
                                      (uint8_t)name_index,
                                      short_name, short_len,
                                      &entry, NULL);
        if (ret == 0 && entry) {
            /* Entry exists — check if the new value fits in same slot */
            uint32_t aligned_new = EXT2_EXT_ATTR_ALIGN((uint32_t)size);
            uint32_t aligned_old = EXT2_EXT_ATTR_ALIGN(entry->e_value_size);
            uint32_t old_offs = entry->e_value_offs;

            if (aligned_new <= aligned_old) {
                /* Same or smaller — fit in place */
                memcpy(ea_block + old_offs, value, size);
                if (aligned_new < aligned_old) {
                    /* Zero the remaining bytes */
                    memset(ea_block + old_offs + size, 0,
                           aligned_old - size);
                }
                entry->e_value_size = (uint32_t)size;
                entry->e_hash = ext2_ea_hash((uint8_t)name_index,
                                              short_name, short_len);

                /* Write the block back */
                ret = ext2_write_block(ep, ea_block_num, ea_block);
                if (ret < 0)
                    return ret;
                return 0;
            }

            /* New value is larger — need to rebuild the block.
             * We'll fall through to the add-new-entry path below,
             * after removing the old entry from consideration. */
        }
    }

    /* ── Rebuild the EA block ──────────────────────────────────────
     *
     * We need to pack all existing entries (excluding the one we're
     * replacing if it exists) plus the new entry, with all values
     * packed from the end of the block.
     *
     * For simplicity, we rebuild the entire block from scratch.
     * This is not optimal for performance but is correct for all
     * cases. */

    /* Collect all entries from the old block */
    struct {
        struct ext2_ext_attr_entry *entry;
        uint32_t entry_len;     /* total bytes for entry + name (aligned) */
        uint32_t value_offs;    /* offset of value in old block */
        uint32_t value_size;    /* size of value */
        uint32_t value_alen;    /* aligned value size */
        int keep;               /* 1 = keep this entry */
    } entries[32];
    int num_entries = 0;
    int found_old = 0;

    if (ea_block_num != 0 && hdr->h_magic == EXT2_EXT_ATTR_MAGIC) {
        struct ext2_ext_attr_entry *entry =
            (struct ext2_ext_attr_entry *)(ea_block + sizeof(*hdr));

        while ((uint8_t *)entry + sizeof(*entry) <= ea_block + block_size) {
            if (entry->e_name_len == 0 && entry->e_name_index == 0)
                break;

            uint32_t elen = EXT2_EXT_ATTR_ENTRY_LEN(entry->e_name_len);
            if ((uint8_t *)entry + elen > ea_block + block_size)
                break;

            /* Check if this is the entry we're replacing */
            int is_same = (entry->e_name_index == (uint8_t)name_index &&
                           entry->e_name_len == short_len);
            if (is_same) {
                const uint8_t *ename = (const uint8_t *)(entry + 1);
                is_same = (memcmp(ename, short_name, short_len) == 0);
            }

            if (is_same) {
                found_old = 1;
                /* Skip — we'll add the new value instead */
            } else if (num_entries < 32) {
                entries[num_entries].entry = entry;
                entries[num_entries].entry_len = elen;
                entries[num_entries].value_offs = entry->e_value_offs;
                entries[num_entries].value_size = entry->e_value_size;
                entries[num_entries].value_alen =
                    EXT2_EXT_ATTR_ALIGN(entry->e_value_size);
                entries[num_entries].keep = 1;
                num_entries++;
            }

            entry = EXT2_EXT_ATTR_NEXT(entry);
        }
    }

    /* Add the new entry */
    uint32_t new_entry_len = EXT2_EXT_ATTR_ENTRY_LEN(short_len);
    uint32_t new_value_alen = EXT2_EXT_ATTR_ALIGN((uint32_t)size);

    /* Calculate if there's enough space */
    uint32_t total_entry_space = sizeof(*hdr);
    for (int i = 0; i < num_entries; i++)
        total_entry_space += entries[i].entry_len;
    total_entry_space += new_entry_len;
    total_entry_space = EXT2_EXT_ATTR_ALIGN(total_entry_space);

    uint32_t total_value_space = new_value_alen;
    for (int i = 0; i < num_entries; i++)
        total_value_space += entries[i].value_alen;

    if (total_entry_space + total_value_space > block_size)
        return -ENOSPC;

    /* ── Allocate block if needed ────────────────────────────────── */
    if (ea_block_num == 0) {
        int ret = ext2_alloc_block(ep, &ea_block_num);
        if (ret < 0)
            return ret;
        block_allocated = 1;
    }

    /* ── Rebuild the block ───────────────────────────────────────── */
    memset(ea_block, 0, block_size);

    struct ext2_ext_attr_header *new_hdr =
        (struct ext2_ext_attr_header *)ea_block;
    new_hdr->h_magic = EXT2_EXT_ATTR_MAGIC;
    new_hdr->h_refcount = 1;
    new_hdr->h_blocks = 1;
    new_hdr->h_hash = 0;
    new_hdr->h_checksum = 0;

    /* Pack entries at the front */
    uint32_t entry_pos = sizeof(*new_hdr);
    uint32_t value_pos = block_size;

    for (int i = 0; i < num_entries; i++) {
        if (!entries[i].keep)
            continue;

        struct ext2_ext_attr_entry *new_entry =
            (struct ext2_ext_attr_entry *)(ea_block + entry_pos);
        new_entry->e_name_len = entries[i].entry->e_name_len;
        new_entry->e_name_index = entries[i].entry->e_name_index;
        new_entry->e_value_block = 0;
        new_entry->e_value_size = entries[i].value_size;

        /* Copy the name */
        const uint8_t *old_name = (const uint8_t *)(entries[i].entry + 1);
        memcpy(new_entry + 1, old_name, new_entry->e_name_len);

        /* Copy the value to the end of the block */
        value_pos -= entries[i].value_alen;
        new_entry->e_value_offs = (uint16_t)value_pos;
        memcpy(ea_block + value_pos,
               ea_block + entries[i].value_offs,
               entries[i].value_size);

        /* Compute hash */
        new_entry->e_hash = ext2_ea_hash(
            new_entry->e_name_index,
            (const char *)(new_entry + 1),
            new_entry->e_name_len);

        entry_pos += entries[i].entry_len;
    }

    /* Add the new entry */
    {
        struct ext2_ext_attr_entry *new_entry =
            (struct ext2_ext_attr_entry *)(ea_block + entry_pos);
        new_entry->e_name_len = short_len;
        new_entry->e_name_index = (uint8_t)name_index;
        new_entry->e_value_block = 0;
        new_entry->e_value_size = (uint32_t)size;

        /* Copy the short name */
        memcpy(new_entry + 1, short_name, short_len);

        /* Place the value at the end */
        value_pos -= new_value_alen;
        new_entry->e_value_offs = (uint16_t)value_pos;
        memset(ea_block + value_pos, 0, new_value_alen);
        memcpy(ea_block + value_pos, value, size);

        /* Compute hash */
        new_entry->e_hash = ext2_ea_hash(
            (uint8_t)name_index, short_name, short_len);

        entry_pos += new_entry_len;
    }

    /* Write the block */
    int ret = ext2_write_block(ep, ea_block_num, ea_block);
    if (ret < 0) {
        if (block_allocated) {
            ext2_free_block(ep, ea_block_num);
        }
        return ret;
    }

    /* Update inode */
    if (block_allocated) {
        inode->i_file_acl = ea_block_num;
    }

    return 0;
}

/* ── ext2_ea_list ──────────────────────────────────────────────────── */

int ext2_ea_list(struct ext2_priv *ep, struct ext2_inode *inode,
                 char *buf, size_t size)
{
    if (!ep || !inode)
        return -EINVAL;

    /* No EA block — nothing to list */
    if (inode->i_file_acl == 0)
        return 0;

    uint32_t block_size = ep->block_size;
    if (block_size > 4096)
        return -EINVAL;

    uint8_t ea_block[4096];
    memset(ea_block, 0, sizeof(ea_block));
    int ret = ext2_read_block(ep, inode->i_file_acl, ea_block);
    if (ret < 0)
        return ret;

    const struct ext2_ext_attr_header *hdr =
        (const struct ext2_ext_attr_header *)ea_block;
    if (hdr->h_magic != EXT2_EXT_ATTR_MAGIC)
        return -EFSCORRUPTED;

    size_t total = 0;
    struct ext2_ext_attr_entry *entry =
        (struct ext2_ext_attr_entry *)(ea_block + sizeof(*hdr));

    while ((uint8_t *)entry + sizeof(*entry) <= ea_block + block_size) {
        if (entry->e_name_len == 0 && entry->e_name_index == 0)
            break;

        uint32_t elen = EXT2_EXT_ATTR_ENTRY_LEN(entry->e_name_len);
        if ((uint8_t *)entry + elen > ea_block + block_size)
            break;

        /* Get the namespace prefix */
        const char *prefix = ext2_ea_index_to_prefix(
            (int)entry->e_name_index);
        if (!prefix) {
            entry = EXT2_EXT_ATTR_NEXT(entry);
            continue;
        }

        size_t prefix_len = strlen(prefix);
        size_t full_len = prefix_len + entry->e_name_len + 1; /* +1 for NUL */

        if (buf && total + full_len <= size) {
            memcpy(buf + total, prefix, prefix_len);
            memcpy(buf + total + prefix_len, entry + 1,
                   entry->e_name_len);
            buf[total + prefix_len + entry->e_name_len - 1] = '\0';
            /* Actually, we need a proper null terminator at the end.
             * Let's place it correctly: */
        }

        total += full_len;
        entry = EXT2_EXT_ATTR_NEXT(entry);
    }

    /* If buf is NULL, just return the total size needed */
    if (!buf)
        return (int)total;

    /* Now fill the buffer properly with null-terminated strings */
    size_t written = 0;
    entry = (struct ext2_ext_attr_entry *)(ea_block + sizeof(*hdr));
    while ((uint8_t *)entry + sizeof(*entry) <= ea_block + block_size &&
           written + 1 <= size) {
        if (entry->e_name_len == 0 && entry->e_name_index == 0)
            break;

        uint32_t elen = EXT2_EXT_ATTR_ENTRY_LEN(entry->e_name_len);
        if ((uint8_t *)entry + elen > ea_block + block_size)
            break;

        const char *prefix = ext2_ea_index_to_prefix(
            (int)entry->e_name_index);
        if (!prefix) {
            entry = EXT2_EXT_ATTR_NEXT(entry);
            continue;
        }

        size_t prefix_len = strlen(prefix);
        size_t name_len = entry->e_name_len;
        size_t full_len = prefix_len + name_len + 1;

        if (written + full_len > size)
            return -ERANGE;

        memcpy(buf + written, prefix, prefix_len);
        memcpy(buf + written + prefix_len, entry + 1, name_len);
        buf[written + prefix_len + name_len] = '\0';
        written += full_len;

        entry = EXT2_EXT_ATTR_NEXT(entry);
    }

    return (int)written;
}

/* ── ext2_ea_remove ────────────────────────────────────────────────── */

int ext2_ea_remove(struct ext2_priv *ep, uint32_t ino,
                   struct ext2_inode *inode,
                   const char *name)
{
    if (!ep || !inode || !name)
        return -EINVAL;

    uint32_t block_size = ep->block_size;
    if (block_size > 4096)
        return -EINVAL;

    /* No EA block — nothing to remove */
    if (inode->i_file_acl == 0)
        return -ENODATA;

    /* Parse namespace from full name */
    const char *dot = strchr(name, '.');
    if (!dot || dot == name)
        return -EINVAL;

    char prefix[16];
    size_t prefix_len = (size_t)(dot - name);
    if (prefix_len >= sizeof(prefix))
        return -EINVAL;
    memcpy(prefix, name, prefix_len);
    prefix[prefix_len] = '\0';

    int name_index = ext2_ea_name_to_index(prefix);
    if (name_index < 0)
        return -EINVAL;

    const char *short_name = dot + 1;
    uint8_t short_len = (uint8_t)strlen(short_name);
    if (short_len == 0)
        return -EINVAL;

    uint8_t ea_block[4096];
    memset(ea_block, 0, sizeof(ea_block));
    int ret = ext2_read_block(ep, inode->i_file_acl, ea_block);
    if (ret < 0)
        return ret;

    struct ext2_ext_attr_header *hdr =
        (struct ext2_ext_attr_header *)ea_block;
    if (hdr->h_magic != EXT2_EXT_ATTR_MAGIC)
        return -EFSCORRUPTED;

    /* Find the entry to remove */
    struct ext2_ext_attr_entry *entry = NULL;
    struct ext2_ext_attr_entry *prev = NULL;
    ret = ext2_ea_find_entry(ea_block, block_size,
                              (uint8_t)name_index,
                              short_name, short_len,
                              &entry, &prev);
    if (ret < 0)
        return ret;

    /* Count total entries */
    int total_entries = 0;
    struct ext2_ext_attr_entry *e =
        (struct ext2_ext_attr_entry *)(ea_block + sizeof(*hdr));
    while ((uint8_t *)e + sizeof(*e) <= ea_block + block_size) {
        if (e->e_name_len == 0 && e->e_name_index == 0)
            break;
        total_entries++;
        e = EXT2_EXT_ATTR_NEXT(e);
    }

    /* If this is the last entry, free the whole block */
    if (total_entries <= 1) {
        uint32_t block_num = inode->i_file_acl;
        inode->i_file_acl = 0;
        inode->i_blocks = 0;
        ext2_free_block(ep, block_num);
        return 0;
    }

    /* Rebuild the block without the removed entry */
    uint8_t new_block[4096];
    memset(new_block, 0, block_size);

    struct ext2_ext_attr_header *new_hdr =
        (struct ext2_ext_attr_header *)new_block;
    new_hdr->h_magic = EXT2_EXT_ATTR_MAGIC;
    new_hdr->h_refcount = 1;
    new_hdr->h_blocks = 1;
    new_hdr->h_hash = 0;
    new_hdr->h_checksum = 0;

    uint32_t entry_pos = sizeof(*new_hdr);
    uint32_t value_pos = block_size;

    e = (struct ext2_ext_attr_entry *)(ea_block + sizeof(*hdr));
    while ((uint8_t *)e + sizeof(*e) <= ea_block + block_size) {
        if (e->e_name_len == 0 && e->e_name_index == 0)
            break;

        uint32_t elen = EXT2_EXT_ATTR_ENTRY_LEN(e->e_name_len);
        if ((uint8_t *)e + elen > ea_block + block_size)
            break;

        /* Skip the entry we're removing */
        if (e == entry) {
            e = EXT2_EXT_ATTR_NEXT(e);
            continue;
        }

        struct ext2_ext_attr_entry *new_entry =
            (struct ext2_ext_attr_entry *)(new_block + entry_pos);
        new_entry->e_name_len = e->e_name_len;
        new_entry->e_name_index = e->e_name_index;
        new_entry->e_value_block = 0;
        new_entry->e_value_size = e->e_value_size;

        /* Copy name */
        memcpy(new_entry + 1, e + 1, e->e_name_len);

        /* Copy value to end */
        uint32_t val_alen = EXT2_EXT_ATTR_ALIGN(e->e_value_size);
        value_pos -= val_alen;
        new_entry->e_value_offs = (uint16_t)value_pos;
        if (e->e_value_size > 0)
            memcpy(new_block + value_pos,
                   ea_block + e->e_value_offs,
                   e->e_value_size);

        new_entry->e_hash = ext2_ea_hash(
            e->e_name_index,
            (const char *)(e + 1),
            e->e_name_len);

        entry_pos += elen;
        e = EXT2_EXT_ATTR_NEXT(e);
    }

    /* Write the new block */
    ret = ext2_write_block(ep, inode->i_file_acl, new_block);
    if (ret < 0)
        return ret;

    return 0;
}
