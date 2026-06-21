/*
 * rbac.c — Role-Based Access Control and service accounts (C188–C189)
 *
 * Implements:
 *   C188: RBAC — roles, bindings, and authorization checks
 *   C189: Service accounts — create, retrieve tokens
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

#define ROLES_MAX               16
#define BINDINGS_MAX            32
#define SA_MAX                  16
#define RBAC_NAME_MAX           64
#define RBAC_RESOURCE_MAX       64
#define VERBS_MAX               8
#define VERB_LEN                16
#define SUBJECT_KIND_LEN        16
#define SUBJECT_NAME_LEN        64
#define TOKEN_LEN               128
#define NAMESPACE_LEN           64

/* ── RBAC role ───────────────────────────────────────────────────────── */

struct rbac_role {
    char   in_use;
    char   name[RBAC_NAME_MAX];
    char   resource[RBAC_RESOURCE_MAX];
    char   verbs[VERBS_MAX][VERB_LEN];
    int    verb_count;
};

/* ── RBAC binding (role → subject) ───────────────────────────────────── */

struct rbac_binding {
    char   in_use;
    char   role_name[RBAC_NAME_MAX];
    char   subject_kind[SUBJECT_KIND_LEN]; /* "user", "group", "serviceaccount" */
    char   subject_name[SUBJECT_NAME_LEN];
    char   namespace[NAMESPACE_LEN];
};

/* ── Service account ─────────────────────────────────────────────────── */

struct service_account {
    char   in_use;
    char   name[RBAC_NAME_MAX];
    char   namespace[NAMESPACE_LEN];
    char   token[TOKEN_LEN];
    uint64_t created_at;
};

/* ── Global state ────────────────────────────────────────────────────── */

static struct rbac_role       role_table[ROLES_MAX];
static struct rbac_binding    binding_table[BINDINGS_MAX];
static struct service_account sa_table[SA_MAX];
static int                    role_count;
static int                    binding_count;
static int                    sa_count;
static spinlock_t             rbac_lock;
static int                    rbac_initialised;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int role_find_slot(void)
{
    for (int i = 0; i < ROLES_MAX; i++) {
        if (!role_table[i].in_use) return i;
    }
    return -1;
}

static struct rbac_role *role_find_by_name(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < ROLES_MAX; i++) {
        if (role_table[i].in_use && strcmp(role_table[i].name, name) == 0)
            return &role_table[i];
    }
    return NULL;
}

static int binding_find_slot(void)
{
    for (int i = 0; i < BINDINGS_MAX; i++) {
        if (!binding_table[i].in_use) return i;
    }
    return -1;
}

static int sa_find_slot(void)
{
    for (int i = 0; i < SA_MAX; i++) {
        if (!sa_table[i].in_use) return i;
    }
    return -1;
}

static struct service_account *sa_find(const char *name, const char *namespace)
{
    if (!name || !namespace) return NULL;
    for (int i = 0; i < SA_MAX; i++) {
        if (sa_table[i].in_use &&
            strcmp(sa_table[i].name, name) == 0 &&
            strcmp(sa_table[i].namespace, namespace) == 0)
            return &sa_table[i];
    }
    return NULL;
}

/* Generate a pseudo-random bearer token */
static void sa_generate_token(char *out, size_t max_len)
{
    if (max_len < TOKEN_LEN) return;
    const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    uint64_t tick = timer_get_ticks();
    uint64_t mix  = tick * 6364136223846793005ULL;
    for (int i = 0; i < TOKEN_LEN - 1; i++) {
        mix = mix * 6364136223846793005ULL + 1442695040888963407ULL;
        out[i] = chars[(mix >> (i % 6) * 6) & 0x3F];
    }
    out[TOKEN_LEN - 1] = '\0';
}

/* ── C188: RBAC initialisation ───────────────────────────────────────── */

int rbac_init(void)
{
    memset(role_table, 0, sizeof(role_table));
    memset(binding_table, 0, sizeof(binding_table));
    memset(sa_table, 0, sizeof(sa_table));
    role_count     = 0;
    binding_count  = 0;
    sa_count       = 0;
    rbac_initialised = 1;
    kprintf("[RBAC] Role-based access control initialised (max %d roles, %d bindings, %d SAs)\n",
            ROLES_MAX, BINDINGS_MAX, SA_MAX);
    return 0;
}

/* ── C188: Create a role ─────────────────────────────────────────────── */

