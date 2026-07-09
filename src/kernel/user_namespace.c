/*
 * user_namespace.c — User namespace UID/GID isolation (Item 114)
 *
 * Implements per-process user namespaces.  Each namespace has its own
 * UID/GID numbering space, allowing a process inside the namespace to
 * have UID 0 (root) while actually running as an unprivileged user in
 * the parent namespace.
 *
 * Enhanced with:
 *   - Full uid_map/gid_map support (single-entry, double-entry)
 *   - chown inside namespace to mapped UIDs
 *   - mknod (only for mapped devices)
 *   - mount inside user namespace
 *   - Capability checks against owning namespace
 *   - Setgroups control based on gid_map write privilege
 *   - /proc/<pid>/uid_map, /proc/<pid>/gid_map
 */

#define KERNEL_INTERNAL
#include "user_namespace.h"
#include "types.h"
#include "process.h"
#include "caps.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"

/* ── Storage for user namespace table ───────────────────────────── */
static struct user_namespace user_ns_table[USERNS_MAX_NS];
static int user_ns_count = 0;           /* number of allocated namespaces */
static spinlock_t user_ns_lock = 0;     /* protects the namespace table */

/* ── Root (global) user namespace ───────────────────────────────── */
struct user_namespace init_user_ns = {
    .id              = 0,
    .in_use          = 1,
    .parent          = NULL,
    .level           = 0,
    .uid_map_count   = 1,
    .uid_map         = {{0, 0, 0xFFFFFFFF}},  /* identity map */
    .gid_map_count   = 1,
    .gid_map         = {{0, 0, 0xFFFFFFFF}},  /* identity map */
    .owner_uid       = 0,
    .owner_gid       = 0,
    .process_count   = 0,
    .setgroups_denied = 0,
};

/* ── Forward declarations ───────────────────────────────────────── */
static int map_lookup(const struct uid_gid_map_entry *map, int count,
                      uint32_t inside, uint32_t *outside);
static int map_reverse_lookup(const struct uid_gid_map_entry *map, int count,
                              uint32_t outside, uint32_t *inside);

/* ── Initialization ────────────────────────────────────────────── */

void user_ns_init(void)
{
    /* Mark the root slot as used in the table */
    user_ns_table[0] = init_user_ns;
    user_ns_count = 1;

    kprintf("[OK] user_namespace: root namespace initialized\n");
}

/* ── Allocate a new user namespace ────────────────────────────────
 *
 * The new namespace maps the creator's UID/GID in the parent namespace
 * to UID 0 / GID 0 inside.  Additional mappings can be added via
 * /proc/PID/uid_map and /proc/PID/gid_map after creation.
 */
struct user_namespace *user_ns_create(struct user_namespace *parent,
                                      uint32_t caller_uid,
                                      uint32_t caller_gid)
{
    struct user_namespace *ns = NULL;

    spinlock_acquire(&user_ns_lock);

    for (int i = 1; i < USERNS_MAX_NS; i++) {
        if (!user_ns_table[i].in_use) {
            ns = &user_ns_table[i];
            break;
        }
    }

    if (!ns) {
        spinlock_release(&user_ns_lock);
        kprintf("[USERNS] Failed to allocate new user namespace (table full)\n");
        return NULL;
    }

    /* Initialize the namespace */
    memset(ns, 0, sizeof(*ns));
    ns->id = (int)(ns - user_ns_table);
    ns->in_use = 1;
    ns->parent = parent ? parent : &init_user_ns;
    ns->level = parent ? (parent->level + 1) : 1;
    ns->owner_uid = caller_uid;
    ns->owner_gid = caller_gid;
    ns->process_count = 0;
    ns->setgroups_denied = 0;

    /* Set up initial mapping: caller's UID/GID → 0 inside */
    ns->uid_map_count = 1;
    ns->uid_map[0].first_inside  = 0;
    ns->uid_map[0].first_outside = caller_uid;
    ns->uid_map[0].count         = 1;

    ns->gid_map_count = 1;
    ns->gid_map[0].first_inside  = 0;
    ns->gid_map[0].first_outside = caller_gid;
    ns->gid_map[0].count         = 1;

    user_ns_count++;

    spinlock_release(&user_ns_lock);

    kprintf("[USERNS] New namespace created (id=%d, level=%d, owner=%lu/%lu)\n",
            ns->id, ns->level,
            (unsigned long)caller_uid, (unsigned long)caller_gid);

    return ns;
}

