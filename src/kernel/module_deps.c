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
 *
 * Topological sort:
 *   Modules are loaded in dependency order using a DFS-based
 *   topological ordering.  When module A depends on B, B is loaded
 *   first (if not already loaded), then A.
 *
 * rmmod dependency check:
 *   A module cannot be unloaded while other loaded modules depend
 *   on it (i.e., list it as a dependency).  module_can_unload()
 *   checks this condition.
 *
 * lsmod dependency trees:
 *   module_dep_print_tree() displays the dependency hierarchy
 *   for diagnostic purposes.
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
            /* Hold a reference to the dependency so it cannot be
             * unloaded while this module depends on it. */
            module_get(dep_mod);
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

        /* Hold a reference to the auto-loaded dependency so it stays
         * alive while this module depends on it. */
        {
            struct kernel_module *dep_mod = module_find(dep_name);
            if (dep_mod)
                module_get(dep_mod);
        }

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

/* ── rmmod dependency checking ─────────────────────────────────────────
 *
 * Check whether any loaded module lists @mod_name as a dependency.
 * If so, the named module cannot be safely unloaded.
 *
 * Returns 0 if the module can be unloaded (no dependents),
 * -1 if dependents exist with a message in @err_buf.
 */
int module_can_unload(const char *mod_name, char *err_buf, int err_len)
{
    if (!mod_name)
        return 0;

    for (int i = 0; i < MODULE_MAX; i++) {
        struct kernel_module *mod = module_get_by_id(i);
        if (!mod || mod->state != MODULE_LIVE)
            continue;

        for (int j = 0; j < mod->num_deps; j++) {
            if (strcmp(mod->deps[j].name, mod_name) == 0) {
                snprintf(err_buf, (size_t)err_len,
                         "cannot unload '%s': '%s' depends on it",
                         mod_name, mod->name);
                return -1;
            }
        }
    }

    return 0;
}

/* ── Dependency tree printing (for lsmod) ───────────────────────────────
 *
 * Print the dependency tree starting from @mod_name, with indentation
 * showing the hierarchy.  Recursively prints each dependency's own
 * dependencies.
 */
void module_dep_print_tree(const char *mod_name, int depth)
{
    if (!mod_name)
        return;

    struct kernel_module *mod = module_find(mod_name);
    if (!mod)
        return;

    /* Print indentation */
    for (int i = 0; i < depth; i++)
        kprintf("  ");

    kprintf("%s", mod_name);

    if (mod->num_deps > 0) {
        kprintf(" [");

        for (int i = 0; i < mod->num_deps; i++) {
            if (i > 0) kprintf(", ");
            kprintf("%s%s", mod->deps[i].name,
                    mod->deps[i].loaded ? "" : " (missing)");
        }
        kprintf("]");
    }
    kprintf("\n");

    /* Recursively print each dependency */
    for (int i = 0; i < mod->num_deps; i++) {
        if (mod->deps[i].loaded) {
            module_dep_print_tree(mod->deps[i].name, depth + 1);
        }
    }
}

/*
 * Topologically sort the loaded module list.
 * Uses DFS-based topological ordering in reverse postorder.
 * The output array @sorted is filled with module pointers in load order
 * (dependencies first).  Returns the number of sorted modules.
 */
int module_dep_topological_sort(struct kernel_module **sorted, int max)
{
    if (!sorted)
        return 0;

    /* Build a list of loaded modules */
    struct kernel_module *loaded[MODULE_MAX];
    int num_loaded = 0;

    for (int i = 0; i < MODULE_MAX; i++) {
        struct kernel_module *mod = module_get_by_id(i);
        if (mod && mod->state == MODULE_LIVE)
            loaded[num_loaded++] = mod;
    }

    /* DFS-based topological sort using simple arrays */
    int visited[MODULE_MAX];
    memset(visited, 0, sizeof(visited));

    int sorted_count = 0;

    /* We run DFS from each unvisited module */
    int stack[MODULE_MAX * 4]; /* (module_idx, dep_idx, state) */
    int stack_pos = 0;

    for (int i = 0; i < num_loaded; i++) {
        if (visited[i])
            continue;

        /* Push initial state: start processing module i */
        /* Use negative values to indicate "postorder" processing */
        stack[stack_pos++] = i;   /* module index */
        stack[stack_pos++] = -1;  /* start: no dependency being processed */

        while (stack_pos > 0) {
            int dep_idx = stack[--stack_pos];
            int mod_idx = stack[--stack_pos];

            if (dep_idx == -2) {
                /* Postorder marker: add this module to sorted list */
                if (sorted_count < max)
                    sorted[sorted_count++] = loaded[mod_idx];
                continue;
            }

            if (visited[mod_idx])
                continue;

            if (dep_idx == -1) {
                /* First visit: start processing dependencies */
                visited[mod_idx] = 1;

                /* Push postorder marker */
                stack[stack_pos++] = mod_idx;
                stack[stack_pos++] = -2;

                /* Push dependencies in reverse order */
                struct kernel_module *mod = loaded[mod_idx];
                for (int j = mod->num_deps - 1; j >= 0; j--) {
                    /* Find the dep module in loaded[] */
                    for (int k = 0; k < num_loaded; k++) {
                        if (strcmp(loaded[k]->name, mod->deps[j].name) == 0 &&
                            !visited[k]) {
                            stack[stack_pos++] = k;
                            stack[stack_pos++] = -1;
                            break;
                        }
                    }
                }
            }
        }
    }

    return sorted_count;
}

/* ── Stub: module_deps_add ─────────────────────────────── */
int module_deps_add(const char *mod, const char *dep)
{
    (void)mod;
    (void)dep;
    kprintf("[moddeps] module_deps_add: not yet implemented\n");
    return 0;
}
/* ── Stub: module_deps_remove ─────────────────────────────── */
int module_deps_remove(const char *mod, const char *dep)
{
    (void)mod;
    (void)dep;
    kprintf("[moddeps] module_deps_remove: not yet implemented\n");
    return 0;
}
/* ── Stub: module_deps_resolve ─────────────────────────────── */
int module_deps_resolve(const char *mod, void *list)
{
    (void)mod;
    (void)list;
    kprintf("[moddeps] module_deps_resolve: not yet implemented\n");
    return 0;
}
