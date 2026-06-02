#include "landlock.h"
#include "printf.h"
#include "string.h"
#include "errno.h"
#include "kernel.h"
#include "process.h"
#include "scheduler.h"

/*
 * Landlock implementation — path-based access-control sandboxing.
 *
 * We maintain a static table of rulesets, each containing a fixed-size
 * array of path-beneath rules.  A process that has called
 * landlock_restrict_self() stores the ruleset index in its
 * landlock_ruleset_id field.  Access checks via landlock_check_path()
 * verify that the requested operation is permitted by the process's
 * own ruleset.  This enforcement is called from VFS operations.
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
        return -ENOSYS;
    if (!attr)
        return -EFAULT;
    if (size < sizeof(struct landlock_ruleset_attr))
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
        return -ENOSYS;
    if (ruleset_fd < 0 || ruleset_fd >= LANDLOCK_MAX_RULESETS)
        return -EBADF;
    if (!landlock_table[ruleset_fd].used)
        return -EBADF;
    if (rule_type != LANDLOCK_RULE_PATH_BENEATH)
        return -EINVAL;
    if (!rule_attr)
        return -EFAULT;

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

int landlock_restrict_self(int ruleset_fd, uint32_t flags)
{
    (void)flags;

    if (!landlock_initialised)
        return -ENOSYS;
    if (ruleset_fd < 0 || ruleset_fd >= LANDLOCK_MAX_RULESETS)
        return -EBADF;
    if (!landlock_table[ruleset_fd].used)
        return -EBADF;

    struct process *current = process_get_current();
    if (!current)
        return -ESRCH;

    /* If the process already has a landlock ruleset, reject (no stacking yet) */
    if (current->landlock_ruleset_id >= 0)
        return -EPERM;

    /* Set no_new_privs as required by Landlock semantics */
    current->no_new_privs = 1;

    /* Store the ruleset index so landlock_check_path() can find it */
    current->landlock_ruleset_id = ruleset_fd;

    return 0;
}

/*
 * landlock_check_path() — verify that the given process is allowed
 * to perform the requested access_bits on the given path.
 *
 * Only the ruleset associated with this process (via landlock_ruleset_id)
 * is checked.  If the process has no landlock ruleset, all access is
 * permitted.
 *
 * Returns 0 if allowed, -EACCES if denied.
 */
int landlock_check_path(const struct process *proc, const char *path,
                        uint64_t access_bits)
{
    if (!proc || !path)
        return -EACCES;

    /* If the process has no landlock restrictions, all access allowed */
    if (proc->landlock_ruleset_id < 0)
        return 0;

    int rs_id = proc->landlock_ruleset_id;
    if (rs_id >= LANDLOCK_MAX_RULESETS)
        return -EACCES;

    const struct landlock_ruleset *rs = &landlock_table[rs_id];
    if (!rs->used)
        return -EACCES;

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