int rbac_create_role(const char *name, const char *resource,
                     const char *const *verbs, int verb_count)
{
    if (!name || !resource || !verbs || !rbac_initialised)
        return -EINVAL;
    if (strlen(name) == 0 || strlen(name) >= RBAC_NAME_MAX)
        return -EINVAL;
    if (strlen(resource) >= RBAC_RESOURCE_MAX)
        return -EINVAL;
    if (verb_count <= 0 || verb_count > VERBS_MAX)
        return -EINVAL;

    spinlock_acquire(&rbac_lock);

    if (role_find_by_name(name)) {
        spinlock_release(&rbac_lock);
        return -EEXIST;
    }

    int slot = role_find_slot();
    if (slot < 0) {
        spinlock_release(&rbac_lock);
        return -ENOSPC;
    }

    struct rbac_role *r = &role_table[slot];
    r->in_use = 1;
    strncpy(r->name, name, RBAC_NAME_MAX - 1);
    r->name[RBAC_NAME_MAX - 1] = '\0';
    strncpy(r->resource, resource, RBAC_RESOURCE_MAX - 1);
    r->resource[RBAC_RESOURCE_MAX - 1] = '\0';
    r->verb_count = verb_count;

    for (int i = 0; i < verb_count; i++) {
        strncpy(r->verbs[i], verbs[i], VERB_LEN - 1);
        r->verbs[i][VERB_LEN - 1] = '\0';
    }

    role_count++;
    kprintf("[RBAC] Created role '%s' on resource '%s' (%d verbs)\n",
            r->name, r->resource, r->verb_count);

    spinlock_release(&rbac_lock);
    return 0;
}

/* ── C188: Create a binding (role → subject) ─────────────────────────── */

int rbac_create_binding(const char *name, const char *role,
                         const char *subject_kind, const char *subject_name,
                         const char *namespace)
{
    if (!name || !role || !subject_kind || !subject_name || !rbac_initialised)
        return -EINVAL;

    spinlock_acquire(&rbac_lock);

    /* Verify role exists */
    if (!role_find_by_name(role)) {
        spinlock_release(&rbac_lock);
        return -ENOENT;
    }

    int slot = binding_find_slot();
    if (slot < 0) {
        spinlock_release(&rbac_lock);
        return -ENOSPC;
    }

    struct rbac_binding *b = &binding_table[slot];
    b->in_use = 1;
    strncpy(b->role_name, role, RBAC_NAME_MAX - 1);
    b->role_name[RBAC_NAME_MAX - 1] = '\0';
    strncpy(b->subject_kind, subject_kind, SUBJECT_KIND_LEN - 1);
    b->subject_kind[SUBJECT_KIND_LEN - 1] = '\0';
    strncpy(b->subject_name, subject_name, SUBJECT_NAME_LEN - 1);
    b->subject_name[SUBJECT_NAME_LEN - 1] = '\0';
    if (namespace) {
        strncpy(b->namespace, namespace, NAMESPACE_LEN - 1);
        b->namespace[NAMESPACE_LEN - 1] = '\0';
    } else {
        b->namespace[0] = '\0';
    }

    binding_count++;
    kprintf("[RBAC] Bound role '%s' to %s:%s (namespace=%s)\n",
            b->role_name, b->subject_kind, b->subject_name,
            b->namespace[0] ? b->namespace : "(cluster)");

    spinlock_release(&rbac_lock);
    return 0;
}

/* ── C188: Authorize a subject for a resource+verb ───────────────────── */

int rbac_authorize(const char *subject_kind, const char *subject_name,
                   const char *resource, const char *verb,
                   const char *namespace)
{
    if (!subject_kind || !subject_name || !resource || !verb || !rbac_initialised)
        return -EINVAL;

    spinlock_acquire(&rbac_lock);

    /* Iterate all bindings matching this subject */
    for (int i = 0; i < BINDINGS_MAX; i++) {
        if (!binding_table[i].in_use)
            continue;

        struct rbac_binding *b = &binding_table[i];

        /* Check subject match */
        if (strcmp(b->subject_kind, subject_kind) != 0)
            continue;
        if (strcmp(b->subject_name, subject_name) != 0)
            continue;

        /* Check namespace match (empty = cluster-wide) */
        if (b->namespace[0] != '\0' && namespace &&
            strcmp(b->namespace, namespace) != 0)
            continue;
        if (b->namespace[0] != '\0' && !namespace)
            continue;

        /* Look up the role */
        struct rbac_role *r = role_find_by_name(b->role_name);
        if (!r)
            continue;

        /* Check resource match */
        /* A role resource of "*" matches everything */
        if (strcmp(r->resource, "*") != 0 &&
            strcmp(r->resource, resource) != 0)
            continue;

        /* Check verb match */
        for (int v = 0; v < r->verb_count; v++) {
            if (strcmp(r->verbs[v], "*") == 0 ||
                strcmp(r->verbs[v], verb) == 0) {
                spinlock_release(&rbac_lock);
                return 0; /* authorized */
            }
        }
    }

    spinlock_release(&rbac_lock);
    return -EACCES;
}

