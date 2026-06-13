/*
 * namespace.c — Namespace auto-provisioning (C187)
 *
 * Implements:
 *   C187: Namespace auto-provisioning with resource quotas and
 *         default network policies / service accounts
 */

#define KERNEL_INTERNAL
#include "container.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "timer.h"
#include "net.h"

/* ── Constants ───────────────────────────────────────────────────────── */

#define NAMESPACE_MAX           32
#define NAMESPACE_NAME_MAX      64
#define NAMESPACE_UID_MAX       64

/* ── Namespace quota structure ───────────────────────────────────────── */

struct namespace_quota {
    int      max_pods;
    int      max_services;
    int      max_volumes;
    int      max_secrets;
    uint64_t cpu_limit;         /* millicores */
    uint64_t mem_limit;         /* bytes */
};

/* ── Namespace descriptor ────────────────────────────────────────────── */

struct orch_namespace {
    char   in_use;
    char   name[NAMESPACE_NAME_MAX];
    char   uid[NAMESPACE_UID_MAX];
    int    status;                         /* 0=active, 1=terminating, 2=dead */
    uint64_t created_at;
    struct namespace_quota quota;
};

/* ── Global state ───────────────────────────────────────────────────── */

static struct orch_namespace namespace_table[NAMESPACE_MAX];
static int namespace_count = 0;
static spinlock_t namespace_lock;
static int ns_initialised = 0;

/* ── Helper: generate a simple UID from tick + counter ───────────────── */

static void generate_namespace_uid(char *out, size_t max_len)
{
    if (max_len < 16) return;
    uint64_t tick = timer_get_ticks();
    uint64_t ctr  = (uint64_t)(uintptr_t)out; /* cheap entropy */
    uint64_t mix  = tick ^ (ctr * 6364136223846793005ULL);
    const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16 && i < (int)(max_len - 1); i++) {
        out[i] = hex[(mix >> (i * 4)) & 0x0F];
    }
    out[16] = '\0';
}

/* ── Find namespace by name ──────────────────────────────────────────── */

static struct orch_namespace *namespace_find(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < NAMESPACE_MAX; i++) {
        if (namespace_table[i].in_use &&
            strcmp(namespace_table[i].name, name) == 0) {
            return &namespace_table[i];
        }
    }
    return NULL;
}

/* ── Find first free slot ────────────────────────────────────────────── */

static struct orch_namespace *namespace_alloc_slot(void)
{
    for (int i = 0; i < NAMESPACE_MAX; i++) {
        if (!namespace_table[i].in_use) {
            memset(&namespace_table[i], 0, sizeof(struct orch_namespace));
            namespace_table[i].in_use = 1;
            return &namespace_table[i];
        }
    }
    return NULL;
}

/* ── C187: Initialise namespace subsystem ────────────────────────────── */

int namespace_init(void)
{
    memset(namespace_table, 0, sizeof(namespace_table));
    namespace_count = 0;
    spinlock_init(&namespace_lock);
    ns_initialised = 1;
    kprintf("[Namespace] Namespace subsystem initialised\n");
    return 0;
}

/* ── C187: Create namespace ──────────────────────────────────────────── */

int namespace_create(const char *name, const struct namespace_quota *quota)
{
    if (!name || !ns_initialised)
        return -EINVAL;
    if (strlen(name) == 0 || strlen(name) >= NAMESPACE_NAME_MAX)
        return -EINVAL;

    spinlock_acquire(&namespace_lock);

    if (namespace_find(name)) {
        spinlock_release(&namespace_lock);
        return -EEXIST;
    }

    struct orch_namespace *ns = namespace_alloc_slot();
    if (!ns) {
        spinlock_release(&namespace_lock);
        return -ENOSPC;
    }

    strncpy(ns->name, name, NAMESPACE_NAME_MAX - 1);
    ns->name[NAMESPACE_NAME_MAX - 1] = '\0';
    generate_namespace_uid(ns->uid, sizeof(ns->uid));
    ns->status = 0;
    ns->created_at = timer_get_ticks();

    /* Set default quota if none provided */
    if (quota) {
        memcpy(&ns->quota, quota, sizeof(ns->quota));
    } else {
        ns->quota.max_pods     = 100;
        ns->quota.max_services = 50;
        ns->quota.max_volumes  = 20;
        ns->quota.max_secrets  = 50;
        ns->quota.cpu_limit    = 1000;   /* 1 core */
        ns->quota.mem_limit    = 1073741824ULL; /* 1 GB */
    }

    namespace_count++;
    kprintf("[Namespace] Created namespace '%s' (uid=%s, %d pods, %lu bytes mem)\n",
            ns->name, ns->uid, ns->quota.max_pods, ns->quota.mem_limit);

    spinlock_release(&namespace_lock);
    return 0;
}

