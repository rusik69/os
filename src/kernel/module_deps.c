/*
 * module_deps.c — Module dependency resolver with topological sort
 *                 and cycle detection (M25)
 *
 * When a module declares dependencies via MODULE_DEPENDS() macro
 * (embedded in the .modinfo section as "depends=dep1,dep2"), this
 * module ensures they are loaded before the dependent module starts.
 *
 * Loading sequence:
 *   1. Parse "depends=" from .modinfo in the ELF parser
 *   2. For each dependency:
 *      a. If already loaded (LIVE state), continue
 *      b. If not loaded, try request_module() to auto-load
 *      c. If load fails, abort with a descriptive error
 *   3. Detect cycles using a depth-limited DFS with a visited set
 *      before attempting to load any module.
 */

#include "module.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"

/* Maximum recursion depth for cycle detection */
#define MODULE_DEP_MAX_DEPTH 32

/* ── Forward declarations ──────────────────────────────────────────── */

/* ── Cycle detection ───────────────────────────────────────────────────
 *
 * Performs a depth-first search from @mod_name to detect cycles in the
 * dependency graph.  @visited tracks modules currently on the recursion
 * stack (to detect back-edges), @depth limits recursion depth.
 *
 * Returns 0 if no cycle is found, -1 if a cycle is detected with a
 * descriptive message written to @err_buf.
 */
static int module_dep_detect_cycle(const char *mod_name,
                                    const char *stack[], int stack_len,
                                    char *err_buf, int err_len)
{
    if (!mod_name || !mod_name[0])
        return 0;

    /* Check if we've exceeded recursion depth */
    if (stack_len >= MODULE_DEP_MAX_DEPTH) {
        snprintf(err_buf, (size_t)err_len,
                 "dependency resolution exceeded max depth (%d) for '%s' "
                 "(possible cycle or too deep)",
                 MODULE_DEP_MAX_DEPTH, mod_name);
        return -1;
    }

    /* Check if mod_name is already in the current stack (back-edge = cycle) */
    for (int i = 0; i < stack_len; i++) {
        if (strcmp(stack[i], mod_name) == 0) {
            /* Build a cycle path string for diagnostics */
            char cycle_path[256];
            int pos = 0;
            for (int j = i; j < stack_len && pos < (int)sizeof(cycle_path) - 40; j++) {
                int n = snprintf(cycle_path + pos,
                                 (size_t)(sizeof(cycle_path) - (size_t)pos),
                                 "%s -> ", stack[j]);
                if (n > 0) pos += n;
            }
            snprintf(cycle_path + pos,
                     (size_t)(sizeof(cycle_path) - (size_t)pos),
                     "%s", mod_name);
            snprintf(err_buf, (size_t)err_len,
                     "dependency cycle detected: %s", cycle_path);
            return -1;
        }
    }

    /* Find the module and check its dependencies */
    struct kernel_module *mod = module_find(mod_name);
    if (!mod)
        return 0; /* not loaded yet — no cycle possible from unloaded modules */

    /* Add current module to the recursion stack.
     * We can't modify the stack parameter directly since it's const
     * (declared as 'const char *stack[]').  Instead, we build a local
     * copy for the recursive call. */
    const char *local_stack[MODULE_DEP_MAX_DEPTH];
    int local_len = stack_len;
    for (int i = 0; i < stack_len; i++)
        local_stack[i] = stack[i];
    local_stack[local_len] = mod->name;
    local_len++;

    /* Recurse into each dependency */
    for (int i = 0; i < mod->num_deps; i++) {
        if (module_dep_detect_cycle(mod->deps[i].name,
                                     local_stack, local_len,
                                     err_buf, err_len) < 0) {
            return -1;
        }
    }

    return 0;
}

/* ── Dependency resolver (topological sort with auto-load) ──────────────
 *
 * Given a module name, ensures all its transitive dependencies are loaded.
 * Uses a simple depth-first topological ordering:
 *   1. If the module is already loaded, skip
 *   2. For each dependency, recursively resolve first
 *   3. If a dependency is not loaded, try auto-loading via request_module()
 *   4. If auto-load fails, return error
 *
 * Parameters:
 *   @mod_name:  The name of the module whose dependencies to resolve
 *   @err_buf:   Buffer for error messages
 *   @err_len:   Size of error buffer
 *
 * Returns 0 on success (all dependencies resolved), -1 on failure.
 */