/* ── C189: Create service account ────────────────────────────────────── */

int sa_create(const char *name, const char *namespace)
{
    if (!name || !namespace || !rbac_initialised)
        return -EINVAL;
    if (strlen(name) == 0 || strlen(name) >= RBAC_NAME_MAX)
        return -EINVAL;
    if (strlen(namespace) >= NAMESPACE_LEN)
        return -EINVAL;

    spinlock_acquire(&rbac_lock);

    if (sa_find(name, namespace)) {
        spinlock_release(&rbac_lock);
        return -EEXIST;
    }

    int slot = sa_find_slot();
    if (slot < 0) {
        spinlock_release(&rbac_lock);
        return -ENOSPC;
    }

    struct service_account *sa = &sa_table[slot];
    sa->in_use = 1;
    strncpy(sa->name, name, RBAC_NAME_MAX - 1);
    sa->name[RBAC_NAME_MAX - 1] = '\0';
    strncpy(sa->namespace, namespace, NAMESPACE_LEN - 1);
    sa->namespace[NAMESPACE_LEN - 1] = '\0';
    sa->created_at = timer_get_ticks();

    sa_generate_token(sa->token, sizeof(sa->token));
    sa_count++;

    kprintf("[RBAC] Created service account '%s' in namespace '%s'\n",
            sa->name, sa->namespace);

    spinlock_release(&rbac_lock);
    return 0;
}

/* ── C189: Get service account token ─────────────────────────────────── */

int sa_get_token(const char *name, const char *namespace,
                 char *out, size_t max_len)
{
    if (!name || !namespace || !out || !rbac_initialised)
        return -EINVAL;

    spinlock_acquire(&rbac_lock);

    struct service_account *sa = sa_find(name, namespace);
    if (!sa) {
        spinlock_release(&rbac_lock);
        return -ENOENT;
    }

    size_t tlen = strlen(sa->token);
    if (tlen >= max_len) {
        spinlock_release(&rbac_lock);
        return -ENOSPC;
    }

    strncpy(out, sa->token, max_len - 1);
    out[max_len - 1] = '\0';

    spinlock_release(&rbac_lock);
    return 0;
}

/* ── C189: Delete service account ────────────────────────────────────── */

int sa_delete(const char *name, const char *namespace)
{
    if (!name || !namespace || !rbac_initialised)
        return -EINVAL;

    spinlock_acquire(&rbac_lock);

    struct service_account *sa = sa_find(name, namespace);
    if (!sa) {
        spinlock_release(&rbac_lock);
        return -ENOENT;
    }

    memset(sa->token, 0, sizeof(sa->token));
    sa->in_use = 0;
    sa_count--;

    kprintf("[RBAC] Deleted service account '%s' in namespace '%s'\n",
            name, namespace);

    spinlock_release(&rbac_lock);
    return 0;
}

/* ── Utility: list roles ─────────────────────────────────────────────── */

int rbac_list_roles(char names[][RBAC_NAME_MAX], int max_count)
{
    if (!names || !rbac_initialised)
        return -EINVAL;

    spinlock_acquire(&rbac_lock);
    int written = 0;
    for (int i = 0; i < ROLES_MAX && written < max_count; i++) {
        if (role_table[i].in_use) {
            strncpy(names[written], role_table[i].name, RBAC_NAME_MAX - 1);
            names[written][RBAC_NAME_MAX - 1] = '\0';
            written++;
        }
    }
    spinlock_release(&rbac_lock);
    return written;
}

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Stub: rbac_check_permission ───────────────────── */
int rbac_check_permission(const char *subject, const char *resource, const char *verb)
{
    (void)subject;
    (void)resource;
    (void)verb;
    kprintf("[RBAC] rbac_check_permission: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: rbac_add_role ───────────────────────────── */
int rbac_add_role(const char *name, const char *resource, const char *const *verbs, int verb_count)
{
    (void)name;
    (void)resource;
    (void)verbs;
    (void)verb_count;
    kprintf("[RBAC] rbac_add_role: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: rbac_remove_role ────────────────────────── */
int rbac_remove_role(const char *name)
{
    (void)name;
    kprintf("[RBAC] rbac_remove_role: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: rbac_add_binding ────────────────────────── */
int rbac_add_binding(const char *role_name, const char *subject_kind, const char *subject_name)
{
    (void)role_name;
    (void)subject_kind;
    (void)subject_name;
    kprintf("[RBAC] rbac_add_binding: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: rbac_remove_binding ─────────────────────── */
int rbac_remove_binding(const char *role_name, const char *subject_name)
{
    (void)role_name;
    (void)subject_name;
    kprintf("[RBAC] rbac_remove_binding: not yet implemented\n");
    return -ENOSYS;
}
