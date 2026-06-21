/*
 * cgroup_namespace.c — Cgroup namespace isolation (Item 117)
 *
 * Implements per-process cgroup namespaces.  Each namespace has a root
 * cgroup path; processes inside the namespace see their cgroup path
 * relative to this root.  When a process is created with CLONE_NEWCGROUP,
 * its current cgroup path at creation time becomes the namespace root.
 *
 * In a child cgroup namespace:
 *   - The first process sees its cgroup path as "/" (the namespace root).
 *   - Other processes in the namespace see paths like "/subgroup".
 *
 * Outside the namespace (or without CLONE_NEWCGROUP), the full system
 * cgroup path is visible.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "cgroup_namespace.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"

/* ── Storage ────────────────────────────────────────────────────────── */
static struct cgroup_namespace cgroup_ns_table[CGROUP_NS_MAX];
static int cgroup_ns_count = 0;
static spinlock_t cgroup_ns_lock = 0;

/* ── Initialization ─────────────────────────────────────────────────── */

void cgroup_ns_init(void)
{
    /* The root cgroup namespace: path is "/" (no virtualization) */
    struct cgroup_namespace *root = &cgroup_ns_table[0];
    memset(root, 0, sizeof(*root));
    root->in_use = 1;
    root->refcount = 1;  /* held by init process */
    root->root_path[0] = '/';
    root->root_path[1] = '\0';
    cgroup_ns_count = 1;

    kprintf("[OK] cgroup_namespace: initialized (max %d namespaces)\n",
            CGROUP_NS_MAX - 1);
}

/* ── Create a new namespace ───────────────────────────────────────────
 *
 * Creates a namespace that virtualizes cgroup paths at 'current_cgroup_path'.
 * The new namespace's root_path is set to current_cgroup_path, and processes
 * inside this namespace will see paths relative to it.
 */

struct cgroup_namespace *cgroup_ns_create(const char *current_cgroup_path)
{
    struct cgroup_namespace *ns = NULL;

    spinlock_acquire(&cgroup_ns_lock);

    for (int i = 1; i < CGROUP_NS_MAX; i++) {
        if (!cgroup_ns_table[i].in_use) {
            ns = &cgroup_ns_table[i];
            break;
        }
    }

    if (!ns) {
        spinlock_release(&cgroup_ns_lock);
        kprintf("[CGROUP_NS] Failed to allocate namespace (table full)\n");
        return NULL;
    }

    memset(ns, 0, sizeof(*ns));
    ns->in_use = 1;
    ns->refcount = 1;

    /* Copy the root path (default to "/" if empty) */
    if (current_cgroup_path && current_cgroup_path[0] != '\0') {
        size_t len = strlen(current_cgroup_path);
        if (len >= CGROUP_NS_ROOT_LEN)
            len = CGROUP_NS_ROOT_LEN - 1;
        memcpy(ns->root_path, current_cgroup_path, len);
        ns->root_path[len] = '\0';
    } else {
        ns->root_path[0] = '/';
        ns->root_path[1] = '\0';
    }

    cgroup_ns_count++;

    spinlock_release(&cgroup_ns_lock);

    kprintf("[CGROUP_NS] New ns created (root='%s')\n", ns->root_path);

    return ns;
}

/* ── Reference counting ─────────────────────────────────────────────── */

struct cgroup_namespace *cgroup_ns_get(struct cgroup_namespace *ns)
{
    if (!ns || !ns->in_use) return NULL;

    spinlock_acquire(&cgroup_ns_lock);
    ns->refcount++;
    spinlock_release(&cgroup_ns_lock);

    return ns;
}

void cgroup_ns_put(struct cgroup_namespace *ns)
{
    if (!ns || ns == &cgroup_ns_table[0]) return;  /* never free root */

    spinlock_acquire(&cgroup_ns_lock);

    if (ns->refcount > 0)
        ns->refcount--;

    if (ns->refcount == 0) {
        memset(ns, 0, sizeof(*ns));
        cgroup_ns_count--;
        kprintf("[CGROUP_NS] Namespace freed\n");
    }

    spinlock_release(&cgroup_ns_lock);
}

/* ── Path virtualization ──────────────────────────────────────────────
 *
 * Given a full cgroup path, produce the namespace-virtualized version.
 * If the full path starts with the namespace's root_path, strip the root
 * prefix.  Otherwise, show the full path (shouldn't normally happen).
 */

void cgroup_ns_get_path(const struct cgroup_namespace *ns,
                        const char *full_path,
                        char *out, int max)
{
    if (!out || max <= 0) return;

    /* No namespace, or root namespace: show the full path */
    if (!ns || ns == &cgroup_ns_table[0]) {
        size_t len = strlen(full_path);
        if (len >= (size_t)max) len = (size_t)(max - 1);
        memcpy(out, full_path, len);
        out[len] = '\0';
        return;
    }

    /* Strip the root prefix */
    size_t root_len = strlen(ns->root_path);
    size_t path_len = strlen(full_path);

    if (path_len >= root_len &&
        memcmp(full_path, ns->root_path, root_len) == 0) {
        /* Path starts with root.  Skip the root prefix. */
        const char *relative = full_path + root_len;

        /* If result would be empty, show "/" */
        if (*relative == '\0') {
            out[0] = '/';
            out[1] = '\0';
            return;
        }

        size_t rel_len = strlen(relative);
        if (rel_len >= (size_t)max) rel_len = (size_t)(max - 1);
        memcpy(out, relative, rel_len);
        out[rel_len] = '\0';
    } else {
        /* Path is outside namespace root — show full path as fallback */
        size_t len = strlen(full_path);
        if (len >= (size_t)max) len = (size_t)(max - 1);
        memcpy(out, full_path, len);
        out[len] = '\0';
    }
}

/* ── Inode calculation for /proc/PID/ns/cgroup ────────────────────────
 *
 * Returns a unique inode number for the namespace based on its root path
 * and table index.  Processes in different cgroup namespaces will see
 * different inode numbers via /proc/PID/ns/cgroup.
 */

uint64_t cgroup_ns_inode(const struct cgroup_namespace *ns)
{
    if (!ns) return 4026531835ULL;  /* default constant */

    /* Mix table index and root path hash for uniqueness */
    uint64_t h = (uint64_t)(ns - cgroup_ns_table) * 1000003ULL;
    for (int i = 0; ns->root_path[i] && i < CGROUP_NS_ROOT_LEN; i++)
        h = h * 31 + (uint8_t)ns->root_path[i];
    h &= 0x7FFFFFFFFFFFFFFFULL;
    if (h < 1024) h += 1024;
    return h;
}

/* ── Stub: cgroup_ns_create ─────────────────────────────── */
int cgroup_ns_create(void *parent)
{
    (void)parent;
    kprintf("[cgroup_ns] cgroup_ns_create: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: cgroup_ns_delete ─────────────────────────────── */
int cgroup_ns_delete(void *ns)
{
    (void)ns;
    kprintf("[cgroup_ns] cgroup_ns_delete: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: cgroup_ns_attach ─────────────────────────────── */
int cgroup_ns_attach(void *ns, void *task)
{
    (void)ns;
    (void)task;
    kprintf("[cgroup_ns] cgroup_ns_attach: not yet implemented\n");
    return -ENOSYS;
}