int module_dep_resolve(const char *mod_name, char *err_buf, int err_len)
{
    if (!mod_name || !mod_name[0])
        return 0;

    /* Find the module (may not be loaded yet if we're resolving pre-load) */
    struct kernel_module *mod = module_find(mod_name);

    /* If the module isn't loaded, we can't check its deps — caller should
     * load it first or this is a pre-load check.  For pre-load resolution,
     * the caller passes the dependency list separately. */
    if (!mod)
        return 0;

    /* Check for dependency cycles first */
    const char *stack[MODULE_DEP_MAX_DEPTH];
    stack[0] = mod_name;
    if (module_dep_detect_cycle(mod_name, stack, 1, err_buf, err_len) < 0)
        return -1;

    /* Resolve each dependency recursively */
    for (int i = 0; i < mod->num_deps; i++) {
        const char *dep_name = mod->deps[i].name;

        /* Mark as resolved if already live */
        struct kernel_module *dep_mod = module_find(dep_name);
        if (dep_mod && dep_mod->state == MODULE_LIVE) {
            mod->deps[i].loaded = 1;
            continue;
        }

        /* Try to auto-load the dependency */
        kprintf("[MOD_DEPS] Auto-loading dependency '%s' for '%s'...\n",
                dep_name, mod_name);

        int dep_id = request_module("%s", dep_name);
        if (dep_id < 0) {
            snprintf(err_buf, (size_t)err_len,
                     "dependency '%s' required by '%s' could not be loaded "
                     "(ret=%d).  Is the module available in /modules/?",
                     dep_name, mod_name, dep_id);
            return -1;
        }

        /* Dependency loaded successfully */
        mod->deps[i].loaded = 1;

        /* Recursively resolve the dependency's own dependencies */
        if (module_dep_resolve(dep_name, err_buf, err_len) < 0)
            return -1;
    }

    return 0;
}

/* ── Resolve all dependencies for a given list of dependency strings ─────
 *
 * Called during ELF module loading (before module registration) when we
 * have the depends string from .modinfo but the module isn't registered yet.
 *
 * @dep_names:    Comma-separated list of dependency names (or NULL)
 * @err_buf:      Buffer for error messages
 * @err_len:      Size of error buffer
 *
 * Returns 0 on success, -1 on failure.
 */
int module_dep_resolve_list(const char *dep_names,
                             char *err_buf, int err_len)
{
    if (!dep_names || !dep_names[0])
        return 0;

    /* Make a writable copy of the depends string */
    char deps[256];
    strncpy(deps, dep_names, sizeof(deps) - 1);
    deps[sizeof(deps) - 1] = '\0';

    /* Parse comma-separated dependencies */
    char *saveptr = NULL;
    char *token = strtok_r(deps, ",", &saveptr);
    while (token) {
        /* Skip leading whitespace */
        while (*token == ' ' || *token == '\t')
            token++;

        if (*token == '\0') {
            token = strtok_r(NULL, ",", &saveptr);
            continue;
        }

        kprintf("[MOD_DEPS] Resolving dependency '%s'...\n", token);

        /* Check if already loaded */
        struct kernel_module *dep_mod = module_find(token);
        if (dep_mod && dep_mod->state == MODULE_LIVE) {
            token = strtok_r(NULL, ",", &saveptr);
            continue;
        }

        /* Try to auto-load */
        int dep_id = request_module("%s", token);
        if (dep_id < 0) {
            snprintf(err_buf, (size_t)err_len,
                     "dependency '%s' could not be loaded (ret=%d)",
                     token, dep_id);
            return -1;
        }

        /* Recursively resolve the dependency's own dependencies */
        if (module_dep_resolve(token, err_buf, err_len) < 0)
            return -1;

        token = strtok_r(NULL, ",", &saveptr);
    }

    return 0;
}

/* ── Check whether all dependencies of @mod are loaded ────────────────
 * Wrapper around module_deps_resolved() with improved error reporting.
 */
int module_dep_check_resolved(struct kernel_module *mod,
                               char *err_buf, int err_len)
{
    if (!mod)
        return 0;

    if (module_deps_resolved(mod))
        return 0; /* all resolved */

    /* Find the first unresolved dependency and report it */
    for (int i = 0; i < mod->num_deps; i++) {
        if (!mod->deps[i].loaded) {
            snprintf(err_buf, (size_t)err_len,
                     "module '%s': dependency '%s' not loaded",
                     mod->name, mod->deps[i].name);
            return -1;
        }
    }

    return 0; /* no deps means "resolved" */
}
