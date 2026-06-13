/*
 * pod_security.c — Pod security policies (C190)
 *
 * Implements:
 *   C190: Pod Security Policies — admission control for pod security
 *         contexts (privileged, host network, host PID, host IPC)
 */

#define KERNEL_INTERNAL
#include "container.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "timer.h"

/* ── Constants ───────────────────────────────────────────────────────── */

#define POLICIES_MAX            16
#define PSP_NAME_MAX            64
#define ALLOWED_VOLUMES_MAX     8
#define ALLOWED_USERS_MAX       8
#define SECCOMP_PROFILES_MAX    4
#define VOLUME_NAME_MAX         64
#define USER_NAME_MAX           64
#define SECCOMP_NAME_MAX        64

/* ── Pod Security Policy ─────────────────────────────────────────────── */

struct pod_security_policy {
    char   in_use;
    char   name[PSP_NAME_MAX];
    int    privileged;
    int    host_network;
    int    host_pid;
    int    host_ipc;
    char   allowed_volumes[ALLOWED_VOLUMES_MAX][VOLUME_NAME_MAX];
    int    allowed_volume_count;
    char   allowed_users[ALLOWED_USERS_MAX][USER_NAME_MAX];
    int    allowed_user_count;
    char   seccomp_profiles[SECCOMP_PROFILES_MAX][SECCOMP_NAME_MAX];
    int    seccomp_profile_count;
};

/* ── Global state ────────────────────────────────────────────────────── */

static struct pod_security_policy psp_table[POLICIES_MAX];
static int                        psp_count;
static spinlock_t                 psp_lock;
static int                        psp_initialised;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int psp_find_slot(void)
{
    for (int i = 0; i < POLICIES_MAX; i++) {
        if (!psp_table[i].in_use) return i;
    }
    return -1;
}

static struct pod_security_policy *psp_find_by_name(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < POLICIES_MAX; i++) {
        if (psp_table[i].in_use && strcmp(psp_table[i].name, name) == 0)
            return &psp_table[i];
    }
    return NULL;
}

/* ── C190: Initialise security policies ──────────────────────────────── */

int psp_init(void)
{
    memset(psp_table, 0, sizeof(psp_table));
    psp_count = 0;
    psp_initialised = 1;
    kprintf("[PSP] Pod security policy subsystem initialised (max %d policies)\n",
            POLICIES_MAX);
    return 0;
}

/* ── C190: Create a security policy ──────────────────────────────────── */

int psp_create(const char *name, int privileged, int host_network,
               int host_pid, int host_ipc,
               const char *const *allowed_volumes, int allowed_volume_count,
               const char *const *allowed_users, int allowed_user_count,
               const char *const *seccomp_profiles, int seccomp_profile_count)
{
    if (!name || !psp_initialised)
        return -EINVAL;
    if (strlen(name) == 0 || strlen(name) >= PSP_NAME_MAX)
        return -EINVAL;

    spinlock_acquire(&psp_lock);

    if (psp_find_by_name(name)) {
        spinlock_release(&psp_lock);
        return -EEXIST;
    }

    int slot = psp_find_slot();
    if (slot < 0) {
        spinlock_release(&psp_lock);
        return -ENOSPC;
    }

    struct pod_security_policy *p = &psp_table[slot];
    p->in_use = 1;
    strncpy(p->name, name, PSP_NAME_MAX - 1);
    p->name[PSP_NAME_MAX - 1] = '\0';
    p->privileged   = privileged;
    p->host_network = host_network;
    p->host_pid     = host_pid;
    p->host_ipc     = host_ipc;

    /* Copy allowed volumes */
    p->allowed_volume_count = 0;
    if (allowed_volumes && allowed_volume_count > 0) {
        int n = allowed_volume_count < ALLOWED_VOLUMES_MAX
                ? allowed_volume_count : ALLOWED_VOLUMES_MAX;
        for (int i = 0; i < n; i++) {
            strncpy(p->allowed_volumes[i], allowed_volumes[i], VOLUME_NAME_MAX - 1);
            p->allowed_volumes[i][VOLUME_NAME_MAX - 1] = '\0';
        }
        p->allowed_volume_count = n;
    }

    /* Copy allowed users */
    p->allowed_user_count = 0;
    if (allowed_users && allowed_user_count > 0) {
        int n = allowed_user_count < ALLOWED_USERS_MAX
                ? allowed_user_count : ALLOWED_USERS_MAX;
        for (int i = 0; i < n; i++) {
            strncpy(p->allowed_users[i], allowed_users[i], USER_NAME_MAX - 1);
            p->allowed_users[i][USER_NAME_MAX - 1] = '\0';
        }
        p->allowed_user_count = n;
    }

    /* Copy seccomp profiles */
    p->seccomp_profile_count = 0;
    if (seccomp_profiles && seccomp_profile_count > 0) {
        int n = seccomp_profile_count < SECCOMP_PROFILES_MAX
                ? seccomp_profile_count : SECCOMP_PROFILES_MAX;
        for (int i = 0; i < n; i++) {
            strncpy(p->seccomp_profiles[i], seccomp_profiles[i], SECCOMP_NAME_MAX - 1);
            p->seccomp_profiles[i][SECCOMP_NAME_MAX - 1] = '\0';
        }
        p->seccomp_profile_count = n;
    }

    psp_count++;
    kprintf("[PSP] Created security policy '%s' (priv=%d, net=%d, pid=%d, ipc=%d)\n",
            p->name, p->privileged, p->host_network, p->host_pid, p->host_ipc);

    spinlock_release(&psp_lock);
    return 0;
}