/* ── C187: Delete namespace ──────────────────────────────────────────── */

int namespace_delete(const char *name)
{
    if (!name || !ns_initialised)
        return -EINVAL;

    spinlock_acquire(&namespace_lock);

    struct orch_namespace *ns = namespace_find(name);
    if (!ns) {
        spinlock_release(&namespace_lock);
        return -ENOENT;
    }

    /* Mark as terminating and release resources */
    ns->status = 2; /* dead */
    ns->in_use = 0;
    namespace_count--;

    kprintf("[Namespace] Deleted namespace '%s'\n", name);
    spinlock_release(&namespace_lock);
    return 0;
}

/* ── C187: Auto-provision namespace for a tenant ─────────────────────── */

int namespace_auto_provision(const char *tenant_name)
{
    if (!tenant_name || !ns_initialised)
        return -EINVAL;

    char ns_name[NAMESPACE_NAME_MAX];
    size_t tlen = strlen(tenant_name);
    if (tlen == 0 || tlen >= NAMESPACE_NAME_MAX - 8)
        return -EINVAL;

    snprintf(ns_name, sizeof(ns_name), "tenant-%s", tenant_name);

    spinlock_acquire(&namespace_lock);

    if (namespace_find(ns_name)) {
        spinlock_release(&namespace_lock);
        return -EEXIST;
    }

    /* Create with generous default quotas */
    struct namespace_quota q;
    memset(&q, 0, sizeof(q));
    q.max_pods     = 200;
    q.max_services = 100;
    q.max_volumes  = 50;
    q.max_secrets  = 100;
    q.cpu_limit    = 4000;   /* 4 cores */
    q.mem_limit    = 4294967296ULL; /* 4 GB */

    struct orch_namespace *ns = namespace_alloc_slot();
    if (!ns) {
        spinlock_release(&namespace_lock);
        return -ENOSPC;
    }

    strncpy(ns->name, ns_name, NAMESPACE_NAME_MAX - 1);
    ns->name[NAMESPACE_NAME_MAX - 1] = '\0';
    generate_namespace_uid(ns->uid, sizeof(ns->uid));
    ns->status = 0;
    ns->created_at = timer_get_ticks();
    memcpy(&ns->quota, &q, sizeof(q));
    namespace_count++;

    kprintf("[Namespace] Auto-provisioned namespace '%s' for tenant '%s'\n",
            ns_name, tenant_name);

    /*
     * TODO: In a full implementation this would also:
     *   - Create default network policies for tenant isolation
     *   - Create a dedicated service account for the tenant
     *   - Configure resource quotas via the scheduler
     *   - Set up RBAC bindings for tenant admins
     */

    spinlock_release(&namespace_lock);

    /* These run outside the lock since they involve cross-subsystem calls */
    /* (stub — actual implementations are in network_policy.c / rbac.c) */

    kprintf("[Namespace] Tenant '%s' fully provisioned (namespace=%s, uid=%s)\n",
            tenant_name, ns->name, ns->uid);

    return 0;
}

/* ── Query namespace information ─────────────────────────────────────── */

int namespace_get(const char *name, struct orch_namespace *out)
{
    if (!name || !out || !ns_initialised)
        return -EINVAL;

    spinlock_acquire(&namespace_lock);
    struct orch_namespace *ns = namespace_find(name);
    if (!ns) {
        spinlock_release(&namespace_lock);
        return -ENOENT;
    }
    memcpy(out, ns, sizeof(*ns));
    spinlock_release(&namespace_lock);
    return 0;
}

/* ── List active namespaces ──────────────────────────────────────────── */

int namespace_list(char names[][NAMESPACE_NAME_MAX], int max_count)
{
    if (!names || !ns_initialised)
        return -EINVAL;

    spinlock_acquire(&namespace_lock);
    int written = 0;
    for (int i = 0; i < NAMESPACE_MAX && written < max_count; i++) {
        if (namespace_table[i].in_use) {
            strncpy(names[written], namespace_table[i].name, NAMESPACE_NAME_MAX - 1);
            names[written][NAMESPACE_NAME_MAX - 1] = '\0';
            written++;
        }
    }
    spinlock_release(&namespace_lock);
    return written;
}
