/*
 * scheduler_policy.c — Resource quotas, limits, priority, affinity (C96–C100)
 *
 * Implements:
 *   C96: Resource quotas — namespace-level limits
 *   C97: Limit ranges — default/min/max resources per container
 *   C98: Pod priority and preemption
 *   C99: Pod affinity and anti-affinity
 *   C100: Taints and tolerations — node specialization
 */

#define KERNEL_INTERNAL
#include "container.h"
#include "orch_api.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"

/* ═══════════════════════════════════════════════════════════════════════
 *  C96: Resource quotas — namespace-level limits
 * ═══════════════════════════════════════════════════════════════════════ */

#define MAX_QUOTAS            16
#define NAMESPACE_NAME_MAX    64

/* Resource quota for a namespace */
struct resource_quota {
    char   in_use;
    char   namespace[NAMESPACE_NAME_MAX];
    uint64_t max_pods;
    uint64_t max_containers;
    uint64_t max_cpu_cores;       /* In millicores (1 core = 1000) */
    uint64_t max_memory_bytes;
    uint64_t max_volume_count;
    /* Current usage (tracked dynamically) */
    uint64_t used_pods;
    uint64_t used_containers;
    uint64_t used_cpu_millicores;
    uint64_t used_memory_bytes;
    uint64_t used_volumes;
    spinlock_t lock;
};

static struct resource_quota quota_table[MAX_QUOTAS];

/* C96: Set resource quota for a namespace */
int quota_set(const char *namespace, uint64_t max_pods, uint64_t max_containers,
              uint64_t max_cpu_millicores, uint64_t max_memory_bytes,
              uint64_t max_volumes)
{
    if (!namespace) return -EINVAL;

    for (int i = 0; i < MAX_QUOTAS; i++) {
        if (quota_table[i].in_use && strcmp(quota_table[i].namespace, namespace) == 0) {
            /* Update existing */
            quota_table[i].max_pods = max_pods;
            quota_table[i].max_containers = max_containers;
            quota_table[i].max_cpu_cores = max_cpu_millicores;
            quota_table[i].max_memory_bytes = max_memory_bytes;
            quota_table[i].max_volume_count = max_volumes;
            return 0;
        }
    }

    /* Create new */
    for (int i = 0; i < MAX_QUOTAS; i++) {
        if (!quota_table[i].in_use) {
            struct resource_quota *q = &quota_table[i];
            strncpy(q->namespace, namespace, sizeof(q->namespace) - 1);
            q->max_pods = max_pods;
            q->max_containers = max_containers;
            q->max_cpu_cores = max_cpu_millicores;
            q->max_memory_bytes = max_memory_bytes;
            q->max_volume_count = max_volumes;
            q->used_pods = 0;
            q->used_containers = 0;
            q->used_cpu_millicores = 0;
            q->used_memory_bytes = 0;
            q->used_volumes = 0;
            q->in_use = 1;
            kprintf("[Quota] Set quota for namespace '%s': pods=%llu, cpu=%llu, mem=%llu\n",
                    namespace, (unsigned long long)max_pods,
                    (unsigned long long)max_cpu_millicores,
                    (unsigned long long)max_memory_bytes);
            return 0;
        }
    }
    return -ENOSPC;
}

/* C96: Check if creating a resource would exceed quota */
int quota_check(const char *namespace, uint64_t cpu_millicores,
                uint64_t memory_bytes, int is_pod, int is_volume)
{
    if (!namespace) return 0; /* No namespace = no quota */

    for (int i = 0; i < MAX_QUOTAS; i++) {
        if (!quota_table[i].in_use || strcmp(quota_table[i].namespace, namespace) != 0)
            continue;

        struct resource_quota *q = &quota_table[i];
        spinlock_acquire(&q->lock);

        if (is_pod && q->used_pods >= q->max_pods) {
            spinlock_release(&q->lock);
            return -ENOSPC; /* "quota: pods exceeded" */
        }
        if (is_volume && q->used_volumes >= q->max_volume_count) {
            spinlock_release(&q->lock);
            return -ENOSPC;
        }
        if (q->used_containers >= q->max_containers) {
            spinlock_release(&q->lock);
            return -ENOSPC;
        }
        if (q->used_cpu_millicores + cpu_millicores > q->max_cpu_cores) {
            spinlock_release(&q->lock);
            return -ENOSPC;
        }
        if (q->used_memory_bytes + memory_bytes > q->max_memory_bytes) {
            spinlock_release(&q->lock);
            return -ENOMEM;
        }

        spinlock_release(&q->lock);
        return 0; /* OK */
    }
    return 0; /* No quota set for namespace — allowed */
}

