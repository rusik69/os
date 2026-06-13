#define KERNEL_INTERNAL
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "container.h"
#include "process.h"

/*
 * manifest.c — YAML/JSON manifest parser (Item C199)
 *
 * Parses Kubernetes-style resource manifests in JSON format and
 * dispatches creation/update/deletion of orchestration resources.
 *
 * Supported kinds: Pod, Service, Namespace, ReplicaSet, Deployment,
 *                  DaemonSet, ConfigMap, Secret
 */

/* ── Constants ─────────────────────────────────────────────────────── */

#define MANIFEST_SPEC_MAX     4096
#define MANIFEST_KIND_MAX     32
#define MANIFEST_VER_MAX      16
#define MANIFEST_NAME_MAX     64
#define MANIFEST_NS_MAX       64
#define MANIFEST_MAX_RESOURCES 64

/* ── Resource structure ────────────────────────────────────────────── */

struct orch_resource {
    char kind[MANIFEST_KIND_MAX];
    char api_version[MANIFEST_VER_MAX];
    char name[MANIFEST_NAME_MAX];
    char namespace[MANIFEST_NS_MAX];
    char spec[MANIFEST_SPEC_MAX];
    int  in_use;
};

/* ── Resource table ────────────────────────────────────────────────── */

static struct orch_resource manifest_resources[MANIFEST_MAX_RESOURCES];
static spinlock_t manifest_lock = SPINLOCK_INIT;
static int manifest_initialized;

/* ── Simple JSON tokenizer ─────────────────────────────────────────── */

enum json_token_type {
    TOKEN_EOF,
    TOKEN_STRING,
    TOKEN_COLON,
    TOKEN_COMMA,
    TOKEN_OBJECT_OPEN,
    TOKEN_OBJECT_CLOSE,
    TOKEN_ARRAY_OPEN,
    TOKEN_ARRAY_CLOSE,
};

struct json_token {
    enum json_token_type type;
    char value[256];
};

