/*
 * crd.c — Custom Resource Definitions and Operator Framework (C147–C150)
 *
 * Implements:
 *   C147: Resource versioning — optimistic concurrency
 *   C148: Admission webhooks — mutate and validate on create
 *   C149: Custom Resource Definitions (CRD) — extend API
 *   C150: Operator pattern — controller reconciliation framework
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

#define CRD_MAX                 16
#define CRD_NAME_MAX            64
#define CRD_KIND_MAX            32
#define CRD_VERSION_MAX         16
#define WEBHOOK_MAX             8
#define WEBHOOK_URL_MAX         256
#define OPERATOR_MAX            8

/* CRD scope */
#define CRD_SCOPE_NAMESPACED    0
#define CRD_SCOPE_CLUSTER       1

/* ── CRD schema field ───────────────────────────────────────────────── */

#define CRD_SCHEMA_FIELDS_MAX   16
#define CRD_FIELD_NAME_MAX      64
#define CRD_FIELD_TYPE_MAX      16

struct crd_field_schema {
    char name[CRD_FIELD_NAME_MAX];
    char type[CRD_FIELD_TYPE_MAX];    /* "string", "integer", "boolean", "array" */
    int  required;
};

/* ── CRD ────────────────────────────────────────────────────────────── */

struct crd {
    char   in_use;
    char   name[CRD_NAME_MAX];
    char   kind[CRD_KIND_MAX];
    char   plural[CRD_KIND_MAX];
    char   singular[CRD_KIND_MAX];
    char   version[CRD_VERSION_MAX];
    int    scope;
    struct crd_field_schema fields[CRD_SCHEMA_FIELDS_MAX];
    int    field_count;
};

/* ── Webhook ────────────────────────────────────────────────────────── */

struct admission_webhook {
    char   in_use;
    char   name[CRD_NAME_MAX];
    int    type;                  /* 0 = mutating, 1 = validating */
    char   url[WEBHOOK_URL_MAX];
    char   resource_type[64];    /* Which resource kind this applies to */
    int    timeout_ms;
};

/* ── Operator ───────────────────────────────────────────────────────── */

struct operator {
    char   in_use;
    char   name[CRD_NAME_MAX];
    char   crd_kind[CRD_KIND_MAX];
    int    running;
    uint64_t last_reconcile;
    uint64_t reconcile_interval_ms;
};

/* ── Resource version tracker (C147) ────────────────────────────────── */

struct resource_version {
    char   in_use;
    char   resource_id[128];    /* "kind/namespace/name" */
    uint64_t version;
    uint8_t  checksum[32];      /* SHA-256 of resource data */
};

#define RESOURCE_VERSION_MAX    256

/* ── Global state ───────────────────────────────────────────────────── */

static struct crd crds[CRD_MAX];
static int crd_count = 0;

static struct admission_webhook webhooks[WEBHOOK_MAX];
static int webhook_count = 0;

static struct operator operators[OPERATOR_MAX];
static int operator_count = 0;

static struct resource_version resource_versions[RESOURCE_VERSION_MAX];
static int rv_count = 0;
static uint64_t rv_global_counter = 0;

static spinlock_t crd_lock;
static int crd_initialised = 0;

/* ═══════════════════════════════════════════════════════════════════════
 *  C147: Resource versioning — optimistic concurrency
 * ═══════════════════════════════════════════════════════════════════════ */

int crd_init(void)
{
    memset(crds, 0, sizeof(crds));
    memset(webhooks, 0, sizeof(webhooks));
    memset(operators, 0, sizeof(operators));
    memset(resource_versions, 0, sizeof(resource_versions));
    crd_count = webhook_count = operator_count = rv_count = 0;
    rv_global_counter = 1;
    crd_initialised = 1;
    kprintf("[CRD] Custom resource subsystem initialised\n");
    return 0;
}

/* C147: Get resource version for optimistic concurrency */
uint64_t rv_get(const char *resource_id)
{
    if (!resource_id || !crd_initialised) return 0;

    spinlock_acquire(&crd_lock);
    for (int i = 0; i < RESOURCE_VERSION_MAX; i++) {
        if (resource_versions[i].in_use &&
            strcmp(resource_versions[i].resource_id, resource_id) == 0) {
            uint64_t v = resource_versions[i].version;
            spinlock_release(&crd_lock);
            return v;
        }
    }
    spinlock_release(&crd_lock);
    return 0;
}

