/*
 * secrets.c — Image pull secrets and secret management (C191)
 *
 * Implements:
 *   C191: Secret management — create, get, delete opaque, TLS, and
 *         Docker config secrets for image pull authentication
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

#define SECRETS_MAX             32
#define SECRET_NAME_MAX         64
#define NAMESPACE_LEN           64
#define SECRET_TYPE_LEN         32
#define SECRET_DATA_MAX         256

/* Secret types */
#define SECRET_TYPE_OPAQUE      "opaque"
#define SECRET_TYPE_TLS         "tls"
#define SECRET_TYPE_DOCKERCFG   "dockerconfig"

/* ── Secret descriptor ───────────────────────────────────────────────── */

struct orch_secret {
    char   in_use;
    char   name[SECRET_NAME_MAX];
    char   namespace[NAMESPACE_LEN];
    char   type[SECRET_TYPE_LEN];
    uint8_t data[SECRET_DATA_MAX];
    int    data_len;
    uint64_t created_at;
};

/* ── Global state ────────────────────────────────────────────────────── */

static struct orch_secret secret_table[SECRETS_MAX];
static int                secret_count;
static spinlock_t         secret_lock;
static int                secrets_initialised;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int secret_find_slot(void)
{
    for (int i = 0; i < SECRETS_MAX; i++) {
        if (!secret_table[i].in_use) return i;
    }
    return -1;
}

static struct orch_secret *secret_find(const char *name, const char *namespace)
{
    if (!name || !namespace) return NULL;
    for (int i = 0; i < SECRETS_MAX; i++) {
        if (secret_table[i].in_use &&
            strcmp(secret_table[i].name, name) == 0 &&
            strcmp(secret_table[i].namespace, namespace) == 0)
            return &secret_table[i];
    }
    return NULL;
}

/* ── C191: Initialise secret subsystem ───────────────────────────────── */

int orch_secrets_init(void)
{
    memset(secret_table, 0, sizeof(secret_table));
    secret_count = 0;
    secrets_initialised = 1;
    kprintf("[Secrets] Secret management initialised (max %d secrets)\n",
            SECRETS_MAX);
    return 0;
}

/* ── C191: Create a secret ───────────────────────────────────────────── */

int orch_secrets_create(const char *name, const char *namespace,
                   const char *type, const uint8_t *data, int data_len)
{
    if (!name || !namespace || !type || !data || !secrets_initialised)
        return -EINVAL;
    if (strlen(name) == 0 || strlen(name) >= SECRET_NAME_MAX)
        return -EINVAL;
    if (strlen(namespace) >= NAMESPACE_LEN)
        return -EINVAL;
    if (strlen(type) >= SECRET_TYPE_LEN)
        return -EINVAL;
    if (data_len <= 0 || data_len > SECRET_DATA_MAX)
        return -EINVAL;

    spinlock_acquire(&secret_lock);

    if (secret_find(name, namespace)) {
        spinlock_release(&secret_lock);
        return -EEXIST;
    }

    int slot = secret_find_slot();
    if (slot < 0) {
        spinlock_release(&secret_lock);
        return -ENOSPC;
    }

    struct orch_secret *s = &secret_table[slot];
    s->in_use = 1;
    strncpy(s->name, name, SECRET_NAME_MAX - 1);
    s->name[SECRET_NAME_MAX - 1] = '\0';
    strncpy(s->namespace, namespace, NAMESPACE_LEN - 1);
    s->namespace[NAMESPACE_LEN - 1] = '\0';
    strncpy(s->type, type, SECRET_TYPE_LEN - 1);
    s->type[SECRET_TYPE_LEN - 1] = '\0';
    memcpy(s->data, data, (size_t)data_len);
    s->data[data_len] = '\0'; /* ensure termination even for text data */
    s->data_len = data_len;
    s->created_at = timer_get_ticks();

    secret_count++;

    /* NOTE: we deliberately do NOT kprintf the secret data contents */
    kprintf("[Secrets] Created secret '%s' in namespace '%s' (type=%s, %d bytes)\n",
            s->name, s->namespace, s->type, s->data_len);

    spinlock_release(&secret_lock);
    return 0;
}

/* ── C191: Get secret data ───────────────────────────────────────────── */

int orch_secrets_get(const char *name, const char *namespace,
                uint8_t *out, int max_len)
{
    if (!name || !namespace || !out || !secrets_initialised)
        return -EINVAL;

    spinlock_acquire(&secret_lock);

    struct orch_secret *s = secret_find(name, namespace);
    if (!s) {
        spinlock_release(&secret_lock);
        return -ENOENT;
    }

    if (s->data_len > max_len) {
        spinlock_release(&secret_lock);
        return -ENOSPC;
    }

    memcpy(out, s->data, (size_t)s->data_len);
    int ret_len = s->data_len;

    spinlock_release(&secret_lock);
    return ret_len;
}

/* ── C191: Delete a secret ───────────────────────────────────────────── */

int orch_secrets_delete(const char *name, const char *namespace)
{
    if (!name || !namespace || !secrets_initialised)
        return -EINVAL;

    spinlock_acquire(&secret_lock);

    struct orch_secret *s = secret_find(name, namespace);
    if (!s) {
        spinlock_release(&secret_lock);
        return -ENOENT;
    }

    /* Zero out the secret data before freeing the slot */
    memset(s->data, 0, (size_t)s->data_len);
    s->in_use = 0;
    s->data_len = 0;
    secret_count--;

    kprintf("[Secrets] Deleted secret '%s' in namespace '%s'\n", name, namespace);
    spinlock_release(&secret_lock);
    return 0;
}

/* ── Utility: list secrets in a namespace ────────────────────────────── */

int orch_secrets_list(const char *namespace,
                 char names[][SECRET_NAME_MAX], int max_count)
{
    if (!namespace || !names || !secrets_initialised)
        return -EINVAL;

    spinlock_acquire(&secret_lock);
    int written = 0;
    for (int i = 0; i < SECRETS_MAX && written < max_count; i++) {
        if (secret_table[i].in_use &&
            strcmp(secret_table[i].namespace, namespace) == 0) {
            strncpy(names[written], secret_table[i].name, SECRET_NAME_MAX - 1);
            names[written][SECRET_NAME_MAX - 1] = '\0';
            written++;
        }
    }
    spinlock_release(&secret_lock);
    return written;
}

/* ── Stub: secret_create ─────────────────────────────── */
int secret_create(const char *name, const void *data, size_t len)
{
    (void)name;
    (void)data;
    (void)len;
    kprintf("[secrets] secret_create: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: secret_get ─────────────────────────────── */
int secret_get(const char *name, void *data, size_t *len)
{
    (void)name;
    (void)data;
    (void)len;
    kprintf("[secrets] secret_get: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: secret_delete ─────────────────────────────── */
int secret_delete(const char *name)
{
    (void)name;
    kprintf("[secrets] secret_delete: not yet implemented\n");
    return -ENOSYS;
}