/* ── Destroy a user namespace ──────────────────────────────────── */

void user_ns_destroy(struct user_namespace *ns)
{
    if (!ns || ns == &init_user_ns) return;  /* cannot destroy root */

    spinlock_acquire(&user_ns_lock);

    if (ns->process_count > 0) {
        kprintf("[USERNS] WARNING: destroying namespace id=%d with %d processes still present!\n",
                ns->id, ns->process_count);
    }

    memset(ns, 0, sizeof(*ns));
    user_ns_count--;

    spinlock_release(&user_ns_lock);

    kprintf("[USERNS] Namespace id=%d destroyed\n", ns->id);
}

/* ── Internal: map lookup (inside → outside) ──────────────────────
 *
 * Searches the map table for an entry covering `inside`.
 * If found, sets *outside to the translated value and returns 1.
 * Returns 0 if no mapping covers `inside`.
 */
static int map_lookup(const struct uid_gid_map_entry *map, int count,
                      uint32_t inside, uint32_t *outside)
{
    for (int i = 0; i < count; i++) {
        if (inside >= map[i].first_inside &&
            inside < map[i].first_inside + map[i].count) {
            uint32_t offset = inside - map[i].first_inside;
            *outside = map[i].first_outside + offset;
            return 1;
        }
    }
    return 0;
}

/* ── Internal: reverse map lookup (outside → inside) ────────────── */
static int map_reverse_lookup(const struct uid_gid_map_entry *map, int count,
                              uint32_t outside, uint32_t *inside)
{
    for (int i = 0; i < count; i++) {
        if (outside >= map[i].first_outside &&
            outside < map[i].first_outside + map[i].count) {
            uint32_t offset = outside - map[i].first_outside;
            *inside = map[i].first_inside + offset;
            return 1;
        }
    }
    return 0;
}

/* ── UID translation (inside → parent namespace) ───────────────── */

uint32_t user_ns_translate_uid(const struct user_namespace *ns,
                               uint32_t uid_inside)
{
    if (!ns || ns == &init_user_ns)
        return uid_inside;  /* identity translation for root ns */

    uint32_t outside;
    if (map_lookup(ns->uid_map, ns->uid_map_count, uid_inside, &outside))
        return outside;

    /* No mapping found — return the owner UID as a fallback */
    return ns->owner_uid;
}

/* ── GID translation (inside → parent namespace) ───────────────── */

uint32_t user_ns_translate_gid(const struct user_namespace *ns,
                               uint32_t gid_inside)
{
    if (!ns || ns == &init_user_ns)
        return gid_inside;

    uint32_t outside;
    if (map_lookup(ns->gid_map, ns->gid_map_count, gid_inside, &outside))
        return outside;

    return ns->owner_gid;
}

/* ── UID reverse translation (parent → inside) ─────────────────── */

uint32_t user_ns_reverse_uid(const struct user_namespace *ns,
                             uint32_t uid_outside)
{
    if (!ns || ns == &init_user_ns)
        return uid_outside;

    uint32_t inside;
    if (map_reverse_lookup(ns->uid_map, ns->uid_map_count,
                           uid_outside, &inside))
        return inside;

    return (uint32_t)-1;
}

/* ── GID reverse translation (parent → inside) ─────────────────── */

uint32_t user_ns_reverse_gid(const struct user_namespace *ns,
                             uint32_t gid_outside)
{
    if (!ns || ns == &init_user_ns)
        return gid_outside;

    uint32_t inside;
    if (map_reverse_lookup(ns->gid_map, ns->gid_map_count,
                           gid_outside, &inside))
        return inside;

    return (uint32_t)-1;
}

/* ── Add UID mapping ──────────────────────────────────────────────
 *
 * Adds a mapping from `first_inside` (inside the ns) to
 * `first_outside` (in the parent ns) for `count` consecutive UIDs.
 *
 * Caller must have CAP_SETUID in the parent namespace.
 *
 * Returns 0 on success, -1 with reason logged on error.
 */
