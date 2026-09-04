#include "landlock.h"

#include "errno.h"
#include "kernel.h"
#include "lsm.h"
#include "printf.h"
#include "process.h"
#include "scheduler.h"
#include "string.h"
#include "vfs.h"

/*
 * Landlock implementation — path-based access-control sandboxing.
 *
 * We maintain a static table of rulesets, each containing a fixed-size
 * array of path-beneath rules.  A process that has called
 * landlock_restrict_self() stores the ruleset indices in its
 * landlock_ruleset_ids[] array.  Access checks via landlock_check_path()
 * verify that the requested operation is permitted by ALL of the
 * process's stacked rulesets.  This enforcement is called from VFS
 * operations.
 *
 * Up to LANDLOCK_MAX_RULESETS_PER_PROC (4) rulesets can be stacked on a
 * single process.  All stacked rulesets must grant the requested access
 * for it to be allowed (intersection semantics).
 */

/* A single path-beneath rule */
struct landlock_path_rule {
    int      used;
    char     path[128];        /* resolved path (from parent_fd + path) */
    uint64_t allowed_access;   /* bitmask of allowed access types */
};

/* A ruleset (set of rules) */
struct landlock_ruleset {
    int      used;
    uint64_t handled_access_fs;   /* which access types this ruleset governs */
    int      rule_count;
    struct landlock_path_rule rules[LANDLOCK_MAX_RULES];
};

/* Global ruleset table */
static struct landlock_ruleset landlock_table[LANDLOCK_MAX_RULESETS];
static int                    landlock_initialised;

void landlock_init(void)
{
    if (landlock_initialised)
        return;

    memset(landlock_table, 0, sizeof(landlock_table));
    landlock_initialised = 1;

    kprintf("[OK] landlock: path-based access control initialised\n");
}

/* Register the Landlock enforcement hook with the LSM framework.  This
 * must run after lsm_init() (Landlock is stack-registered via module_init
 * ordering in the kernel boot path). */
void landlock_lsm_register(void) {
    lsm_register_hook(LSM_HOOK_INODE_PERMISSION, (void *)landlock_enforce);
    lsm_stack_register("landlock");
}

/* Helper: resolve a parent_fd to a path string for use in rules. */
static int resolve_path_from_fd(int fd, char *buf, size_t bufsz)
{
    if (fd < 0) {
        strncpy(buf, "/", bufsz);
        buf[bufsz - 1] = '\0';
        return 0;
    }

    struct process *current = process_get_current();
    if (!current) {
        strncpy(buf, "/", bufsz);
        buf[bufsz - 1] = '\0';
        return 0;
    }

    if (fd == 0) {
        strncpy(buf, "/", bufsz);
        buf[bufsz - 1] = '\0';
        return 0;
    }

    if (fd > 0 && fd < PROCESS_FD_MAX && current->fd_table[fd].used) {
        strncpy(buf, current->fd_table[fd].path, bufsz - 1);
        buf[bufsz - 1] = '\0';
        return 0;
    }

    /* Fallback */
    strncpy(buf, "/", bufsz);
    buf[bufsz - 1] = '\0';
    return 0;
}

/* ── Public API ───────────────────────────────────────────────── */

int landlock_create_ruleset(const struct landlock_ruleset_attr *attr,
                            size_t size, uint32_t flags)
{
    (void)flags;

    if (!landlock_initialised)
        return 0;
    if (!attr)
        return -EFAULT;
    if (size < sizeof(struct landlock_ruleset_attr))
        return -EINVAL;

    /* Validate that only known access bits are handled */
    if (attr->handled_access_fs & ~LANDLOCK_ACCESS_FS_MASK)
        return -EINVAL;

    /* Find a free ruleset slot */
    int idx;

    for (idx = 0; idx < LANDLOCK_MAX_RULESETS; idx++) {
        if (!landlock_table[idx].used)
            goto found;
    }
    return -ENFILE;

found:
    landlock_table[idx].used             = 1;
    landlock_table[idx].handled_access_fs = attr->handled_access_fs;
    landlock_table[idx].rule_count       = 0;
    memset(landlock_table[idx].rules, 0, sizeof(landlock_table[idx].rules));

    return idx;   /* return ruleset fd (table index) */
}

