/*
 * mnt_namespace.c — Mount namespace (Item 112)
 *
 * Per-process mount namespace with copy-on-clone semantics.
 * The root namespace wraps the global mount table; CLONE_NEWNS
 * produces an independent copy.
 *
 * Each namespace carries its own struct vfs_mount array so that
 * mount and umount operations are isolated.  Reference counting
 * ensures that a namespace is freed when the last process using
 * it exits or unshares.
 */

#include "mnt_namespace.h"

#include "export.h"
#include "heap.h" /* kmalloc / kfree */
#include "printf.h"
#include "process.h" /* process_get_current() */
#include "string.h"

/* ── Root namespace singleton ─────────────────────────────────────── */

/* When the current process has no explicit mnt_ns (NULL), operations
 * fall back to the global mount table declared in vfs.c.  We anchor
 * the root namespace here by wrapping that table.  For namespaces
 * created via CLONE_NEWNS, we allocate a fresh mnt_namespace. */

static struct mnt_namespace *root_ns = NULL;

/* ── Global registry for cross-namespace propagation ────────────── */
/* Every live mount namespace is linked here so that MS_SHARED mount
 * and umount events can be propagated to peer namespaces. */
static struct mnt_namespace *all_ns = NULL;
static int next_peer_group = 1; /* allocated peer-group ids */

/* Forward decl: shared-mount umount events also propagate to peers. */
static void mnt_propagate_umount(struct mnt_namespace *src, const char *mountpoint);

static void mnt_ns_register(struct mnt_namespace *ns) {
    ns->next_all = all_ns;
    all_ns = ns;
}

static void mnt_ns_unregister(struct mnt_namespace *ns) {
    struct mnt_namespace **p = &all_ns;
    while (*p) {
        if (*p == ns) {
            *p = ns->next_all;
            return;
        }
        p = &(*p)->next_all;
    }
}

/* ── Internal helpers ─────────────────────────────────────────────── */

/* Deep-copy a vfs_mount entry (strings and fields). */
static void copy_mount_entry(struct vfs_mount *dst, const struct vfs_mount *src) {
    memcpy(dst->mountpoint, src->mountpoint, sizeof(dst->mountpoint));
    dst->ops = src->ops;
    dst->priv = src->priv;
    dst->flags = src->flags;
    dst->is_bind = src->is_bind;
    memcpy(dst->bind_source, src->bind_source, sizeof(dst->bind_source));
    dst->journal_active = src->journal_active;
    dst->journal_seq = src->journal_seq;
    dst->encrypted = src->encrypted;
    memcpy(dst->enc_key, src->enc_key, sizeof(dst->enc_key));
    dst->prop = src->prop;
    dst->peer_group = src->peer_group;
}

/* ── API implementation ────────────────────────────────────────────── */

struct mnt_namespace *mnt_ns_create_root(void) {
    /* Only the root namespace wraps the global mount table.
     * We store a pointer to the initial global data so that
     * mnt_ns_current() can return this anchor. */
    extern struct vfs_mount mounts[VFS_MAX_MOUNTS];
    extern int num_mounts;

    if (root_ns)
        return root_ns;

    root_ns = (struct mnt_namespace *)kmalloc(sizeof(struct mnt_namespace));
    if (!root_ns) {
        kprintf("[MNT_NS] ERROR: cannot allocate root namespace\n");
        return NULL;
    }

    memcpy(root_ns->mounts, mounts, sizeof(root_ns->mounts));
    root_ns->num_mounts = num_mounts;
    root_ns->refcount = 1;
    mnt_ns_register(root_ns);

    kprintf("[MNT_NS] Root namespace created (%d mounts)\n", root_ns->num_mounts);
    return root_ns;
}

struct mnt_namespace *mnt_ns_copy(const struct mnt_namespace *src) {
    if (!src) {
        src = root_ns;
        if (!src)
            return NULL;
    }

    struct mnt_namespace *ns = (struct mnt_namespace *)kmalloc(sizeof(struct mnt_namespace));
    if (!ns) {
        kprintf("[MNT_NS] ERROR: cannot allocate namespace\n");
        return NULL;
    }

    /* Deep-copy the mount table */
    ns->num_mounts = src->num_mounts;
    for (int i = 0; i < src->num_mounts; i++) {
        copy_mount_entry(&ns->mounts[i], &src->mounts[i]);
    }
    /* Zero out unused slots */
    for (int i = src->num_mounts; i < VFS_MAX_MOUNTS; i++) {
        memset(&ns->mounts[i], 0, sizeof(struct vfs_mount));
    }

    ns->refcount = 1;
    mnt_ns_register(ns);

    kprintf("[MNT_NS] Copied namespace (%d mounts)\n", ns->num_mounts);
    return ns;
}

void mnt_ns_put(struct mnt_namespace *ns) {
    if (!ns || ns == root_ns)
        return; /* root namespace lives forever */

    if (ns->refcount <= 0) {
        kprintf("[MNT_NS] WARNING: put on namespace with refcount=%d\n", ns->refcount);
        return;
    }

    ns->refcount--;
    if (ns->refcount == 0) {
        kprintf("[MNT_NS] Freeing namespace\n");
        mnt_ns_unregister(ns);
        kfree(ns);
    }
}