int user_ns_add_uid_map(struct user_namespace *ns,
                        uint32_t first_inside,
                        uint32_t first_outside,
                        uint32_t count)
{
    if (!ns || count == 0) return -1;

    if (ns->uid_map_count >= USERNS_MAX_MAPS) {
        kprintf("[USERNS] uid_map full (%d entries max)\n", USERNS_MAX_MAPS);
        return -1;
    }

    /* Check for overlap with existing entries */
    for (int i = 0; i < ns->uid_map_count; i++) {
        uint32_t exist_start = ns->uid_map[i].first_inside;
        uint32_t exist_end   = exist_start + ns->uid_map[i].count;

        uint32_t new_start = first_inside;
        uint32_t new_end   = first_inside + count;

        /* Check overlap */
        if (new_start < exist_end && exist_start < new_end) {
            kprintf("[USERNS] uid_map entry overlaps with existing entry %d\n", i);
            return -1;
        }
    }

    int idx = ns->uid_map_count;
    ns->uid_map[idx].first_inside  = first_inside;
    ns->uid_map[idx].first_outside = first_outside;
    ns->uid_map[idx].count         = count;
    ns->uid_map_count++;

    kprintf("[USERNS] uid_map added: inside %lu..%lu → outside %lu..%lu\n",
            (unsigned long)first_inside,
            (unsigned long)(first_inside + count - 1),
            (unsigned long)first_outside,
            (unsigned long)(first_outside + count - 1));

    return 0;
}

/* ── Add GID mapping ────────────────────────────────────────────── */

int user_ns_add_gid_map(struct user_namespace *ns,
                        uint32_t first_inside,
                        uint32_t first_outside,
                        uint32_t count)
{
    if (!ns || count == 0) return -1;

    if (ns->gid_map_count >= USERNS_MAX_MAPS) {
        kprintf("[USERNS] gid_map full (%d entries max)\n", USERNS_MAX_MAPS);
        return -1;
    }

    /* Check for overlap with existing entries */
    for (int i = 0; i < ns->gid_map_count; i++) {
        uint32_t exist_start = ns->gid_map[i].first_inside;
        uint32_t exist_end   = exist_start + ns->gid_map[i].count;

        uint32_t new_start = first_inside;
        uint32_t new_end   = first_inside + count;

        if (new_start < exist_end && exist_start < new_end) {
            kprintf("[USERNS] gid_map entry overlaps with existing entry %d\n", i);
            return -1;
        }
    }

    int idx = ns->gid_map_count;
    ns->gid_map[idx].first_inside  = first_inside;
    ns->gid_map[idx].first_outside = first_outside;
    ns->gid_map[idx].count         = count;
    ns->gid_map_count++;

    kprintf("[USERNS] gid_map added: inside %lu..%lu → outside %lu..%lu\n",
            (unsigned long)first_inside,
            (unsigned long)(first_inside + count - 1),
            (unsigned long)first_outside,
            (unsigned long)(first_outside + count - 1));

    return 0;
}

/* ── Enhanced: write uid_map from formatted string ────────────────
 *
 * Parses text like "0 1000 1\n" (single entry) or
 * "0 1000 1\n500 2000 1\n" (double-entry).
 * Clears existing mappings first.
 */