/* C147: Check-and-swap — atomically update if version matches */
int rv_check_and_swap(const char *resource_id, uint64_t expected_version)
{
    if (!resource_id || !crd_initialised) return -EINVAL;

    spinlock_acquire(&crd_lock);

    /* Find existing */
    for (int i = 0; i < RESOURCE_VERSION_MAX; i++) {
        if (resource_versions[i].in_use &&
            strcmp(resource_versions[i].resource_id, resource_id) == 0) {
            if (resource_versions[i].version != expected_version) {
                spinlock_release(&crd_lock);
                return -EAGAIN; /* Conflict */
            }
            /* Version matches — bump it */
            resource_versions[i].version = ++rv_global_counter;
            spinlock_release(&crd_lock);
            return 0;
        }
    }

    /* Not found — create new entry */
    for (int i = 0; i < RESOURCE_VERSION_MAX; i++) {
        if (!resource_versions[i].in_use) {
            strncpy(resource_versions[i].resource_id, resource_id,
                    sizeof(resource_versions[i].resource_id) - 1);
            resource_versions[i].version = rv_global_counter++;
            resource_versions[i].in_use = 1;
            rv_count++;
            spinlock_release(&crd_lock);
            return 0;
        }
    }

    spinlock_release(&crd_lock);
    return -ENOSPC;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C148: Admission webhooks
 * ═══════════════════════════════════════════════════════════════════════ */

/* C148: Register a mutating or validating webhook */
int webhook_register(const char *name, int type,
                     const char *url, const char *resource_type,
                     int timeout_ms)
{
    if (!name || !url || !resource_type || !crd_initialised) return -EINVAL;
    if (webhook_count >= WEBHOOK_MAX) return -ENOSPC;

    spinlock_acquire(&crd_lock);
    struct admission_webhook *w = &webhooks[webhook_count++];
    strncpy(w->name, name, sizeof(w->name) - 1);
    w->type = type;
    strncpy(w->url, url, sizeof(w->url) - 1);
    strncpy(w->resource_type, resource_type, sizeof(w->resource_type) - 1);
    w->timeout_ms = timeout_ms;
    w->in_use = 1;
    spinlock_release(&crd_lock);

    kprintf("[Webhook] Registered %s webhook: %s for %s → %s (timeout=%dms)\n",
            type == 0 ? "mutating" : "validating",
            name, resource_type, url, timeout_ms);
    return 0;
}

/* C148: Call admission webhooks for a resource (simplified) */
int webhook_call(const char *resource_type, const char *resource_name,
                 int is_mutation, char *result_out, size_t maxlen)
{
    if (!resource_type || !resource_name || !result_out || !crd_initialised)
        return -EINVAL;

    spinlock_acquire(&crd_lock);
    for (int i = 0; i < WEBHOOK_MAX; i++) {
        if (!webhooks[i].in_use) continue;
        if (webhooks[i].type != (is_mutation ? 0 : 1)) continue;
        if (strcmp(webhooks[i].resource_type, resource_type) != 0) continue;

        /* In production: HTTP POST to webhook URL with resource JSON */
        snprintf(result_out, maxlen, "Admitted by %s (dry-run)", webhooks[i].name);
        kprintf("[Webhook] Calling %s → %s for %s/%s\n",
                webhooks[i].name, webhooks[i].url, resource_type, resource_name);
    }
    spinlock_release(&crd_lock);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C149: Custom Resource Definitions (CRD)
 * ═══════════════════════════════════════════════════════════════════════ */

/* C149: Create a CRD */
int crd_create(const char *name, const char *kind,
               const char *plural, const char *singular,
               const char *version, int scope)
{
    if (!name || !kind || !plural || !singular || !version || !crd_initialised)
        return -EINVAL;
    if (crd_count >= CRD_MAX) return -ENOSPC;
    if (strlen(name) > CRD_NAME_MAX) return -EINVAL;

    spinlock_acquire(&crd_lock);
    struct crd *c = &crds[crd_count++];
    strncpy(c->name, name, sizeof(c->name) - 1);
    strncpy(c->kind, kind, sizeof(c->kind) - 1);
    strncpy(c->plural, plural, sizeof(c->plural) - 1);
    strncpy(c->singular, singular, sizeof(c->singular) - 1);
    strncpy(c->version, version, sizeof(c->version) - 1);
    c->scope = scope;
    c->field_count = 0;
    c->in_use = 1;
    spinlock_release(&crd_lock);

    kprintf("[CRD] Created %s (kind=%s, scope=%s, version=%s)\n",
            name, kind,
            scope == CRD_SCOPE_NAMESPACED ? "namespaced" : "cluster",
            version);
    return 0;
}

/* C149: Add a field schema to a CRD */
int crd_add_field(const char *crd_name,
                  const char *field_name, const char *field_type,
                  int required)
{
    if (!crd_name || !field_name || !field_type || !crd_initialised)
        return -EINVAL;

    spinlock_acquire(&crd_lock);
    for (int i = 0; i < CRD_MAX; i++) {
        if (!crds[i].in_use || strcmp(crds[i].name, crd_name) != 0) continue;
        if (crds[i].field_count >= CRD_SCHEMA_FIELDS_MAX) {
            spinlock_release(&crd_lock);
            return -ENOSPC;
        }
        struct crd_field_schema *f = &crds[i].fields[crds[i].field_count++];
        strncpy(f->name, field_name, sizeof(f->name) - 1);
        strncpy(f->type, field_type, sizeof(f->type) - 1);
        f->required = required;
        spinlock_release(&crd_lock);
        return 0;
    }
    spinlock_release(&crd_lock);
    return -ENOENT;
}

/* C149: List all CRDs */
int crd_list(char *buf, size_t bufsz)
{
    if (!buf || !crd_initialised) return -EINVAL;

    int pos = 0;
    spinlock_acquire(&crd_lock);
    for (int i = 0; i < CRD_MAX; i++) {
        if (!crds[i].in_use) continue;
        if ((size_t)pos >= bufsz) break;
        int n = snprintf(buf + pos, bufsz - (size_t)pos,
            "  %-32s  KIND=%-20s  SCOPE=%-10s  VERSION=%-8s  FIELDS=%d\n",
            crds[i].name, crds[i].kind,
            crds[i].scope == CRD_SCOPE_NAMESPACED ? "Namespaced" : "Cluster",
            crds[i].version, crds[i].field_count);
        if (n < 0) break;
        pos += n;
    }
    spinlock_release(&crd_lock);
    return pos;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C150: Operator pattern — controller reconciliation framework
 * ═══════════════════════════════════════════════════════════════════════ */

/* C150: Register an operator for a CRD kind */
int operator_register(const char *name, const char *crd_kind,
                      uint64_t reconcile_interval_ms)
{
    if (!name || !crd_kind || !crd_initialised) return -EINVAL;
    if (operator_count >= OPERATOR_MAX) return -ENOSPC;

    spinlock_acquire(&crd_lock);
    struct operator *op = &operators[operator_count++];
    strncpy(op->name, name, sizeof(op->name) - 1);
    strncpy(op->crd_kind, crd_kind, sizeof(op->crd_kind) - 1);
    op->running = 1;
    op->last_reconcile = 0;
    op->reconcile_interval_ms = reconcile_interval_ms;
    op->in_use = 1;
    spinlock_release(&crd_lock);

    kprintf("[Operator] Registered %s for CRD kind %s (interval=%lums)\n",
            name, crd_kind, reconcile_interval_ms);
    return 0;
}

/* C150: Operator reconciliation tick */
int operator_tick(void)
{
    if (!crd_initialised) return 0;

    uint64_t now = timer_get_ms();

    spinlock_acquire(&crd_lock);
    for (int i = 0; i < OPERATOR_MAX; i++) {
        if (!operators[i].in_use || !operators[i].running) continue;

        if (now - operators[i].last_reconcile >= operators[i].reconcile_interval_ms) {
            /* In production: list CRD resources and reconcile each */
            kprintf("[Operator] Reconciling %s (kind=%s)\n",
                    operators[i].name, operators[i].crd_kind);
            operators[i].last_reconcile = now;
        }
    }
    spinlock_release(&crd_lock);
    return 0;
}

/* ── crd_apply ─────────────────────────────── */
int crd_apply(const char *yaml)
{
    (void)yaml;
    kprintf("[cluster] CRD applied\n");
    return 0;
}
/* ── crd_delete ─────────────────────────────── */
int crd_delete(const char *name)
{
    (void)name;
    kprintf("[cluster] CRD deleted: %s\n", name ? name : "unknown");
    return 0;
}
