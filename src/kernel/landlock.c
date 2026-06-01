#include "landlock.h"
#include "printf.h"
#include "string.h"
#include "errno.h"
#include "kernel.h"
#include "heap.h"

/*
 * Landlock implementation.
 *
 * We maintain a static table of rulesets, each containing a fixed-size
 * array of path-beneath rules.  A process that has called
 * landlock_restrict_self is subject to access checks via
 * landlock_check_path().
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

/* Helper: find length-limited path from a fake fd.
 * In a real kernel this would resolve from the VFS layer.
 * Here we just copy the string from the fd's path if available. */
static int resolve_path_from_fd(int fd, char *buf, size_t bufsz)
{
    /* The "parent_fd" in our mock is actually a path string stored
     * in the process's fd table, or we treat small fd values as
     * well-known roots:
     *   fd 0 = /dev/tty  (not a directory)
     *   fd 1 = stdout    (not a directory)
     *   For a real implementation we would walk the VFS.
     *
     * Since this is a simplified model, we stub it out:
     * if fd == 0 we treat as "/" (root).
     * Otherwise we try to read from the process fd table.
     */
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

    /* Attach the ruleset to the current process.
     * We store the ruleset index directly in the process for checks.
     * We reuse a spare field: no_new_privs indicates sandboxing.
     */
    /* Store the ruleset fd as a negative value so we can look it up.
     * Actually, we store it in a separate field or just a static mapping.
     * For simplicity, we use the process's 'no_new_privs' flag to
     * indicate that landlock is active + store the ruleset index
     * via a separate per-pid table.
     */
    current->no_new_privs = 1;

    /* Store the mapping: we use a simple static array process -> ruleset */
    /* For this simplified model we just note that the process is restricted.
     * The actual enforcement happens in landlock_check_path(). */

    return 0;
}

int landlock_check_path(const struct process *proc, const char *path,
                        uint64_t access_bits)
{
    if (!proc || !path)
        return -EACCES;

    /* If the process has no landlock restrictions, all access allowed */
    if (!proc->no_new_privs)
        return 0;

    /* Scan all rulesets and find any that apply to this process.
     * In a real implementation we'd walk the process's ruleset chain.
     * For this simplified model, we check all active rulesets. */
    for (int r = 0; r < LANDLOCK_MAX_RULESETS; r++) {
        if (!landlock_table[r].used)
            continue;

        const struct landlock_ruleset *rs = &landlock_table[r];

        /* Only check the access types that this ruleset handles */
        if ((access_bits & ~rs->handled_access_fs) != 0)
            continue;   /* this ruleset doesn't govern these bits */

        /* Check each rule in the ruleset */
        for (int i = 0; i < rs->rule_count; i++) {
            const struct landlock_path_rule *rule = &rs->rules[i];
            if (!rule->used)
                continue;

            /* Check if the path matches (simple prefix match) */
            size_t plen = strlen(rule->path);
            if (strncmp(path, rule->path, plen) == 0) {
                /* Path under this rule — check access bits */
                if ((access_bits & ~rule->allowed_access) == 0) {
                    /* All requested access is granted by this rule */
                    return 0;
                }
            }
        }
    }

    /* If we get here, no rule granted the requested access */
    return -EACCES;
}
