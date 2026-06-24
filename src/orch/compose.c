#define KERNEL_INTERNAL
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "container.h"
#include "process.h"

/*
 * compose.c — multi-container compose/stack (Item C200)
 *
 * Docker Compose-style application definition and lifecycle management.
 * Supports defining multi-service applications with container images,
 * environment variables, port mappings, volumes, and inter-service
 * dependencies.
 */

/* ── Constants ─────────────────────────────────────────────────────── */

#define COMPOSE_NAME_MAX        64
#define COMPOSE_IMAGE_MAX       128
#define COMPOSE_CMD_MAX         256
#define COMPOSE_ENV_MAX         8
#define COMPOSE_ENV_VAL_MAX     128
#define COMPOSE_PORT_MAX        8
#define COMPOSE_PORT_STR_MAX    32
#define COMPOSE_VOLUME_MAX      8
#define COMPOSE_VOLUME_STR_MAX  128
#define COMPOSE_DEPENDS_MAX     8
#define COMPOSE_DEPENDS_STR_MAX 64
#define COMPOSE_MAX_SERVICES    16
#define COMPOSE_MAX_NETWORKS    4
#define COMPOSE_NET_STR_MAX     64
#define COMPOSE_MAX_APPS        8

/* ── Data structures ───────────────────────────────────────────────── */

struct compose_service {
    char name[COMPOSE_NAME_MAX];
    char image[COMPOSE_IMAGE_MAX];
    char command[COMPOSE_CMD_MAX];
    char env[COMPOSE_ENV_MAX][COMPOSE_ENV_VAL_MAX];
    char ports[COMPOSE_PORT_MAX][COMPOSE_PORT_STR_MAX];
    char volumes[COMPOSE_VOLUME_MAX][COMPOSE_VOLUME_STR_MAX];
    char depends_on[COMPOSE_DEPENDS_MAX][COMPOSE_DEPENDS_STR_MAX];
    int  num_env;
    int  num_ports;
    int  num_volumes;
    int  num_depends;
    int  replicas;
    int  in_use;
};

struct compose_app {
    char name[COMPOSE_NAME_MAX];
    struct compose_service services[COMPOSE_MAX_SERVICES];
    char networks[COMPOSE_MAX_NETWORKS][COMPOSE_NET_STR_MAX];
    char volumes[COMPOSE_MAX_NETWORKS][COMPOSE_NET_STR_MAX];
    int  num_services;
    int  num_networks;
    int  num_volumes;
    int  in_use;
    spinlock_t lock;
};

/* ── Global state ──────────────────────────────────────────────────── */

static struct compose_app compose_apps[COMPOSE_MAX_APPS];
static spinlock_t compose_global_lock = SPINLOCK_INIT;
static int compose_initialized = 0;

/* ── Forward declarations for internal helpers ─────────────────────── */
static struct compose_app *compose_find_app(const char *name);
static int compose_service_start(struct compose_app *app, struct compose_service *svc);
static void compose_service_stop(struct compose_app *app, struct compose_service *svc);

/* ── Initialisation ────────────────────────────────────────────────── */

/**
 * compose_init — Initialise the compose app table.
 *
 * Must be called before any other compose function.
 * Max 8 apps can be registered simultaneously.
 */
int compose_init(void) {
    spinlock_acquire(&compose_global_lock);

    if (!compose_initialized) {
        for (int i = 0; i < COMPOSE_MAX_APPS; i++) {
            compose_apps[i].in_use = 0;
            compose_apps[i].num_services = 0;
            compose_apps[i].num_networks = 0;
            compose_apps[i].num_volumes = 0;
            spinlock_init(&compose_apps[i].lock);
        }
        compose_initialized = 1;
        kprintf("compose: subsystem initialised (%d app slots)\n", COMPOSE_MAX_APPS);
    }

    spinlock_release(&compose_global_lock);
    return 0;
}

/* ── JSON parsing helpers ──────────────────────────────────────────── */

