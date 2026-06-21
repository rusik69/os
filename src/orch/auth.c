/*
 * auth.c — API server authentication (C189)
 *
 * Implements:
 *   C189: Bearer token authentication for the orchestration API server
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

#define AUTH_TOKENS_MAX         32
#define AUTH_TOKEN_LEN          128
#define AUTH_USER_LEN           64
#define AUTH_KIND_LEN           16

/* ── Auth token entry ────────────────────────────────────────────────── */

struct auth_token {
    char   in_use;
    char   token[AUTH_TOKEN_LEN];
    char   user[AUTH_USER_LEN];
    char   kind[AUTH_KIND_LEN];          /* "user" or "serviceaccount" */
    uint64_t expiry;                     /* 0 = no expiry */
    uint64_t created_at;
};

/* ── Global state ────────────────────────────────────────────────────── */

static struct auth_token auth_table[AUTH_TOKENS_MAX];
static int               auth_count;
static spinlock_t        auth_lock;
static int               auth_initialised;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int auth_find_slot(void)
{
    for (int i = 0; i < AUTH_TOKENS_MAX; i++) {
        if (!auth_table[i].in_use) return i;
    }
    return -1;
}

static struct auth_token *auth_find_by_token(const char *token)
{
    if (!token) return NULL;
    for (int i = 0; i < AUTH_TOKENS_MAX; i++) {
        if (auth_table[i].in_use &&
            strcmp(auth_table[i].token, token) == 0)
            return &auth_table[i];
    }
    return NULL;
}

/* ── Initialise authentication subsystem ─────────────────────────────── */

int auth_init(void)
{
    memset(auth_table, 0, sizeof(auth_table));
    auth_count = 0;
    auth_initialised = 1;
    kprintf("[Auth] Authentication subsystem initialised (max %d tokens)\n",
            AUTH_TOKENS_MAX);
    return 0;
}

/* ── Register a new auth token ───────────────────────────────────────── */

int auth_register(const char *token, const char *user,
                  const char *kind, uint64_t expiry_ms)
{
    if (!token || !user || !kind || !auth_initialised)
        return -EINVAL;
    if (strlen(token) >= AUTH_TOKEN_LEN ||
        strlen(user) >= AUTH_USER_LEN ||
        strlen(kind) >= AUTH_KIND_LEN)
        return -EINVAL;

    spinlock_acquire(&auth_lock);

    if (auth_find_by_token(token)) {
        spinlock_release(&auth_lock);
        return -EEXIST;
    }

    int slot = auth_find_slot();
    if (slot < 0) {
        spinlock_release(&auth_lock);
        return -ENOSPC;
    }

    struct auth_token *at = &auth_table[slot];
    at->in_use = 1;
    strncpy(at->token, token, AUTH_TOKEN_LEN - 1);
    at->token[AUTH_TOKEN_LEN - 1] = '\0';
    strncpy(at->user, user, AUTH_USER_LEN - 1);
    at->user[AUTH_USER_LEN - 1] = '\0';
    strncpy(at->kind, kind, AUTH_KIND_LEN - 1);
    at->kind[AUTH_KIND_LEN - 1] = '\0';
    at->created_at = timer_get_ticks();

    if (expiry_ms > 0) {
        at->expiry = timer_get_ticks() + (expiry_ms / 10); /* timer ticks are ~10ms */
    } else {
        at->expiry = 0; /* no expiry */
    }

    auth_count++;
    kprintf("[Auth] Registered token for user '%s' (kind=%s)\n", user, kind);

    spinlock_release(&auth_lock);
    return 0;
}

/* ── Validate a token, return user info (C189) ───────────────────────── */

int auth_validate_token(const char *token, char *user_out, size_t user_max,
                        char *kind_out, size_t kind_max)
{
    if (!token || !auth_initialised)
        return -EINVAL;

    spinlock_acquire(&auth_lock);

    struct auth_token *at = auth_find_by_token(token);
    if (!at) {
        spinlock_release(&auth_lock);
        return -EINVAL;
    }

    /* Check expiry */
    if (at->expiry > 0 && timer_get_ticks() >= at->expiry) {
        /* Token expired — remove it */
        at->in_use = 0;
        auth_count--;
        spinlock_release(&auth_lock);
        return -EINVAL;
    }

    if (user_out && user_max > 0) {
        strncpy(user_out, at->user, user_max - 1);
        user_out[user_max - 1] = '\0';
    }
    if (kind_out && kind_max > 0) {
        strncpy(kind_out, at->kind, kind_max - 1);
        kind_out[kind_max - 1] = '\0';
    }

    spinlock_release(&auth_lock);
    return 0;
}

/* ── Authenticate a Bearer token string (C189) ───────────────────────── */

int auth_authenticate(const char *token_str, char *user_out, size_t user_max)
{
    if (!token_str || !user_out || !auth_initialised)
        return -EINVAL;

    /* Expect "Bearer <token>" format */
    const char *prefix = "Bearer ";
    size_t plen = strlen(prefix);

    if (strncmp(token_str, prefix, plen) != 0)
        return -EINVAL;

    const char *token = token_str + plen;
    if (strlen(token) == 0)
        return -EINVAL;

    char kind[AUTH_KIND_LEN];
    int ret = auth_validate_token(token, user_out, user_max, kind, sizeof(kind));
    if (ret < 0)
        return ret;

    kprintf("[Auth] Authenticated %s '%s' via Bearer token\n", kind, user_out);
    return 0;
}

/* ── Revoke a token ──────────────────────────────────────────────────── */

int auth_revoke(const char *token)
{
    if (!token || !auth_initialised)
        return -EINVAL;

    spinlock_acquire(&auth_lock);

    struct auth_token *at = auth_find_by_token(token);
    if (!at) {
        spinlock_release(&auth_lock);
        return -ENOENT;
    }

    memset(at, 0, sizeof(*at));
    auth_count--;

    kprintf("[Auth] Token revoked\n");
    spinlock_release(&auth_lock);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Stub: auth_authorize ──────────────────────────── */
int auth_authorize(const char *token, const char *resource, const char *verb)
{
    (void)token;
    (void)resource;
    (void)verb;
    kprintf("[Auth] auth_authorize: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: auth_login ──────────────────────────────── */
int auth_login(const char *username, const char *password, char *token_out, size_t token_max)
{
    (void)username;
    (void)password;
    (void)token_out;
    (void)token_max;
    kprintf("[Auth] auth_login: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: auth_logout ─────────────────────────────── */
int auth_logout(const char *token)
{
    (void)token;
    kprintf("[Auth] auth_logout: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: auth_token_validate ──────────────────────── */
int auth_token_validate(const char *token, char *user_out, size_t user_max)
{
    (void)token;
    (void)user_out;
    (void)user_max;
    kprintf("[Auth] auth_token_validate: not yet implemented\n");
    return -ENOSYS;
}