/* ── C190: Validate a pod spec against the active policy ─────────────── */

int psp_validate(int privileged, int host_network, int host_pid)
{
    if (!psp_initialised)
        return -EINVAL;

    spinlock_acquire(&psp_lock);

    /* Find the most restrictive policy (first one that applies) */
    /* In a full implementation, this would use pod labels / user info
     * to select the appropriate policy.  For now, we validate against
     * all active policies and require at least one to allow the spec. */
    int allowed_by_any = 0;

    for (int i = 0; i < POLICIES_MAX; i++) {
        if (!psp_table[i].in_use)
            continue;

        struct pod_security_policy *p = &psp_table[i];

        /* If privileged is requested, policy must allow it */
        if (privileged && !p->privileged)
            continue;

        /* If host_network is requested, policy must allow it */
        if (host_network && !p->host_network)
            continue;

        /* If host_pid is requested, policy must allow it */
        if (host_pid && !p->host_pid)
            continue;

        /* This policy allows the spec */
        allowed_by_any = 1;
        break;
    }

    spinlock_release(&psp_lock);

    if (!allowed_by_any) {
        kprintf("[PSP] Pod spec denied: priv=%d net=%d pid=%d\n",
                privileged, host_network, host_pid);
        return -EPERM;
    }

    return 0;
}

/* ── C190: Admission control — full pod spec validation ──────────────── */

int psp_admit(int privileged, int host_network, int host_pid,
              int host_ipc, const char *run_as_user)
{
    if (!psp_initialised)
        return -EINVAL;

    spinlock_acquire(&psp_lock);

    int admitted = 0;

    for (int i = 0; i < POLICIES_MAX; i++) {
        if (!psp_table[i].in_use)
            continue;

        struct pod_security_policy *p = &psp_table[i];

        /* Check privileged */
        if (privileged && !p->privileged)
            continue;

        /* Check host network */
        if (host_network && !p->host_network)
            continue;

        /* Check host PID */
        if (host_pid && !p->host_pid)
            continue;

        /* Check host IPC */
        if (host_ipc && !p->host_ipc)
            continue;

        /* Check run_as_user against allowed users */
        if (run_as_user && p->allowed_user_count > 0) {
            int user_found = 0;
            for (int u = 0; u < p->allowed_user_count; u++) {
                if (strcmp(p->allowed_users[u], "*") == 0 ||
                    strcmp(p->allowed_users[u], run_as_user) == 0) {
                    user_found = 1;
                    break;
                }
            }
            if (!user_found)
                continue;
        }

        /* All checks passed — admitted */
        admitted = 1;
        break;
    }

    spinlock_release(&psp_lock);

    if (!admitted) {
        kprintf("[PSP] Admission denied: priv=%d net=%d pid=%d ipc=%d user=%s\n",
                privileged, host_network, host_pid, host_ipc,
                run_as_user ? run_as_user : "(none)");
        return -EPERM;
    }

    kprintf("[PSP] Pod admitted: priv=%d net=%d pid=%d ipc=%d\n",
            privileged, host_network, host_pid, host_ipc);
    return 0;
}

/* ── Utility: delete a security policy ───────────────────────────────── */

int psp_delete(const char *name)
{
    if (!name || !psp_initialised)
        return -EINVAL;

    spinlock_acquire(&psp_lock);

    struct pod_security_policy *p = psp_find_by_name(name);
    if (!p) {
        spinlock_release(&psp_lock);
        return -ENOENT;
    }

    p->in_use = 0;
    psp_count--;

    kprintf("[PSP] Deleted security policy '%s'\n", name);
    spinlock_release(&psp_lock);
    return 0;
}