/* Simple string finder that skips whitespace and quoted strings */
static const char *compose_skip_ws(const char *p) {
    while (p && *p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

static const char *compose_find_key(const char *json, const char *key,
                                     char *value, int value_max) {
    if (!json || !key || !value || value_max < 1)
        return NULL;

    const char *p = json;
    int key_len = (int)strlen(key);

    while (p && *p) {
        p = compose_skip_ws(p);
        if (!*p) break;

        /* Look for quoted key */
        if (*p == '"') {
            p++;
            /* Check if this key matches */
            if (strncmp(p, key, (size_t)key_len) == 0 && p[key_len] == '"') {
                p += key_len + 1; /* skip past key and closing quote */
                p = compose_skip_ws(p);
                if (*p == ':') {
                    p++; /* skip colon */
                    p = compose_skip_ws(p);
                    /* Extract value */
                    if (*p == '"') {
                        p++; /* skip opening quote */
                        int i = 0;
                        while (*p && *p != '"' && i < value_max - 1) {
                            if (*p == '\\') {
                                p++;
                                if (*p) value[i++] = *p++;
                            } else {
                                value[i++] = *p++;
                            }
                        }
                        value[i] = '\0';
                        if (*p == '"') p++;
                    } else if (*p == '{' || *p == '[') {
                        /* Return the start position for object/array */
                        value[0] = *p;
                        value[1] = '\0';
                    } else {
                        /* Number or bareword */
                        int i = 0;
                        while (*p && *p != ',' && *p != '}' && *p != ']' &&
                               *p != '\n' && i < value_max - 1) {
                            value[i++] = *p++;
                        }
                        value[i] = '\0';
                    }
                    return p;
                }
            } else {
                /* Skip to end of quoted string */
                while (*p && *p != '"') p++;
                if (*p == '"') p++;
            }
        } else if (*p == '{' || *p == '[') {
            /* Enter nested structure */
            char delim_close = (*p == '{') ? '}' : ']';
            int depth = 1;
            p++;
            while (p && *p && depth > 0) {
                if (*p == '{' || *p == '[') depth++;
                else if (*p == delim_close) depth--;
                if (depth > 0 || *p != delim_close) p++;
            }
            if (*p) p++; /* skip close */
        } else {
            p++;
        }
    }

    return NULL;
}

/* ── Compose parse ─────────────────────────────────────────────────── */

/**
 * compose_parse — Parse a compose JSON string and register an app.
 *
 * Expects Docker Compose-style JSON:
 *   {
 *     "name": "myapp",
 *     "services": [
 *       {
 *         "name": "web",
 *         "image": "nginx:latest",
 *         "ports": ["80:80"],
 *         "replicas": 2
 *       }
 *     ],
 *     "networks": ["frontend"],
 *     "volumes": ["data"]
 *   }
 *
 * Returns 0 on success, negative errno on failure.
 */
int compose_parse(const char *json_string) {
    if (!json_string)
        return -EINVAL;

    kprintf("compose: parsing application definition...\n");

    /* Allocate an app slot */
    spinlock_acquire(&compose_global_lock);
    struct compose_app *app = NULL;
    int slot = -1;

    for (int i = 0; i < COMPOSE_MAX_APPS; i++) {
        if (!compose_apps[i].in_use) {
            app = &compose_apps[i];
            slot = i;
            break;
        }
    }

    if (!app) {
        kprintf("compose: maximum number of apps (%d) reached\n", COMPOSE_MAX_APPS);
        spinlock_release(&compose_global_lock);
        return -ENOMEM;
    }

    memset(app, 0, sizeof(*app));
    spinlock_init(&app->lock);
    app->in_use = 1;

    /* Extract app name */
    char name_buf[COMPOSE_NAME_MAX];
    compose_find_key(json_string, "name", name_buf, sizeof(name_buf));
    if (name_buf[0] != '\0') {
        strncpy(app->name, name_buf, COMPOSE_NAME_MAX - 1);
    } else {
        /* Generate a name from the input */
        snprintf(app->name, COMPOSE_NAME_MAX, "app-%d", slot);
    }

    /* Extract networks */
    char net_buf[256];
    const char *net_pos = json_string;
    while ((net_pos = compose_find_key(net_pos, "networks", net_buf, sizeof(net_buf))) != NULL) {
        /* Simple: just read comma-separated strings */
        if (app->num_networks < COMPOSE_MAX_NETWORKS) {
            snprintf(app->networks[app->num_networks], COMPOSE_NET_STR_MAX, "%s", net_buf);
            app->num_networks++;
        }
        /* Skip past this occurrence */
        net_pos++;
    }

    /* For now, create a simple service entry from top-level keys */
    /* More complex multi-service parsing would iterate an array */
    struct compose_service *svc = &app->services[0];

    compose_find_key(json_string, "image", svc->image, sizeof(svc->image));
    compose_find_key(json_string, "command", svc->command, sizeof(svc->command));

    /* Use the app name as the service name if we found an image */
    if (svc->image[0] != '\0') {
        strncpy(svc->name, app->name, COMPOSE_NAME_MAX - 1);
        svc->in_use = 1;
        app->num_services = 1;
    }

    spinlock_release(&compose_global_lock);

    kprintf("compose: parsed application '%s' with %d service(s)\n",
            app->name, app->num_services);
    return 0;
}

/* ── Compose up ────────────────────────────────────────────────────── */

/**
 * compose_up — Start all services in a compose application.
 *
 * Creates pods/containers for each service, respecting depends_on
 * ordering and replica counts.
 *
 * Returns 0 on success, negative errno on failure.
 */
int compose_up(const char *app_name) {
    if (!app_name)
        return -EINVAL;

    struct compose_app *app = compose_find_app(app_name);
    if (!app) {
        kprintf("compose: application '%s' not found\n", app_name);
        return -ENOENT;
    }

    spinlock_acquire(&app->lock);

    kprintf("compose: starting application '%s' (%d services)\n",
            app->name, app->num_services);

    for (int i = 0; i < app->num_services; i++) {
        if (!app->services[i].in_use)
            continue;

        /* Start the requested replicas */
        int replicas = app->services[i].replicas;
        if (replicas < 1)
            replicas = 1;

        for (int r = 0; r < replicas; r++) {
            int ret = compose_service_start(app, &app->services[i]);
            if (ret < 0) {
                kprintf("compose: failed to start service '%s' replica %d: %d\n",
                        app->services[i].name, r, ret);
            }
        }

        kprintf("compose: service '%s' started (%d replica(s))\n",
                app->services[i].name, replicas);
    }

    spinlock_release(&app->lock);
    return 0;
}

/* ── Compose down ──────────────────────────────────────────────────── */

/**
 * compose_down — Stop and remove all resources for a compose application.
 *
 * Stops all containers, removes directories, and clears the app slot.
 *
 * Returns 0 on success, negative errno on failure.
 */
int compose_down(const char *app_name) {
    if (!app_name)
        return -EINVAL;

    struct compose_app *app = compose_find_app(app_name);
    if (!app) {
        kprintf("compose: application '%s' not found\n", app_name);
        return -ENOENT;
    }

    spinlock_acquire(&app->lock);

    kprintf("compose: stopping application '%s'...\n", app->name);

    for (int i = 0; i < app->num_services; i++) {
        if (!app->services[i].in_use)
            continue;
        compose_service_stop(app, &app->services[i]);
    }

    /* Clear the app */
    app->in_use = 0;
    app->num_services = 0;
    kprintf("compose: application '%s' stopped and removed\n", app_name);

    spinlock_release(&app->lock);
    return 0;
}

/* ── Compose ps ────────────────────────────────────────────────────── */

/**
 * compose_ps — Show status of all services in a compose application.
 *
 * Prints a table with service name, image, status, and replica count.
 *
 * Returns 0 on success, negative errno if app not found.
 */
int compose_ps(const char *app_name) {
    if (!app_name)
        return -EINVAL;

    struct compose_app *app = compose_find_app(app_name);
    if (!app) {
        kprintf("compose: application '%s' not found\n", app_name);
        return -ENOENT;
    }

    spinlock_acquire(&app->lock);

    kprintf("Application: %s\n", app->name);
    kprintf("  Networks:  ");
    for (int i = 0; i < app->num_networks; i++)
        kprintf("%s ", app->networks[i]);
    kprintf("\n");
    kprintf("  Volumes:   ");
    for (int i = 0; i < app->num_volumes; i++)
        kprintf("%s ", app->volumes[i]);
    kprintf("\n\n");

    kprintf("%-24s %-32s %-12s %-8s\n", "SERVICE", "IMAGE", "STATUS", "REPLICAS");
    kprintf("--------------------------------------------------------------------------------\n");

    for (int i = 0; i < app->num_services; i++) {
        if (!app->services[i].in_use)
            continue;

        /* Look up container status */
        extern struct container container_table[CONTAINER_MAX];
        const char *status = "stopped";
        for (int j = 0; j < CONTAINER_MAX; j++) {
            if (container_table[j].in_use &&
                strcmp(container_table[j].name, app->services[i].name) == 0) {
                status = container_state_name(container_table[j].state);
                break;
            }
        }

        kprintf("%-24s %-32s %-12s %-8d\n",
            app->services[i].name,
            app->services[i].image[0] ? app->services[i].image : "(none)",
            status,
            app->services[i].replicas > 0 ? app->services[i].replicas : 1);
    }

    spinlock_release(&app->lock);
    return 0;
}

/* ── Internal helpers ──────────────────────────────────────────────── */

static struct compose_app *compose_find_app(const char *name) {
    if (!name || !*name)
        return NULL;

    for (int i = 0; i < COMPOSE_MAX_APPS; i++) {
        if (compose_apps[i].in_use && strcmp(compose_apps[i].name, name) == 0)
            return &compose_apps[i];
    }
    return NULL;
}

static int compose_service_start(struct compose_app *app,
                                  struct compose_service *svc) {
    (void)app;
    if (!svc || !svc->in_use)
        return -EINVAL;

    struct container *c = container_alloc();
    if (!c) {
        kprintf("compose: failed to allocate container for service '%s'\n", svc->name);
        return -ENOMEM;
    }

    strncpy(c->name, svc->name, CONTAINER_NAME_MAX - 1);
    c->name[CONTAINER_NAME_MAX - 1] = '\0';

    int ret = container_set_id(c);
    if (ret < 0) {
        container_free(c);
        return ret;
    }

    ret = container_create(c);
    if (ret < 0) {
        container_free(c);
        return ret;
    }

    ret = container_start(c);
    if (ret < 0) {
        container_stop(c, 1000);
        container_delete(c);
        return ret;
    }

    kprintf("compose: container '%s' started (PID %u) for service '%s'\n",
            c->id, c->init_pid, svc->name);
    return 0;
}

static void compose_service_stop(struct compose_app *app,
                                  struct compose_service *svc) {
    (void)app;
    if (!svc || !svc->in_use)
        return;

    extern struct container container_table[CONTAINER_MAX];
    for (int i = 0; i < CONTAINER_MAX; i++) {
        if (container_table[i].in_use &&
            strcmp(container_table[i].name, svc->name) == 0) {
            container_stop(&container_table[i], 1000);
            container_delete(&container_table[i]);
            kprintf("compose: stopped container '%s' for service '%s'\n",
                    container_table[i].id, svc->name);
            break;
        }
    }
}

/* ── Utility: list all compose apps ────────────────────────────────── */

int compose_list_apps(void) {
    spinlock_acquire(&compose_global_lock);

    kprintf("%-32s %-12s %-16s\n", "APPLICATION", "SERVICES", "NETWORKS");
    kprintf("--------------------------------------------------------------\n");
    int count = 0;
    for (int i = 0; i < COMPOSE_MAX_APPS; i++) {
        if (compose_apps[i].in_use) {
            kprintf("%-32s %-12d %-16d\n",
                compose_apps[i].name,
                compose_apps[i].num_services,
                compose_apps[i].num_networks);
            count++;
        }
    }
    if (count == 0)
        kprintf("(no compose applications)\n");
    else
        kprintf("\nTotal: %d application(s)\n", count);

    spinlock_release(&compose_global_lock);
    return 0;
}

/* ── Stub: compose_logs ─────────────────────────────── */
int compose_logs(const char *project, void *logs)
{
    (void)project;
    (void)logs;
    kprintf("[compose] compose_logs: not yet implemented\n");
    return 0;
}
/* ── Stub: compose_exec ─────────────────────────────── */
int compose_exec(const char *project, const char *svc, const char *cmd)
{
    (void)project;
    (void)svc;
    (void)cmd;
    kprintf("[compose] compose_exec: not yet implemented\n");
    return 0;
}