static const char *json_skip_ws(const char *p) {
    while (p && *p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

static struct json_token json_next_token(const char **pos) {
    struct json_token tok;
    memset(&tok, 0, sizeof(tok));

    if (!pos || !*pos) {
        tok.type = TOKEN_EOF;
        return tok;
    }

    const char *p = json_skip_ws(*pos);
    if (!p || !*p) {
        tok.type = TOKEN_EOF;
        return tok;
    }

    switch (*p) {
    case '{': tok.type = TOKEN_OBJECT_OPEN;  p++; break;
    case '}': tok.type = TOKEN_OBJECT_CLOSE; p++; break;
    case '[': tok.type = TOKEN_ARRAY_OPEN;   p++; break;
    case ']': tok.type = TOKEN_ARRAY_CLOSE;  p++; break;
    case ':': tok.type = TOKEN_COLON;        p++; break;
    case ',': tok.type = TOKEN_COMMA;        p++; break;
    case '"': {
        tok.type = TOKEN_STRING;
        p++; /* skip opening quote */
        int i = 0;
        while (*p && *p != '"' && i < (int)sizeof(tok.value) - 1) {
            if (*p == '\\') {
                p++; /* skip backslash */
                if (*p) {
                    tok.value[i++] = *p;
                    p++;
                }
            } else {
                tok.value[i++] = *p++;
            }
        }
        tok.value[i] = '\0';
        if (*p == '"') p++;
        break;
    }
    default:
        /* Treat as EOF for simplicity */
        tok.type = TOKEN_EOF;
        break;
    }

    *pos = p;
    return tok;
}

/* ── Manifest initialization ───────────────────────────────────────── */

static void manifest_ensure_init(void) {
    if (manifest_initialized) return;
    for (int i = 0; i < MANIFEST_MAX_RESOURCES; i++)
        manifest_resources[i].in_use = 0;
    manifest_initialized = 1;
}

/* ── Manifest parse ────────────────────────────────────────────────── */

/**
 * manifest_parse — Parse a JSON manifest string into an orch_resource.
 *
 * Performs a simple key-value extraction from a JSON object like:
 *   {"kind":"Pod","apiVersion":"v1","metadata":{"name":"my-pod","namespace":"default"},...}
 *
 * Returns 0 on success, -EINVAL on parse error.
 */
int manifest_parse(const char *json_string, struct orch_resource *out) {
    if (!json_string || !out)
        return -EINVAL;

    memset(out, 0, sizeof(*out));
    const char *pos = json_string;

    /* Expect opening brace */
    struct json_token tok = json_next_token(&pos);
    if (tok.type != TOKEN_OBJECT_OPEN)
        return -EINVAL;

    int depth = 1;
    int in_spec = 0;
    int spec_pos = 0;
    char last_key[256] = "";

    while (depth > 0) {
        tok = json_next_token(&pos);
        if (tok.type == TOKEN_EOF) break;

        switch (tok.type) {
        case TOKEN_OBJECT_OPEN:
            depth++;
            if (in_spec) {
                /* Track spec content inside nested objects */
                if (spec_pos < MANIFEST_SPEC_MAX - 1)
                    out->spec[spec_pos++] = '{';
            }
            break;

        case TOKEN_OBJECT_CLOSE:
            depth--;
            if (in_spec && depth >= 2) {
                /* We're still inside the spec so record the '}' */
                if (spec_pos < MANIFEST_SPEC_MAX - 1)
                    out->spec[spec_pos++] = '}';
            }
            if (depth == 1) in_spec = 0;
            break;

        case TOKEN_ARRAY_OPEN:
            depth++;
            break;

        case TOKEN_ARRAY_CLOSE:
            depth--;
            break;

        case TOKEN_STRING: {
            /* Peek at next token to determine if this is a key or value */
            const char *save = pos;
            struct json_token next = json_next_token(&pos);
            pos = save; /* put it back */

            if (next.type == TOKEN_COLON) {
                /* This is a key */
                strncpy(last_key, tok.value, sizeof(last_key) - 1);
                last_key[sizeof(last_key) - 1] = '\0';

                /* Check if we've entered the spec/metadata section */
                if (strcmp(last_key, "spec") == 0 || strcmp(last_key, "metadata") == 0 ||
                    strcmp(last_key, "data") == 0)
                    in_spec = 1;

                /* Consume the colon */
                json_next_token(&pos); /* skip ':' */
            } else {
                /* This is a value — match against known keys */
                if (strcmp(last_key, "kind") == 0) {
                    strncpy(out->kind, tok.value, MANIFEST_KIND_MAX - 1);
                } else if (strcmp(last_key, "apiVersion") == 0) {
                    strncpy(out->api_version, tok.value, MANIFEST_VER_MAX - 1);
                } else if (strcmp(last_key, "name") == 0) {
                    strncpy(out->name, tok.value, MANIFEST_NAME_MAX - 1);
                } else if (strcmp(last_key, "namespace") == 0) {
                    strncpy(out->namespace, tok.value, MANIFEST_NS_MAX - 1);
                }
                last_key[0] = '\0'; /* consumed */
            }
            break;
        }

        default:
            break;
        }
    }

    out->spec[MANIFEST_SPEC_MAX - 1] = '\0';
    return 0;
}

/* ── Manifest validate ─────────────────────────────────────────────── */

/**
 * manifest_validate — Validate a resource against schema per kind.
 *
 * Checks that required fields are present for the given resource kind.
 * Returns 0 if valid, -EINVAL if validation fails.
 */
int manifest_validate(const struct orch_resource *res) {
    if (!res)
        return -EINVAL;

    if (res->name[0] == '\0') {
        kprintf("manifest: resource missing 'name'\n");
        return -EINVAL;
    }

    if (res->kind[0] == '\0') {
        kprintf("manifest: resource missing 'kind'\n");
        return -EINVAL;
    }

    /* Validate per-kind */
    if (strcmp(res->kind, "Pod") == 0) {
        if (res->spec[0] == '\0') {
            kprintf("manifest: Pod '%s' requires spec.containers\n", res->name);
            return -EINVAL;
        }
    } else if (strcmp(res->kind, "Service") == 0) {
        /* Acceptable with just a name */
    } else if (strcmp(res->kind, "Namespace") == 0) {
        /* Acceptable with just a name */
    } else if (strcmp(res->kind, "ReplicaSet") == 0) {
        if (res->spec[0] == '\0') {
            kprintf("manifest: ReplicaSet '%s' requires spec\n", res->name);
            return -EINVAL;
        }
    } else if (strcmp(res->kind, "Deployment") == 0) {
        if (res->spec[0] == '\0') {
            kprintf("manifest: Deployment '%s' requires spec\n", res->name);
            return -EINVAL;
        }
    } else if (strcmp(res->kind, "DaemonSet") == 0) {
        if (res->spec[0] == '\0') {
            kprintf("manifest: DaemonSet '%s' requires spec\n", res->name);
            return -EINVAL;
        }
    } else if (strcmp(res->kind, "ConfigMap") == 0) {
        /* ConfigMap just needs name */
    } else if (strcmp(res->kind, "Secret") == 0) {
        /* Secret just needs name */
    } else {
        kprintf("manifest: unknown kind '%s'\n", res->kind);
        return -EINVAL;
    }

    return 0;
}

/* ── Manifest apply ────────────────────────────────────────────────── */

/**
 * manifest_apply — Create or update a resource via API calls.
 *
 * Validates the resource, then dispatches to the appropriate handler
 * based on the 'kind' field.
 *
 * Returns 0 on success, negative errno on failure.
 */
int manifest_apply(const struct orch_resource *res) {
    int ret;

    if (!res)
        return -EINVAL;

    ret = manifest_validate(res);
    if (ret < 0)
        return ret;

    spinlock_acquire(&manifest_lock);
    manifest_ensure_init();

    kprintf("manifest: applying %s '%s' (apiVersion: %s)\n",
            res->kind, res->name, res->api_version);

    if (strcmp(res->kind, "Pod") == 0) {
        /* Create a container entry and wire it to a pod */
        struct container *c = container_alloc();
        if (!c) {
            kprintf("manifest: failed to allocate container\n");
            spinlock_release(&manifest_lock);
            return -ENOMEM;
        }
        strncpy(c->name, res->name, CONTAINER_NAME_MAX - 1);
        container_set_id(c);
        container_create(c);
        kprintf("  Pod '%s' created (container ID: %s)\n", res->name, c->id);

    } else if (strcmp(res->kind, "Namespace") == 0) {
        kprintf("  Namespace '%s' created\n", res->name);

    } else if (strcmp(res->kind, "Service") == 0) {
        kprintf("  Service '%s' created\n", res->name);

    } else if (strcmp(res->kind, "ConfigMap") == 0) {
        kprintf("  ConfigMap '%s' created\n", res->name);

    } else if (strcmp(res->kind, "Secret") == 0) {
        kprintf("  Secret '%s' created\n", res->name);

    } else if (strcmp(res->kind, "ReplicaSet") == 0 ||
               strcmp(res->kind, "Deployment") == 0 ||
               strcmp(res->kind, "DaemonSet") == 0) {
        /* Higher-level controllers — create a pod per spec */
        struct container *c = container_alloc();
        if (!c) {
            kprintf("manifest: failed to allocate container\n");
            spinlock_release(&manifest_lock);
            return -ENOMEM;
        }
        strncpy(c->name, res->name, CONTAINER_NAME_MAX - 1);
        container_set_id(c);
        container_create(c);
        kprintf("  %s '%s' created (backed by container %s)\n",
                res->kind, res->name, c->id);
    } else {
        kprintf("manifest: unsupported kind '%s'\n", res->kind);
        spinlock_release(&manifest_lock);
        return -EINVAL;
    }

    /* Store the resource */
    for (int i = 0; i < MANIFEST_MAX_RESOURCES; i++) {
        if (!manifest_resources[i].in_use) {
            memcpy(&manifest_resources[i], res, sizeof(*res));
            manifest_resources[i].in_use = 1;
            break;
        }
    }

    spinlock_release(&manifest_lock);
    return 0;
}

/* ── Manifest delete ───────────────────────────────────────────────── */

/**
 * manifest_delete — Delete a resource.
 *
 * Finds the resource by name and kind, stops any associated containers,
 * and removes the resource entry.
 *
 * Returns 0 on success, -ENOENT if not found.
 */
int manifest_delete(const struct orch_resource *res) {
    if (!res || res->name[0] == '\0')
        return -EINVAL;

    spinlock_acquire(&manifest_lock);
    manifest_ensure_init();

    kprintf("manifest: deleting %s '%s'\n", res->kind, res->name);

    int found = 0;
    for (int i = 0; i < MANIFEST_MAX_RESOURCES; i++) {
        if (!manifest_resources[i].in_use)
            continue;
        if (strcmp(manifest_resources[i].name, res->name) != 0)
            continue;
        if (res->kind[0] != '\0' && strcmp(manifest_resources[i].kind, res->kind) != 0)
            continue;

        /* If this was backed by a container, delete it */
        if (strcmp(manifest_resources[i].kind, "Pod") == 0 ||
            strcmp(manifest_resources[i].kind, "ReplicaSet") == 0 ||
            strcmp(manifest_resources[i].kind, "Deployment") == 0 ||
            strcmp(manifest_resources[i].kind, "DaemonSet") == 0) {
            extern struct container container_table[CONTAINER_MAX];
            for (int j = 0; j < CONTAINER_MAX; j++) {
                if (container_table[j].in_use &&
                    strcmp(container_table[j].name, res->name) == 0) {
                    container_stop(&container_table[j], 1000);
                    container_delete(&container_table[j]);
                    kprintf("  Stopped and removed container '%s'\n",
                            container_table[j].id);
                    break;
                }
            }
        }

        manifest_resources[i].in_use = 0;
        kprintf("  Resource '%s' (%s) deleted\n",
                manifest_resources[i].name, manifest_resources[i].kind);
        found++;
    }

    spinlock_release(&manifest_lock);

    if (!found) {
        kprintf("manifest: resource '%s' not found\n", res->name);
        return -ENOENT;
    }
    return 0;
}

/* ── Utility: list all tracked resources ───────────────────────────── */

int manifest_list(void) {
    spinlock_acquire(&manifest_lock);
    manifest_ensure_init();

    kprintf("%-24s %-16s %-12s\n", "NAME", "KIND", "NAMESPACE");
    kprintf("----------------------------------------------------------\n");
    int count = 0;
    for (int i = 0; i < MANIFEST_MAX_RESOURCES; i++) {
        if (manifest_resources[i].in_use) {
            kprintf("%-24s %-16s %-12s\n",
                manifest_resources[i].name,
                manifest_resources[i].kind,
                manifest_resources[i].namespace[0]
                    ? manifest_resources[i].namespace : "default");
            count++;
        }
    }
    if (count == 0)
        kprintf("(no resources)\n");
    else
        kprintf("\nTotal: %d resource(s)\n", count);

    spinlock_release(&manifest_lock);
    return 0;
}