struct mnt_namespace *mnt_ns_get(struct mnt_namespace *ns) {
    if (!ns) {
        /* Return the root namespace if current process has none */
        return root_ns;
    }
    ns->refcount++;
    return ns;
}

struct mnt_namespace *mnt_ns_current(void) {
    struct process *cur = process_get_current();
    if (cur && cur->mnt_ns)
        return cur->mnt_ns;

    return root_ns;
}

int mnt_ns_mount(struct mnt_namespace *ns, const char *mountpoint, struct vfs_ops *ops, void *priv,
                 int flags) {
    if (!ns || !mountpoint) {
        extern int vfs_mount_ex(const char *, const struct vfs_ops *, void *, int);
        return vfs_mount_ex(mountpoint, ops, priv, flags);
    }

    if (ns->num_mounts >= VFS_MAX_MOUNTS)
        return -1;

    struct vfs_mount *m = &ns->mounts[ns->num_mounts];
    size_t mlen = strlen(mountpoint);
    if (mlen >= sizeof(m->mountpoint))
        mlen = sizeof(m->mountpoint) - 1;

    memcpy(m->mountpoint, mountpoint, mlen);
    m->mountpoint[mlen] = '\0';
    m->ops = ops;
    m->priv = priv;
    m->flags = flags;
    m->is_bind = 0;
    m->bind_source[0] = '\0';

    /* Inherit propagation mode from the mount we are attaching onto.
     * A mount made on an MS_SHARED mountpoint is shared and propagates;
     * on an MS_SLAVE mountpoint it is a slave (no sending) but still
     * receives events; otherwise it starts private. */
    struct vfs_mount *pre = mnt_ns_resolve(ns, mountpoint);
    if (pre && (pre->prop == MNT_SHARED || pre->prop == MNT_SLAVE)) {
        m->prop = pre->prop;
        m->peer_group = pre->peer_group;
    } else {
        m->prop = MNT_PRIVATE;
        m->peer_group = 0;
    }

    /* Handle MS_BIND: duplicate source entry */
    if (flags & 0x40) { /* MS_BIND */
        struct vfs_mount *src = mnt_ns_resolve(ns, mountpoint);
        if (src) {
            m->ops = src->ops;
            m->priv = src->priv;
            m->is_bind = 1;
            strncpy(m->bind_source, src->mountpoint, sizeof(m->bind_source) - 1);
            m->bind_source[sizeof(m->bind_source) - 1] = '\0';
        }
    }

    ns->num_mounts++;

    /* A shared mount event propagates to peer namespaces. */
    if (m->prop == MNT_SHARED)
        mnt_propagate_mount(ns, m);

    return 0;
}

int mnt_ns_umount(struct mnt_namespace *ns, const char *mountpoint) {
    if (!ns || !mountpoint)
        return -1;

    for (int i = 0; i < ns->num_mounts; i++) {
        if (strcmp(ns->mounts[i].mountpoint, mountpoint) == 0) {
            int was_shared = (ns->mounts[i].prop == MNT_SHARED);
            /* Shift remaining entries left */
            int remaining = ns->num_mounts - i - 1;
            if (remaining > 0) {
                memmove(&ns->mounts[i], &ns->mounts[i + 1],
                        (size_t)remaining * sizeof(struct vfs_mount));
            }
            ns->num_mounts--;
            /* An unmount of a shared mount propagates to peers. */
            if (was_shared)
                mnt_propagate_umount(ns, mountpoint);
            return 0;
        }
    }
    return -1; /* not found */
}

struct vfs_mount *mnt_ns_resolve(struct mnt_namespace *ns, const char *path) {
    if (!ns || !path)
        return NULL;

    struct vfs_mount *best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < ns->num_mounts; i++) {
        size_t mlen = strlen(ns->mounts[i].mountpoint);

        /* Root mount: "/" has length 1 */
        if (mlen == 1 && ns->mounts[i].mountpoint[0] == '/') {
            if (best_len < 1) {
                best = &ns->mounts[i];
                best_len = 1;
            }
            continue;
        }

        if (strncmp(path, ns->mounts[i].mountpoint, mlen) == 0) {
            /* Check that path continues with '/' or is exactly the mountpoint */
            if (path[mlen] == '/' || path[mlen] == '\0') {
                if (mlen > best_len) {
                    best = &ns->mounts[i];
                    best_len = mlen;
                }
            }
        }
    }

    return best;
}

int mnt_ns_list_mounts(struct mnt_namespace *ns, char mount_list[][64], int max) {
    if (!ns)
        ns = root_ns;
    if (!ns)
        return 0;

    int n = ns->num_mounts < max ? ns->num_mounts : max;
    for (int i = 0; i < n; i++) {
        strncpy(mount_list[i], ns->mounts[i].mountpoint, 63);
        mount_list[i][63] = '\0';
    }
    return n;
}

