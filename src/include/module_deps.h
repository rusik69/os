#ifndef MODULE_DEPS_H
#define MODULE_DEPS_H

/* module_deps.h — Module dependency resolution API (M25)
 *
 * Provides functions to resolve and auto-load module dependencies
 * during the module loading process.
 */

/* Resolve all dependencies for a module that is already registered.
 * Checks each dependency, auto-loads missing ones via request_module(),
 * and recurses to resolve transitive dependencies.
 * Returns 0 on success, -1 with error message in @err_buf.
 */
int module_dep_resolve(const char *mod_name, char *err_buf, int err_len);

/* Resolve dependencies from a comma-separated list of module names
 * (as parsed from "depends=" in .modinfo).  Used when the module
 * hasn't been registered yet (pre-load resolution).
 * Returns 0 on success, -1 with error message in @err_buf.
 */
int module_dep_resolve_list(const char *dep_names,
                             char *err_buf, int err_len);

/* Check whether all dependencies of @mod are loaded.
 * Returns 0 if resolved, -1 with error message if any dep is missing.
 */
int module_dep_check_resolved(struct kernel_module *mod,
                               char *err_buf, int err_len);

#endif /* MODULE_DEPS_H */
