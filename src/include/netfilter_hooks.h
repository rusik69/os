#ifndef NETFILTER_HOOKS_H
#define NETFILTER_HOOKS_H

#include "types.h"
#include "netfilter.h"

/* ── Netfilter hook registration API ────────────────────────────────
 *
 * Provides a higher-level interface for registering and iterating
 * netfilter hooks at the five standard Netfilter hook points:
 *   NF_INET_PRE_ROUTING
 *   NF_INET_LOCAL_IN
 *   NF_INET_FORWARD
 *   NF_INET_LOCAL_OUT
 *   NF_INET_POST_ROUTING
 *
 * Functions return 0 on success or negative errno on failure (Linux
 * kernel convention).  This contrasts with the lower-level
 * nf_register_hook() which returns -1 on error.
 */

/* ── Core hook management ───────────────────────────────────────── */

/* Register a hook function at the given hook point.
 * Returns 0 on success, -EINVAL for invalid args, -ENOMEM on allocation
 * failure. */
int  nf_hook_register(int hook, nf_hookfn fn, int priority);

/* Unregister a previously-registered hook function. */
void nf_hook_unregister(int hook, nf_hookfn fn);

/* Iterate all registered hooks at the given hook point.
 * Returns NF_ACCEPT (0) if all hooks accept, NF_DROP (1) or
 * NF_REJECT (2) if any hook rejects the packet. */
int  nf_hook_iterate(int hook, void *skb);

/* ── Per-hook-point convenience wrappers ─────────────────────────── */

static inline int
nf_hook_register_pre_routing(nf_hookfn fn, int priority)
{
    return nf_hook_register(NF_INET_PRE_ROUTING, fn, priority);
}

static inline int
nf_hook_register_local_in(nf_hookfn fn, int priority)
{
    return nf_hook_register(NF_INET_LOCAL_IN, fn, priority);
}

static inline int
nf_hook_register_forward(nf_hookfn fn, int priority)
{
    return nf_hook_register(NF_INET_FORWARD, fn, priority);
}

static inline int
nf_hook_register_local_out(nf_hookfn fn, int priority)
{
    return nf_hook_register(NF_INET_LOCAL_OUT, fn, priority);
}

static inline int
nf_hook_register_post_routing(nf_hookfn fn, int priority)
{
    return nf_hook_register(NF_INET_POST_ROUTING, fn, priority);
}

/* ── Per-hook-point unregister wrappers ──────────────────────────── */

static inline void
nf_hook_unregister_pre_routing(nf_hookfn fn)
{
    nf_hook_unregister(NF_INET_PRE_ROUTING, fn);
}

static inline void
nf_hook_unregister_local_in(nf_hookfn fn)
{
    nf_hook_unregister(NF_INET_LOCAL_IN, fn);
}

static inline void
nf_hook_unregister_forward(nf_hookfn fn)
{
    nf_hook_unregister(NF_INET_FORWARD, fn);
}

static inline void
nf_hook_unregister_local_out(nf_hookfn fn)
{
    nf_hook_unregister(NF_INET_LOCAL_OUT, fn);
}

static inline void
nf_hook_unregister_post_routing(nf_hookfn fn)
{
    nf_hook_unregister(NF_INET_POST_ROUTING, fn);
}

/* ── Query hook registration status ──────────────────────────────── */

/* Return the number of registered hooks at the given hook point. */
int  nf_hook_count(int hook);

/* ── Initialization ──────────────────────────────────────────────── */

/* Initialize the netfilter hook subsystem.  Must be called before
 * any hooks are registered (typically from netfilter_init). */
void nf_hooks_init(void);

#endif /* NETFILTER_HOOKS_H */
