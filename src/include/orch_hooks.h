/*
 * orch_hooks.h — Container lifecycle hooks (Item B30)
 *
 * Defines the lifecycle hook structure and execution API for
 * post-start and pre-stop container lifecycle hooks.
 *
 * Hook types:
 *   HOOK_EXEC — fork+exec a command within the container namespace
 *   HOOK_HTTP — HTTP POST to a URL with retry on failure
 *
 * Hooks are best-effort: if a hook times out or fails, a warning is
 * logged but container lifecycle continues.
 */

#ifndef ORCH_HOOKS_H
#define ORCH_HOOKS_H

#include "types.h"

/* ── Hook type constants ───────────────────────────────────────────── */
#define HOOK_EXEC  0
#define HOOK_HTTP  1

/* ── String size limits ────────────────────────────────────────────── */
#define HOOK_COMMAND_MAX  256
#define HOOK_URL_MAX      512

/* ── Lifecycle hook descriptor ────────────────────────────────────────
 *
 * Describes a single lifecycle hook attached to a container.
 * For EXEC hooks, 'command' holds the full command line to execute
 * inside the container's rootfs.
 * For HTTP hooks, 'http_url' holds the full URL for an HTTP POST
 * request (e.g. "http://10.0.2.15:8080/poststart").
 */
struct lifecycle_hook {
    int   type;                            /* HOOK_EXEC or HOOK_HTTP   */
    char  command[HOOK_COMMAND_MAX];       /* EXEC: command + args     */
    char  http_url[HOOK_URL_MAX];          /* HTTP: POST URL           */
    int   timeout_seconds;                 /* max time for this hook   */
};

/* ── Public API ────────────────────────────────────────────────────── */

/**
 * hooks_run_poststart() — Execute post-start hooks for a container.
 *
 * Called immediately after a container's init process has started.
 * Hooks are executed sequentially; a failure or timeout in one hook
 * does not prevent subsequent hooks from running.
 *
 * @container_id:  Container ID string.
 * @hooks:         Array of lifecycle_hook descriptors.
 * @num_hooks:     Number of hooks in the array.
 *
 * Returns 0 if all hooks completed (including skipped/failed ones),
 * negative errno on invalid arguments.
 */
int hooks_run_poststart(const char *container_id,
                        const struct lifecycle_hook *hooks,
                        int num_hooks);

/**
 * hooks_run_prestop() — Execute pre-stop hooks for a container.
 *
 * Called immediately before a container's init process is signalled.
 * Hooks are executed sequentially; a failure or timeout in one hook
 * does not prevent subsequent hooks from running.
 *
 * @container_id:  Container ID string.
 * @hooks:         Array of lifecycle_hook descriptors.
 * @num_hooks:     Number of hooks in the array.
 *
 * Returns 0 if all hooks completed (including skipped/failed ones),
 * negative errno on invalid arguments.
 */
int hooks_run_prestop(const char *container_id,
                      const struct lifecycle_hook *hooks,
                      int num_hooks);

#endif /* ORCH_HOOKS_H */