int landlock_add_rule(int ruleset_fd, int rule_type,
                      const struct landlock_path_beneath_attr *rule_attr,
                      uint32_t flags)
{
    (void)flags;

    if (!landlock_initialised)
        return 0;
    if (ruleset_fd < 0 || ruleset_fd >= LANDLOCK_MAX_RULESETS)
        return -EBADF;
    if (!landlock_table[ruleset_fd].used)
        return -EBADF;
    if (rule_type != LANDLOCK_RULE_PATH_BENEATH)
        return -EINVAL;
    if (!rule_attr)
        return -EFAULT;

    /* Validate that allowed_access contains only known access bits */
    if (rule_attr->allowed_access & ~LANDLOCK_ACCESS_FS_MASK)
        return -EINVAL;

    struct landlock_ruleset *rs = &landlock_table[ruleset_fd];

    /* Check that only handled access bits are allowed */
    if (rule_attr->allowed_access & ~rs->handled_access_fs)
        return -EINVAL;

    if (rs->rule_count >= LANDLOCK_MAX_RULES)
        return -ENOSPC;

    struct landlock_path_rule *rule = &rs->rules[rs->rule_count];
    rule->used           = 1;
    rule->allowed_access = rule_attr->allowed_access;

    /* Resolve the parent_fd to a path */
    resolve_path_from_fd(rule_attr->parent_fd, rule->path, sizeof(rule->path));

    rs->rule_count++;
    return 0;
}

/*
 * Check whether a single ruleset grants the requested access on the
 * given path.  Returns 0 if the ruleset permits access, -EACCES if
 * denied.
 */
static int landlock_check_ruleset(const struct landlock_ruleset *rs,
                                  const char *path, uint64_t access_bits)
{
    /* If no access bits are requested, trivially allowed */
    if (access_bits == 0)
        return 0;

    /* Only check the access types that this ruleset handles */
    uint64_t relevant = access_bits & rs->handled_access_fs;

    /* If the requested access doesn't touch any handled bits, allowed */
    if (relevant == 0)
        return 0;

    /* Check each rule in the ruleset — if any rule grants all relevant
     * access bits for a matching path prefix, the operation is allowed. */
    for (int i = 0; i < rs->rule_count; i++) {
        const struct landlock_path_rule *rule = &rs->rules[i];
        if (!rule->used)
            continue;

        /* Check if the path matches (simple prefix match) */
        size_t plen = strlen(rule->path);
        if (plen > 0 && strncmp(path, rule->path, plen) == 0) {
            /* Path under this rule — check access bits */
            if ((relevant & ~rule->allowed_access) == 0) {
                /* All requested access is granted by this rule */
                return 0;
            }
        }
    }

    /* If we get here, no rule granted the requested access */
    return -EACCES;
}