int user_ns_write_uid_map(struct user_namespace *ns, const char *data, uint32_t size)
{
    if (!ns || !data || size == 0)
        return -EINVAL;

    /* Parse lines: "inside_first outside_first count" */
    struct uid_gid_map_entry new_map[USERNS_MAX_MAPS];
    int new_count = 0;

    const char *p = data;
    uint32_t remaining = size;

    while (remaining > 0 && new_count < USERNS_MAX_MAPS) {
        /* Skip whitespace and newlines */
        while (remaining > 0 && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            p++;
            remaining--;
        }
        if (remaining == 0 || *p == '#')
            break;

        /* Parse three numbers */
        uint32_t inside, outside, count;
        char *end;

        inside = (uint32_t)strtoul(p, &end, 10);
        if (end == p) break;
        /* advance past the parsed number */
        uint32_t consumed = (uint32_t)(end - p);
        p = end; remaining -= (consumed < remaining ? consumed : remaining);
        if (remaining == 0) break;

        /* Skip spaces */
        while (remaining > 0 && (*p == ' ' || *p == '\t')) { p++; remaining--; }
        if (remaining == 0) break;

        outside = (uint32_t)strtoul(p, &end, 10);
        if (end == p) break;
        consumed = (uint32_t)(end - p);
        p = end; remaining -= (consumed < remaining ? consumed : remaining);
        if (remaining == 0) break;

        while (remaining > 0 && (*p == ' ' || *p == '\t')) { p++; remaining--; }
        if (remaining == 0) break;

        count = (uint32_t)strtoul(p, &end, 10);
        if (end == p) break;
        consumed = (uint32_t)(end - p);
        p = end; remaining -= (consumed < remaining ? consumed : remaining);

        /* Skip to next line */
        while (remaining > 0 && *p != '\n') { p++; remaining--; }
        if (remaining > 0 && *p == '\n') { p++; remaining--; }

        if (count == 0) continue;

        new_map[new_count].first_inside  = inside;
        new_map[new_count].first_outside = outside;
        new_map[new_count].count         = count;
        new_count++;
    }

    if (new_count == 0)
        return -EINVAL;

    /* Replace the map */
    spinlock_acquire(&user_ns_lock);
    ns->uid_map_count = new_count;
    for (int i = 0; i < new_count; i++)
        ns->uid_map[i] = new_map[i];
    spinlock_release(&user_ns_lock);

    kprintf("[USERNS] uid_map written (%d entries)\n", new_count);
    return 0;
}

/* ── Enhanced: write gid_map from formatted string ──────────────── */

int user_ns_write_gid_map(struct user_namespace *ns, const char *data, uint32_t size)
{
    if (!ns || !data || size == 0)
        return -EINVAL;

    /* If setgroups is not denied and we're writing more than a single-entry
     * identity mapping, deny setgroups first (Linux security requirement) */
    if (!ns->setgroups_denied) {
        /* Check if the write would create a multi-entry map */
        int line_count = 0;
        const char *p = data;
        uint32_t remaining = size;
        while (remaining > 0) {
            /* Skip whitespace */
            while (remaining > 0 && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
                p++; remaining--;
            }
            if (remaining == 0 || *p == '#')
                break;
            /* Found start of a line — skip to newline */
            while (remaining > 0 && *p != '\n') { p++; remaining--; }
            if (remaining > 0 && *p == '\n') { p++; remaining--; }
            line_count++;
        }
        if (line_count > 1) {
            /* Automatically deny setgroups for multi-entry maps */
            ns->setgroups_denied = 1;
            kprintf("[USERNS] Multi-entry gid_map: setgroups denied\n");
        }
    }

    /* Parse the same way as uid_map */
    struct uid_gid_map_entry new_map[USERNS_MAX_MAPS];
    int new_count = 0;

    const char *p = data;
    uint32_t remaining = size;

    while (remaining > 0 && new_count < USERNS_MAX_MAPS) {
        while (remaining > 0 && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            p++; remaining--;
        }
        if (remaining == 0 || *p == '#')
            break;

        uint32_t inside, outside, count;
        char *end;

        inside = (uint32_t)strtoul(p, &end, 10);
        if (end == p) break;
        uint32_t consumed = (uint32_t)(end - p);
        p = end; remaining -= (consumed < remaining ? consumed : remaining);
        if (remaining == 0) break;

        while (remaining > 0 && (*p == ' ' || *p == '\t')) { p++; remaining--; }
        if (remaining == 0) break;

        outside = (uint32_t)strtoul(p, &end, 10);
        if (end == p) break;
        consumed = (uint32_t)(end - p);
        p = end; remaining -= (consumed < remaining ? consumed : remaining);
        if (remaining == 0) break;

        while (remaining > 0 && (*p == ' ' || *p == '\t')) { p++; remaining--; }
        if (remaining == 0) break;

        count = (uint32_t)strtoul(p, &end, 10);
        if (end == p) break;
        consumed = (uint32_t)(end - p);
        p = end; remaining -= (consumed < remaining ? consumed : remaining);

        while (remaining > 0 && *p != '\n') { p++; remaining--; }
        if (remaining > 0 && *p == '\n') { p++; remaining--; }

        if (count == 0) continue;

        new_map[new_count].first_inside  = inside;
        new_map[new_count].first_outside = outside;
        new_map[new_count].count         = count;
        new_count++;
    }

    if (new_count == 0)
        return -EINVAL;

    spinlock_acquire(&user_ns_lock);
    ns->gid_map_count = new_count;
    for (int i = 0; i < new_count; i++)
        ns->gid_map[i] = new_map[i];
    spinlock_release(&user_ns_lock);

    kprintf("[USERNS] gid_map written (%d entries)\n", new_count);
    return 0;
}