/* C96: Account for resource usage */
int quota_account(const char *namespace, uint64_t cpu_millicores,
                  uint64_t memory_bytes, int is_pod, int is_volume, int add)
{
    if (!namespace) return 0;

    for (int i = 0; i < MAX_QUOTAS; i++) {
        if (!quota_table[i].in_use || strcmp(quota_table[i].namespace, namespace) != 0)
            continue;

        struct resource_quota *q = &quota_table[i];
        spinlock_acquire(&q->lock);
        // int64_t delta = add ? 1 : -1;
        q->used_containers += (uint64_t)(add ? 1 : 0);
        if (is_pod) q->used_pods += (uint64_t)(add ? 1 : 0);
        if (is_volume) q->used_volumes += (uint64_t)(add ? 1 : 0);
        q->used_cpu_millicores += add ? (int64_t)cpu_millicores : -(int64_t)cpu_millicores;
        q->used_memory_bytes += add ? (int64_t)memory_bytes : -(int64_t)memory_bytes;
        spinlock_release(&q->lock);
        return 0;
    }
    return 0;
}

/* C96: Delete quota for a namespace */
int quota_delete(const char *namespace)
{
    if (!namespace) return -EINVAL;
    for (int i = 0; i < MAX_QUOTAS; i++) {
        if (quota_table[i].in_use && strcmp(quota_table[i].namespace, namespace) == 0) {
            quota_table[i].in_use = 0;
            return 0;
        }
    }
    return -ENOENT;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C97: Limit ranges — default/min/max resources per container
 * ═══════════════════════════════════════════════════════════════════════ */

#define MAX_LIMIT_RANGES    16

struct limit_range {
    char     in_use;
    char     namespace[NAMESPACE_NAME_MAX];
    uint64_t default_cpu_millicores;
    uint64_t default_memory_bytes;
    uint64_t min_cpu_millicores;
    uint64_t min_memory_bytes;
    uint64_t max_cpu_millicores;
    uint64_t max_memory_bytes;
};

static struct limit_range limit_ranges[MAX_LIMIT_RANGES];
static int lr_count = 0;

/* C97: Set limit range for a namespace */
int limitrange_set(const char *namespace,
                   uint64_t def_cpu, uint64_t def_mem,
                   uint64_t min_cpu, uint64_t min_mem,
                   uint64_t max_cpu, uint64_t max_mem)
{
    if (!namespace) return -EINVAL;

    for (int i = 0; i < lr_count; i++) {
        if (strcmp(limit_ranges[i].namespace, namespace) == 0) {
            /* Update */
            limit_ranges[i].default_cpu_millicores = def_cpu;
            limit_ranges[i].default_memory_bytes = def_mem;
            limit_ranges[i].min_cpu_millicores = min_cpu;
            limit_ranges[i].min_memory_bytes = min_mem;
            limit_ranges[i].max_cpu_millicores = max_cpu;
            limit_ranges[i].max_memory_bytes = max_mem;
            return 0;
        }
    }

    if (lr_count >= MAX_LIMIT_RANGES) return -ENOSPC;
    struct limit_range *lr = &limit_ranges[lr_count++];
    strncpy(lr->namespace, namespace, sizeof(lr->namespace) - 1);
    lr->default_cpu_millicores = def_cpu;
    lr->default_memory_bytes = def_mem;
    lr->min_cpu_millicores = min_cpu;
    lr->min_memory_bytes = min_mem;
    lr->max_cpu_millicores = max_cpu;
    lr->max_memory_bytes = max_mem;
    lr->in_use = 1;
    return 0;
}

/* C97: Apply limit range to a container's resource spec */
int limitrange_apply(const char *namespace,
                     uint64_t *cpu_millicores, uint64_t *memory_bytes)
{
    if (!namespace || !cpu_millicores || !memory_bytes) return -EINVAL;

    for (int i = 0; i < lr_count; i++) {
        if (strcmp(limit_ranges[i].namespace, namespace) != 0) continue;
        struct limit_range *lr = &limit_ranges[i];

        /* Apply defaults if unset */
        if (*cpu_millicores == 0) *cpu_millicores = lr->default_cpu_millicores;
        if (*memory_bytes == 0)   *memory_bytes = lr->default_memory_bytes;

        /* Clamp to limits */
        if (*cpu_millicores < lr->min_cpu_millicores)
            *cpu_millicores = lr->min_cpu_millicores;
        if (*memory_bytes < lr->min_memory_bytes)
            *memory_bytes = lr->min_memory_bytes;
        if (*cpu_millicores > lr->max_cpu_millicores) return -EINVAL;
        if (*memory_bytes > lr->max_memory_bytes) return -EINVAL;

        return 0;
    }
    return 0; /* No limit range for namespace */
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C98: Pod priority and preemption
 * ═══════════════════════════════════════════════════════════════════════ */

#define MAX_PRIORITY_CLASSES  16
#define PRIORITY_CLASS_NAME_MAX 64

struct priority_class {
    char   in_use;
    char   name[PRIORITY_CLASS_NAME_MAX];
    int    value;        /* Higher value = higher priority */
    int    preemptible;  /* 1 = can be preempted (lower priority) */
};

static struct priority_class priority_classes[MAX_PRIORITY_CLASSES];
static int priority_class_count = 0;

/* C98: Register a priority class */
int priority_class_register(const char *name, int value, int preemptible)
{
    if (!name) return -EINVAL;

    for (int i = 0; i < priority_class_count; i++) {
        if (strcmp(priority_classes[i].name, name) == 0) {
            priority_classes[i].value = value;
            priority_classes[i].preemptible = preemptible;
            return 0;
        }
    }
    if (priority_class_count >= MAX_PRIORITY_CLASSES) return -ENOSPC;

    struct priority_class *pc = &priority_classes[priority_class_count++];
    strncpy(pc->name, name, sizeof(pc->name) - 1);
    pc->value = value;
    pc->preemptible = preemptible;
    pc->in_use = 1;
    return 0;
}

/* C98: Check priority level for a class */
int priority_class_value(const char *name)
{
    if (!name) return 0;
    for (int i = 0; i < priority_class_count; i++) {
        if (strcmp(priority_classes[i].name, name) == 0)
            return priority_classes[i].value;
    }
    return 0; /* Default priority */
}

/* C98: Preempt lower-priority pods to make room */
int preempt_pods(const char *namespace, int priority_value)
{
    /* Find and kill lower-priority pods in the same namespace */
    kprintf("[Scheduler] Preempting pods with priority < %d in namespace '%s'\n",
            priority_value, namespace);
    /* In production, walk pod table, find matching pods, evict gracefully */
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C99: Pod affinity and anti-affinity
 * ═══════════════════════════════════════════════════════════════════════ */

#define MAX_AFFINITY_RULES   16
#define AFFINITY_OP_IN       0
#define AFFINITY_OP_NOT_IN   1
#define AFFINITY_OP_EXISTS   2
#define AFFINITY_OP_DOES_NOT_EXIST 3

/* Topology key constants */
#define LABEL_KEY_MAX       64
#define LABEL_VAL_MAX       128

#define TOPOLOGY_HOST        "kubernetes.io/hostname"
#define TOPOLOGY_ZONE        "topology.kubernetes.io/zone"
#define TOPOLOGY_REGION      "topology.kubernetes.io/region"

struct affinity_rule {
    int     type;        /* 0 = affinity, 1 = anti-affinity */
    int     required;    /* 1 = hard requirement, 0 = preferred */
    char    key[LABEL_KEY_MAX];
    char    value[LABEL_VAL_MAX];
    int     op;          /* AFFINITY_OP_IN, _NOT_IN, _EXISTS, _DOES_NOT_EXIST */
    char    topology_key[LABEL_KEY_MAX];
};

/* C99: Check affinity constraints for a pod on a candidate node */
int affinity_check(struct affinity_rule *rules, int num_rules,
                   const char *candidate_node_labels,
                   int *score_out)
{
    if (!rules || !score_out) return -EINVAL;

    int match_count = 0;
    int total_required = 0;

    for (int i = 0; i < num_rules; i++) {
        if (rules[i].type == 0) {
            /* Affinity: prefer co-location with pods matching selector */
            if (rules[i].required) total_required++;
            /* Check if candidate matches */
            if (rules[i].op == AFFINITY_OP_EXISTS) {
                match_count++;
            } else if (strcmp(candidate_node_labels, rules[i].value) == 0) {
                match_count++;
            }
        } else {
            /* Anti-affinity: avoid co-location */
            if (rules[i].required && strcmp(candidate_node_labels, rules[i].value) == 0) {
                return 0; /* Failed hard anti-affinity */
            }
        }
    }

    *score_out = total_required > 0 && match_count >= total_required ? 100 : match_count * 10;
    return 1; /* OK */
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C100: Taints and tolerations
 * ═══════════════════════════════════════════════════════════════════════ */

#define MAX_TAINTS  16
#define TAINT_KEY_MAX 128
#define TAINT_VAL_MAX 256

/* Taint effects */
#define TAINT_EFFECT_NO_SCHEDULE         0
#define TAINT_EFFECT_PREFER_NO_SCHEDULE  1
#define TAINT_EFFECT_NO_EXECUTE          2

struct taint {
    char   key[TAINT_KEY_MAX];
    char   value[TAINT_VAL_MAX];
    int    effect;  /* TAINT_EFFECT_* */
};

struct toleration {
    char   key[TAINT_KEY_MAX];
    char   value[TAINT_VAL_MAX];
    int    effect;
    int    toleration_seconds; /* For NoExecute: how long to tolerate */
};

/* Per-node taints */
static struct taint node_taints[MAX_TAINTS];
static int taint_count = 0;

/* C100: Add taint to a node */
int taint_add(const char *key, const char *value, int effect)
{
    if (!key) return -EINVAL;
    if (taint_count >= MAX_TAINTS) return -ENOSPC;

    /* Check for existing and update */
    for (int i = 0; i < taint_count; i++) {
        if (strcmp(node_taints[i].key, key) == 0) {
            strncpy(node_taints[i].value, value ? value : "", TAINT_VAL_MAX - 1);
            node_taints[i].effect = effect;
            return 0;
        }
    }

    strncpy(node_taints[taint_count].key, key, TAINT_KEY_MAX - 1);
    strncpy(node_taints[taint_count].value, value ? value : "", TAINT_VAL_MAX - 1);
    node_taints[taint_count].effect = effect;
    taint_count++;
    return 0;
}

/* C100: Remove taint from a node */
int taint_remove(const char *key)
{
    if (!key) return -EINVAL;
    for (int i = 0; i < taint_count; i++) {
        if (strcmp(node_taints[i].key, key) == 0) {
            memmove(&node_taints[i], &node_taints[i + 1],
                    (taint_count - i - 1) * sizeof(struct taint));
            taint_count--;
            return 0;
        }
    }
    return -ENOENT;
}

/* C100: Check if a pod tolerates all node taints */
int taints_check_tolerations(struct toleration *tol, int num_tol, int *unschedulable)
{
    if (unschedulable) *unschedulable = 0;

    for (int i = 0; i < taint_count; i++) {
        int tolerated = 0;
        for (int j = 0; j < num_tol; j++) {
            /* Check if toleration matches this taint */
            if (strcmp(tol[j].key, node_taints[i].key) == 0 ||
                strcmp(tol[j].key, "") == 0) {
                tolerated = 1;
                break;
            }
        }
        if (!tolerated) {
            if (node_taints[i].effect == TAINT_EFFECT_NO_SCHEDULE) {
                if (unschedulable) *unschedulable = 1;
                return 0; /* Cannot schedule */
            }
            if (node_taints[i].effect == TAINT_EFFECT_PREFER_NO_SCHEDULE) {
                if (unschedulable) *unschedulable = 1;
                return 0;
            }
        }
    }
    return 1; /* Schedule OK */
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Initialisation
 * ═══════════════════════════════════════════════════════════════════════ */

int scheduler_policy_init(void)
{
    memset(quota_table, 0, sizeof(quota_table));
    memset(limit_ranges, 0, sizeof(limit_ranges));
    memset(priority_classes, 0, sizeof(priority_classes));
    memset(node_taints, 0, sizeof(node_taints));
    lr_count = 0;
    priority_class_count = 0;
    taint_count = 0;

    /* Register default priority classes */
    priority_class_register("system-node-critical", 2000, 1);
    priority_class_register("system-cluster-critical", 1000, 1);
    priority_class_register("default", 0, 1);
    priority_class_register("batch-low", -100, 1);

    kprintf("[SchedulerPolicy] Scheduler policy subsystem initialised\n");
    return 0;
}

/* ── Stub: sched_policy_set ─────────────────────────────── */
int sched_policy_set(const char *cont, const char *policy)
{
    (void)cont;
    (void)policy;
    kprintf("[container] sched_policy_set: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: sched_policy_get ─────────────────────────────── */
int sched_policy_get(const char *cont, char *policy, size_t len)
{
    (void)cont;
    (void)policy;
    (void)len;
    kprintf("[container] sched_policy_get: not yet implemented\n");
    return -ENOSYS;
}
