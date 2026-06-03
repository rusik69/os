#ifndef LANDLOCK_H
#define LANDLOCK_H

#include "types.h"
#include "process.h"

/*
 * Landlock — unprivileged access-control sandboxing (simplified).
 * Provides path-based access-control rules that can be stacked
 * onto processes.
 */

/* landlock_create_ruleset flags */
#define LANDLOCK_CREATE_RULESET    1

/* Access rights for filesystem operations */
#define LANDLOCK_ACCESS_FS_EXECUTE      (1ULL << 0)
#define LANDLOCK_ACCESS_FS_WRITE_FILE   (1ULL << 1)
#define LANDLOCK_ACCESS_FS_READ_FILE    (1ULL << 2)
#define LANDLOCK_ACCESS_FS_READ_DIR     (1ULL << 3)

/* Maximum entries in the ruleset / path tables */
#define LANDLOCK_MAX_RULESETS     8
#define LANDLOCK_MAX_RULES       16
#define LANDLOCK_MAX_RULESETS_PER_PROC 4  /* max stacked rulesets per process */

/* Rule type for landlock_add_rule */
#define LANDLOCK_RULE_PATH_BENEATH  1

/* Landlock ruleset attribute (for landlock_create_ruleset) */
struct landlock_ruleset_attr {
    uint64_t handled_access_fs;
};

/* Path-beneath rule (for landlock_add_rule) */
struct landlock_path_beneath_attr {
    uint64_t allowed_access;
    int      parent_fd;       /* fd of parent directory */
};

/* ── Syscall-like API ─────────────────────────────────────────── */

/* Create a new ruleset; returns a fd (table index) */
int landlock_create_ruleset(const struct landlock_ruleset_attr *attr,
                            size_t size, uint32_t flags);

/* Add a rule (path_beneath) to an existing ruleset */
int landlock_add_rule(int ruleset_fd, int rule_type,
                      const struct landlock_path_beneath_attr *rule_attr,
                      uint32_t flags);

/* Apply a ruleset to the current process (enforce) */
int landlock_restrict_self(int ruleset_fd, uint32_t flags);

/* Check whether a given access path is allowed for a given process.
 * Returns 0 if allowed, -EACCES otherwise. */
int landlock_check_path(const struct process *proc, const char *path,
                        uint64_t access_bits);

/* Init called during kernel boot */
void landlock_init(void);

#endif /* LANDLOCK_H */