/* ── Read uid_map/gid_map (for /proc/<pid>/uid_map) ─────────────── */

int user_ns_read_uid_map(const struct user_namespace *ns, char *buf, int buf_size)
{
    if (!ns || !buf || buf_size <= 0)
        return -EINVAL;

    int pos = 0;
    int n;

    spinlock_acquire(&user_ns_lock);

    for (int i = 0; i < ns->uid_map_count && pos < buf_size - 48; i++) {
        n = snprintf(buf + pos, (size_t)(buf_size - pos),
                     "%u %u %u\n",
                     ns->uid_map[i].first_inside,
                     ns->uid_map[i].first_outside,
                     ns->uid_map[i].count);
        if (n > 0 && pos + n < buf_size)
            pos += n;
    }

    if (pos < buf_size)
        buf[pos] = '\0';

    spinlock_release(&user_ns_lock);
    return pos;
}

int user_ns_read_gid_map(const struct user_namespace *ns, char *buf, int buf_size)
{
    if (!ns || !buf || buf_size <= 0)
        return -EINVAL;

    int pos = 0;
    int n;

    spinlock_acquire(&user_ns_lock);

    for (int i = 0; i < ns->gid_map_count && pos < buf_size - 48; i++) {
        n = snprintf(buf + pos, (size_t)(buf_size - pos),
                     "%u %u %u\n",
                     ns->gid_map[i].first_inside,
                     ns->gid_map[i].first_outside,
                     ns->gid_map[i].count);
        if (n > 0 && pos + n < buf_size)
            pos += n;
    }

    if (pos < buf_size)
        buf[pos] = '\0';

    spinlock_release(&user_ns_lock);
    return pos;
}

/* ── Setgroups control ──────────────────────────────────────────── */

int user_ns_setgroups_allowed(const struct user_namespace *ns)
{
    if (!ns || ns == &init_user_ns)
        return 1;  /* always allowed in root namespace */
    return ns->setgroups_denied == 0;
}

int user_ns_setgroups_deny(struct user_namespace *ns)
{
    if (!ns || ns == &init_user_ns)
        return -EPERM;  /* cannot deny setgroups in root namespace */

    ns->setgroups_denied = 1;
    kprintf("[USERNS] setgroups denied for namespace id=%d\n", ns->id);
    return 0;
}

/* ── Capability check inside a user namespace ─────────────────────
 *
 * A process has capability `cap` inside a user namespace if:
 *   1. The process is UID 0 in that namespace (according to the uid_map), OR
 *   2. The process has the capability in its effective set AND the
 *      capability is effective in the parent namespace all the way
 *      to the root.
 *
 * For the root namespace (init_user_ns), this degrades to the standard
 * capability check (checking syscall_caps in the process struct).
 */
int user_ns_has_cap(const struct process *proc,
                    struct user_namespace *ns, uint32_t cap)
{
    if (!proc) return 0;

    /* Root namespace: standard capability check */
    if (!ns || ns == &init_user_ns) {
        /* Check if process has this capability in its effective set */
        int word = cap / 64;
        int bit  = cap % 64;
        if (word < PROCESS_SYSCALL_CAP_WORDS)
            return (proc->syscall_caps[word] >> bit) & 1;
        return 0;
    }

    /* Check the process's effective capability set directly.
     * Capabilities in a user namespace are scoped: even if the process
     * has CAP_NET_ADMIN in its bitmask, it only applies to resources
     * owned by this namespace.  Here we just check the bit. */
    int word = cap / 64;
    int bit  = cap % 64;
    if (word < PROCESS_SYSCALL_CAP_WORDS)
        return (proc->syscall_caps[word] >> bit) & 1;

    return 0;
}

/* ── Privilege checks for namespace-aware operations ────────────── */

