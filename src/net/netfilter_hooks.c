/* netfilter_hooks.c — Netfilter hook registration (standard 5-hook API)
 *
 * Implements a higher-level interface on top of the existing netfilter
 * hook infrastructure.  Provides:
 *   - Proper errno returns (negative errno on failure, 0 on success)
 *   - Per-hook-point convenience registration/unregistration
 *   - Hook count and status tracking
 *   - Initialization of all 5 standard hook chains
 *
 * Hook points (Linux-compatible numbering):
 *   NF_INET_PRE_ROUTING  (0) — before routing decision
 *   NF_INET_LOCAL_IN     (1) — input to local process
 *   NF_INET_FORWARD      (2) — forwarded packet
 *   NF_INET_LOCAL_OUT    (3) — output from local process
 *   NF_INET_POST_ROUTING (4) — after routing decision
 */

#define KERNEL_INTERNAL
#include "netfilter_hooks.h"
#include "netfilter.h"
#include "errno.h"
#include "printf.h"
#include "string.h"

/* ── Hook registration tracking ──────────────────────────────────── */

/* Per-hook-point counters — track how many hooks are registered at
 * each of the 5 standard points.  Used for diagnostics and debugging. */
static int g_hook_count[NF_MAX_HOOKS];

/* ── Core hook management ────────────────────────────────────────── */

int nf_hook_register(int hook, nf_hookfn fn, int priority)
{
    if (hook < 0 || hook >= NF_MAX_HOOKS)
        return -EINVAL;
    if (!fn)
        return -EINVAL;

    int ret = nf_register_hook(hook, fn, priority);
    if (ret != 0)
        return -ENOMEM;

    g_hook_count[hook]++;
    return 0;
}
EXPORT_SYMBOL(nf_hook_register);

void nf_hook_unregister(int hook, nf_hookfn fn)
{
    if (hook < 0 || hook >= NF_MAX_HOOKS)
        return;
    if (!fn)
        return;

    nf_unregister_hook(hook, fn);
    if (g_hook_count[hook] > 0)
        g_hook_count[hook]--;
}
EXPORT_SYMBOL(nf_hook_unregister);

int nf_hook_iterate(int hook, void *skb)
{
    return nf_iterate_hooks(hook, skb);
}
EXPORT_SYMBOL(nf_hook_iterate);

/* ── Query hook registration status ──────────────────────────────── */

int nf_hook_count(int hook)
{
    if (hook < 0 || hook >= NF_MAX_HOOKS)
        return -EINVAL;
    return g_hook_count[hook];
}
EXPORT_SYMBOL(nf_hook_count);

/* ── Initialization / Teardown ───────────────────────────────────── */

void nf_hooks_init(void)
{
    memset(g_hook_count, 0, sizeof(g_hook_count));
    kprintf("[nf-hooks] Netfilter 5-hook chain initialized"
            " (PRE_ROUTING LOCAL_IN FORWARD LOCAL_OUT POST_ROUTING)\n");
}

void nf_hooks_exit(void)
{
    kprintf("[nf-hooks] Netfilter 5-hook chain shutdown\n");
}

#include "module.h"
module_init(nf_hooks_init);
module_exit(nf_hooks_exit);