void mnt_ns_sync(struct mnt_namespace *ns) {
    if (!ns)
        ns = root_ns;
    if (!ns)
        return;

    for (int i = 0; i < ns->num_mounts; i++) {
        if (ns->mounts[i].ops && ns->mounts[i].ops->flush) {
            ns->mounts[i].ops->flush(ns->mounts[i].priv);
        }
    }
}

EXPORT_SYMBOL(mnt_ns_current);
EXPORT_SYMBOL(mnt_ns_mount);
EXPORT_SYMBOL(mnt_ns_resolve);
EXPORT_SYMBOL(mnt_ns_list_mounts);

struct mnt_namespace *mnt_ns_root(void) {
    return root_ns;
}

/* ── Mount propagation (shared/slave/private/unbindable) ─────────── */

int mnt_prop_valid(int prop) {
    if (prop < MNT_PRIVATE || prop > MNT_UNBINDABLE)
        return MNT_PRIVATE;
    return prop;
}

/* Find a peer mount in @other that shares @mountpoint and is willing to
 * receive propagation (MNT_SHARED or MNT_SLAVE).  Returns the entry. */
static struct vfs_mount *find_peer_receiver(struct mnt_namespace *other, const char *mountpoint) {
    for (int i = 0; i < other->num_mounts; i++) {
        struct vfs_mount *om = &other->mounts[i];
        if (strcmp(om->mountpoint, mountpoint) != 0)
            continue;
        if (om->prop == MNT_SHARED || om->prop == MNT_SLAVE)
            return om;
    }
    return NULL;
}

void mnt_propagate_mount(struct mnt_namespace *src, struct vfs_mount *m) {
    struct mnt_namespace *other;
    for (other = all_ns; other; other = other->next_all) {
        if (other == src)
            continue;
        struct vfs_mount *recv = find_peer_receiver(other, m->mountpoint);
        if (!recv)
            continue;
        if (other->num_mounts >= VFS_MAX_MOUNTS) {
            kprintf("[MNT_NS] propagate: peer mount table full\n");
            continue;
        }
        /* Copy the propagated mount into the peer as a bind-like entry,
         * retaining the receiver's own propagation mode. */
        struct vfs_mount *copy = &other->mounts[other->num_mounts++];
        copy_mount_entry(copy, m);
        copy->is_bind = 1;
        copy->prop = recv->prop;
        copy->peer_group = recv->peer_group;
    }
}

static void mnt_propagate_umount(struct mnt_namespace *src, const char *mountpoint) {
    struct mnt_namespace *other;
    for (other = all_ns; other; other = other->next_all) {
        if (other == src)
            continue;
        for (int i = 0; i < other->num_mounts; i++) {
            if (strcmp(other->mounts[i].mountpoint, mountpoint) != 0)
                continue;
            if (other->mounts[i].prop != MNT_SHARED && other->mounts[i].prop != MNT_SLAVE)
                continue;
            int remaining = other->num_mounts - i - 1;
            if (remaining > 0) {
                memmove(&other->mounts[i], &other->mounts[i + 1],
                        (size_t)remaining * sizeof(struct vfs_mount));
            }
            other->num_mounts--;
            break;
        }
    }
}

int mnt_ns_setprop(struct mnt_namespace *ns, const char *path, int prop) {
    if (!ns)
        ns = root_ns;
    if (!ns || !path)
        return -1;

    struct vfs_mount *m = mnt_ns_resolve(ns, path);
    if (!m)
        return -1;

    m->prop = mnt_prop_valid(prop);

    if (m->prop == MNT_SHARED) {
        /* Give the mount a peer-group id so its peers can be correlated. */
        if (m->peer_group == 0)
            m->peer_group = next_peer_group++;
    } else {
        m->peer_group = 0;
    }

    kprintf("[MNT_NS] setprop %s -> %s\n", path,
            m->prop == MNT_PRIVATE  ? "private"
            : m->prop == MNT_SHARED ? "shared"
            : m->prop == MNT_SLAVE  ? "slave"
                                    : "unbindable");
    return 0;
}

int mnt_ns_getprop(struct mnt_namespace *ns, const char *path) {
    if (!ns)
        ns = root_ns;
    if (!ns || !path)
        return -1;
    struct vfs_mount *m = mnt_ns_resolve(ns, path);
    if (!m)
        return -1;
    return mnt_prop_valid(m->prop);
}

EXPORT_SYMBOL(mnt_ns_setprop);
EXPORT_SYMBOL(mnt_ns_getprop);

/* ── Stub: mnt_ns_create ─────────────────────────────── */
static int mnt_ns_create(void *parent) {
    (void)parent;
    kprintf("[mnt_ns] mnt_ns_create: not yet implemented\n");
    return 0;
}
/* ── Stub: mnt_ns_delete ─────────────────────────────── */
static int mnt_ns_delete(void *ns) {
    (void)ns;
    kprintf("[mnt_ns] mnt_ns_delete: not yet implemented\n");
    return 0;
}