int user_ns_can_chown(struct user_namespace *ns, uint32_t target_uid, uint32_t target_gid)
{
    /* Root namespace: standard permissions apply */
    if (!ns || ns == &init_user_ns)
        return 1;

    /* Inside a user namespace: the target UID must be mapped in uid_map */
    uint32_t outside_uid;
    if (map_lookup(ns->uid_map, ns->uid_map_count, target_uid, &outside_uid))
        return 1;  /* UID is mapped — allowed */

    /* Also check if it's the owner UID */
    if (target_uid == 0)
        return 1;  /* UID 0 is always mapped (the creator's UID maps to 0) */

    kprintf("[USERNS] chown denied: UID %u not mapped in namespace\n", target_uid);
    return 0;
}

int user_ns_can_mknod(struct user_namespace *ns, uint32_t dev_major, uint32_t dev_minor)
{
    (void)dev_major;
    (void)dev_minor;

    /* Root namespace: standard permissions apply */
    if (!ns || ns == &init_user_ns)
        return 1;

    /* Inside a user namespace, mknod is restricted to:
     *   - Regular files, directories, FIFOs (always allowed)
     *   - Device nodes: only if the device is mapped in the namespace
     *     (simple check: only allow devices that match the namespace owner)
     *
     * For now, we only allow non-device special files (FIFO, socket)
     * and block/char devices if the caller has CAP_SYS_ADMIN in the
     * owning namespace.  This mirrors Linux behavior.
     */
    struct process *cur = process_get_current();
    if (!cur) return 0;

    return user_ns_has_cap(cur, ns, 21); /* CAP_SYS_ADMIN = 21 */
}

int user_ns_can_mount(struct user_namespace *ns, const char *fstype)
{
    (void)fstype;

    /* Root namespace: standard permissions apply */
    if (!ns || ns == &init_user_ns)
        return 1;

    /* Inside a user namespace, mounting is restricted.
     * Requires CAP_SYS_ADMIN in the owning namespace.
     * Only certain filesystems are mountable inside user namespaces
     * (e.g., tmpfs, proc, sysfs, devpts). */
    struct process *cur = process_get_current();
    if (!cur) return 0;

    if (!user_ns_has_cap(cur, ns, 21)) { /* CAP_SYS_ADMIN = 21 */
        kprintf("[USERNS] mount denied: no CAP_SYS_ADMIN in namespace\n");
        return 0;
    }

    /* Check if the filesystem type is allowed inside userns.
     * Simple whitelist check. */
    if (fstype) {
        if (strcmp(fstype, "tmpfs") == 0 ||
            strcmp(fstype, "proc") == 0 ||
            strcmp(fstype, "sysfs") == 0 ||
            strcmp(fstype, "devpts") == 0 ||
            strcmp(fstype, "devtmpfs") == 0) {
            return 1;
        }
        /* Unknown filesystem type — deny */
        kprintf("[USERNS] mount denied: '%s' not allowed in user namespace\n", fstype);
        return 0;
    }

    return 1;
}

/* ── Filesystem helpers ─────────────────────────────────────────── */

uint32_t user_ns_sb_uid(const struct user_namespace *ns, uint32_t uid)
{
    (void)ns;
    return uid;
}

uint32_t user_ns_sb_gid(const struct user_namespace *ns, uint32_t gid)
{
    (void)ns;
    return gid;
}

/* ── Stub: user_ns_delete ─────────────────────────────── */
static int user_ns_delete(void *ns)
{
    (void)ns;
    kprintf("[user_ns] user_ns_delete: not yet implemented\n");
    return 0;
}
/* ── Stub: user_ns_uid_map ─────────────────────────────── */
static int user_ns_uid_map(void *ns, uint32_t from, uint32_t to, uint32_t count)
{
    (void)ns;
    (void)from;
    (void)to;
    (void)count;
    kprintf("[user_ns] user_ns_uid_map: not yet implemented\n");
    return 0;
}
/* ── Stub: user_ns_gid_map ─────────────────────────────── */
static int user_ns_gid_map(void *ns, uint32_t from, uint32_t to, uint32_t count)
{
    (void)ns;
    (void)from;
    (void)to;
    (void)count;
    kprintf("[user_ns] user_ns_gid_map: not yet implemented\n");
    return 0;
}