int landlock_restrict_self(int ruleset_fd, uint32_t flags)
{
    (void)flags;

    if (!landlock_initialised)
        return 0;
    if (ruleset_fd < 0 || ruleset_fd >= LANDLOCK_MAX_RULESETS)
        return -EBADF;
    if (!landlock_table[ruleset_fd].used)
        return -EBADF;

    struct process *current = process_get_current();
    if (!current)
        return -ESRCH;

    /* Find a free slot in the process's ruleset stack */
    int slot = -1;
    for (int i = 0; i < LANDLOCK_MAX_RULESETS_PER_PROC; i++) {
        if (current->landlock_ruleset_ids[i] < 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -EPERM;  /* too many stacked rulesets */

    /* Set no_new_privs as required by Landlock semantics (on first restrict) */
    current->no_new_privs = 1;

    /* Store the ruleset index so landlock_check_path() can find it */
    current->landlock_ruleset_ids[slot] = ruleset_fd;

    /* Track the union of handled_access_fs across all stacked rulesets.
     * Access rights outside this set are never enforced (Landlock: a
     * process that never declared handling an access type is unrestricted
     * for it). */
    current->landlock_handled_access_fs |= landlock_table[ruleset_fd].handled_access_fs;

    return 0;
}

/*
 * landlock_check_path() — verify that the given process is allowed
 * to perform the requested access_bits on the given path.
 *
 * ALL stacked rulesets are checked.  If any ruleset denies the access,
 * the access is denied (intersection semantics).  If the process has
 * no landlock rulesets, all access is permitted.
 *
 * Returns 0 if allowed, -EACCES if denied.
 */
int landlock_check_path(const struct process *proc, const char *path,
                        uint64_t access_bits)
{
    if (!proc || !path)
        return -EACCES;

    /* Check each stacked ruleset in order.  All must pass. */
    for (int i = 0; i < LANDLOCK_MAX_RULESETS_PER_PROC; i++) {
        int rs_id = proc->landlock_ruleset_ids[i];
        if (rs_id < 0)
            continue;  /* empty slot, skip */

        if (rs_id >= LANDLOCK_MAX_RULESETS)
            return -EACCES;

        const struct landlock_ruleset *rs = &landlock_table[rs_id];
        if (!rs->used)
            return -EACCES;

        int ret = landlock_check_ruleset(rs, path, access_bits);
        if (ret != 0)
            return ret;  /* denied by this ruleset */
    }

    /* All rulesets granted access or no rulesets at all */
    return 0;
}

/* ── Enforcement: access_mask on fs operations ─────────────────────── */

/* Convert a VFS access mask (VFS_R_OK=4, VFS_W_OK=2, VFS_X_OK=1) into
 * the corresponding Landlock filesystem access rights, so rulesets set
 * up via landlock_add_rule can be enforced at every VFS permission check. */
static uint64_t landlock_mask_from_vfs(int mask) {
    uint64_t bits = 0;

    if (mask & VFS_R_OK)
        bits |= LANDLOCK_ACCESS_FS_READ_FILE;
    if (mask & VFS_W_OK)
        bits |= LANDLOCK_ACCESS_FS_WRITE_FILE;
    if (mask & VFS_X_OK)
        bits |= LANDLOCK_ACCESS_FS_EXECUTE;

    return bits;
}

/* LSM inode_permission / file_permission handler: enforce the current
 * process's stacked Landlock rulesets against the requested access. */
int landlock_enforce(const char *path, int mask) {
    struct process *cur;
    uint64_t bits;

    if (!landlock_initialised)
        return 0;
    if (!path)
        return 0;

    /* A pure existence probe (VFS_F_OK) never carries access rights. */
    if (mask == VFS_F_OK)
        return 0;

    cur = process_get_current();
    if (!cur)
        return 0;

    /* Fast path: current process has no stacked rulesets — nothing to
     * enforce (Landlock only restricts processes that opted in). */
    int has_ruleset = 0;
    for (int i = 0; i < LANDLOCK_MAX_RULESETS_PER_PROC; i++) {
        if (cur->landlock_ruleset_ids[i] >= 0) {
            has_ruleset = 1;
            break;
        }
    }
    if (!has_ruleset)
        return 0;

    bits = landlock_mask_from_vfs(mask);
    if (bits == 0)
        return 0;

    /* Only enforce access rights the process has declared handling via
     * its stacked rulesets (tracked handled_access_fs union).  Unhandled
     * rights are unrestricted, so nothing to enforce if the requested
     * access falls entirely outside the handled set. */
    bits &= cur->landlock_handled_access_fs;
    if (bits == 0)
        return 0;

    return landlock_check_path(cur, path, bits);
}

/* ── Stub: landlock_handle_ptrace ─────────────────────────────── */
static int landlock_handle_ptrace(struct process *tracer, struct process *tracee)
{
    (void)tracer;
    (void)tracee;
    kprintf("[landlock] landlock_handle_ptrace: not yet implemented\n");
    return 0;
}

/* ── Stub: landlock_handle_signal ─────────────────────────────── */
static int landlock_handle_signal(struct process *target, int sig)
{
    (void)target;
    (void)sig;
    kprintf("[landlock] landlock_handle_signal: not yet implemented\n");
    return 0;
}

/* ── Stub: landlock_handle_setprocattr ─────────────────────────────── */
static int landlock_handle_setprocattr(struct process *p, const char *attr, const char *value)
{
    (void)p;
    (void)attr;
    (void)value;
    kprintf("[landlock] landlock_handle_setprocattr: not yet implemented\n");
    return 0;
}

/* ── Stub: landlock_handle_getprocattr ─────────────────────────────── */
static int landlock_handle_getprocattr(struct process *p, const char *attr, char *buf, size_t size)
{
    (void)p;
    (void)attr;
    (void)buf;
    (void)size;
    kprintf("[landlock] landlock_handle_getprocattr: not yet implemented\n");
    return 0;
}

/* ── Stub: landlock_handle_unix_stream ─────────────────────────────── */
static int landlock_handle_unix_stream(struct process *src, struct process *dst)
{
    (void)src;
    (void)dst;
    kprintf("[landlock] landlock_handle_unix_stream: not yet implemented\n");
    return 0;
}

/* ── Stub: landlock_handle_unix_dgram ─────────────────────────────── */
static int landlock_handle_unix_dgram(struct process *src, struct process *dst)
{
    (void)src;
    (void)dst;
    kprintf("[landlock] landlock_handle_unix_dgram: not yet implemented\n");
    return 0;
}

/* ── Stub: landlock_handle_socket_create ─────────────────────────────── */
static int landlock_handle_socket_create(struct process *p, int family, int type, int protocol)
{
    (void)p;
    (void)family;
    (void)type;
    (void)protocol;
    kprintf("[landlock] landlock_handle_socket_create: not yet implemented\n");
    return 0;
}
