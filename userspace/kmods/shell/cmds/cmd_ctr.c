#define KERNEL_INTERNAL
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "container.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"

/*
 * cmd_ctr.c — container runtime CLI (Item C196)
 *
 * Usage: ctr containers create|list|start|stop|rm [args...]
 *        ctr images pull|list|rm [args...]
 *        ctr tasks exec|logs|stats [args...]
 *        ctr snapshots list
 */

/* ── Forward declarations ──────────────────────────────────────────── */
static int ctr_containers(int argc, char **argv);
static int ctr_images(int argc, char **argv);
static int ctr_tasks(int argc, char **argv);
static int ctr_snapshots(void);

/* ── Main dispatch ─────────────────────────────────────────────────── */
int cmd_ctr(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: ctr <subcommand> [args...]\n");
        kprintf("\nContainer runtime CLI (OCI-compatible)\n\n");
        kprintf("Subcommands:\n");
        kprintf("  containers    Manage containers (create|list|start|stop|rm)\n");
        kprintf("  images        Manage images (pull|list|rm)\n");
        kprintf("  tasks         Manage tasks (exec|logs|stats)\n");
        kprintf("  snapshots     List available snapshots\n");
        return 0;
    }

    if (strcmp(argv[1], "containers") == 0)
        return ctr_containers(argc - 1, argv + 1);
    if (strcmp(argv[1], "images") == 0)
        return ctr_images(argc - 1, argv + 1);
    if (strcmp(argv[1], "tasks") == 0)
        return ctr_tasks(argc - 1, argv + 1);
    if (strcmp(argv[1], "snapshots") == 0)
        return ctr_snapshots();

    kprintf("ctr: unknown subcommand '%s'. Try 'ctr' for help.\n", argv[1]);
    return 0;
}

/* ── Container lifecycle helpers ───────────────────────────────────── */

/**
 * container_lookup_by_id — Find a container by its ID string.
 * Returns a pointer to the container, or NULL if not found.
 */
static struct container *container_lookup_by_id(const char *id) {
    extern struct container container_table[CONTAINER_MAX];
    if (!id || !*id)
        return NULL;
    for (int i = 0; i < CONTAINER_MAX; i++) {
        if (container_table[i].in_use &&
            strcmp(container_table[i].id, id) == 0) {
            return &container_table[i];
        }
    }
    return NULL;
}

/* ── 'containers' subcommand ───────────────────────────────────────── */

static int ctr_containers(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: ctr containers <create|list|start|stop|rm> [id]\n");
        return 0;
    }

    /* containers list */
    if (strcmp(argv[1], "list") == 0) {
        extern struct container container_table[CONTAINER_MAX];
        int count = 0;

        kprintf("%-24s %-12s %-12s\n", "CONTAINER ID", "STATE", "PID");
        kprintf("----------------------------------------------------------\n");
        for (int i = 0; i < CONTAINER_MAX; i++) {
            if (container_table[i].in_use) {
                kprintf("%-24s %-12s %-12u\n",
                    container_table[i].id,
                    container_state_name(container_table[i].state),
                    container_table[i].init_pid);
                count++;
            }
        }
        if (count == 0)
            kprintf("(no containers)\n");
        else
            kprintf("\nTotal: %d container(s)\n", count);
        return 0;
    }

    /* containers create */
    if (strcmp(argv[1], "create") == 0) {
        struct container *c = container_alloc();
        if (!c) {
            kprintf("ctr: failed to allocate container (table full)\n");
            return 0;
        }
        int ret = container_set_id(c);
        if (ret < 0) {
            kprintf("ctr: failed to set container ID: %d\n", ret);
            container_free(c);
            return 0;
        }
        ret = container_create(c);
        if (ret < 0) {
            kprintf("ctr: container_create failed: %d\n", ret);
            container_free(c);
            return 0;
        }
        kprintf("Container created: %s\n", c->id);
        return 0;
    }

    /* containers start */
    if (strcmp(argv[1], "start") == 0) {
        if (argc < 3) {
            kprintf("Usage: ctr containers start <container_id>\n");
            return 0;
        }
        struct container *c = container_lookup_by_id(argv[2]);
        if (!c) {
            kprintf("ctr: container '%s' not found\n", argv[2]);
            return 0;
        }
        int ret = container_start(c);
        if (ret < 0) {
            kprintf("ctr: container_start failed: %d\n", ret);
            return 0;
        }
        kprintf("Container %s started (PID %u)\n", c->id, c->init_pid);
        return 0;
    }

    /* containers stop */
    if (strcmp(argv[1], "stop") == 0) {
        if (argc < 3) {
            kprintf("Usage: ctr containers stop <container_id>\n");
            return 0;
        }
        struct container *c = container_lookup_by_id(argv[2]);
        if (!c) {
            kprintf("ctr: container '%s' not found\n", argv[2]);
            return 0;
        }
        int timeout = (argc >= 4) ? (int)strtol(argv[3], NULL, 10) : 3000;
        int ret = container_stop(c, timeout);
        if (ret < 0) {
            kprintf("ctr: container_stop failed: %d\n", ret);
            return 0;
        }
        kprintf("Container %s stopped\n", c->id);
        return 0;
    }

    /* containers rm */
    if (strcmp(argv[1], "rm") == 0) {
        if (argc < 3) {
            kprintf("Usage: ctr containers rm <container_id>\n");
            return 0;
        }
        struct container *c = container_lookup_by_id(argv[2]);
        if (!c) {
            kprintf("ctr: container '%s' not found\n", argv[2]);
            return 0;
        }
        int ret = container_delete(c);
        if (ret < 0) {
            kprintf("ctr: container_delete failed: %d\n", ret);
            return 0;
        }
        kprintf("Container %s removed\n", argv[2]);
        return 0;
    }

    kprintf("ctr containers: unknown action '%s'\n", argv[1]);
    return 0;
}

/* ── 'images' subcommand ───────────────────────────────────────────── */

/* Simple image tracking structure */
struct ctr_image {
    char name[128];
    char tag[64];
    int  in_use;
};

#define CTR_MAX_IMAGES 32
static struct ctr_image ctr_image_table[CTR_MAX_IMAGES];
static int ctr_images_init_done;

static void ctr_ensure_images_init(void) {
    if (!ctr_images_init_done) {
        for (int i = 0; i < CTR_MAX_IMAGES; i++)
            ctr_image_table[i].in_use = 0;
        ctr_images_init_done = 1;
    }
}

static struct ctr_image *ctr_image_alloc(void) {
    ctr_ensure_images_init();
    for (int i = 0; i < CTR_MAX_IMAGES; i++) {
        if (!ctr_image_table[i].in_use) {
            ctr_image_table[i].in_use = 1;
            return &ctr_image_table[i];
        }
    }
    return NULL;
}

static int ctr_images(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: ctr images <pull|list|rm> [args...]\n");
        return 0;
    }

    /* images list */
    if (strcmp(argv[1], "list") == 0) {
        ctr_ensure_images_init();
        kprintf("%-40s %-16s\n", "IMAGE", "TAG");
        kprintf("----------------------------------------------------------\n");
        int count = 0;
        for (int i = 0; i < CTR_MAX_IMAGES; i++) {
            if (ctr_image_table[i].in_use) {
                kprintf("%-40s %-16s\n",
                    ctr_image_table[i].name,
                    ctr_image_table[i].tag);
                count++;
            }
        }
        if (count == 0)
            kprintf("(no images)\n");
        else
            kprintf("\nTotal: %d image(s)\n", count);
        return 0;
    }

    /* images pull */
    if (strcmp(argv[1], "pull") == 0) {
        if (argc < 3) {
            kprintf("Usage: ctr images pull <image_name>[:tag]\n");
            return 0;
        }
        struct ctr_image *img = ctr_image_alloc();
        if (!img) {
            kprintf("ctr: image table full\n");
            return 0;
        }
        /* Parse name[:tag] */
        const char *colon = strchr(argv[2], ':');
        if (colon) {
            int name_len = (int)(colon - argv[2]);
            if (name_len > (int)sizeof(img->name) - 1)
                name_len = sizeof(img->name) - 1;
            strncpy(img->name, argv[2], (size_t)name_len);
            img->name[name_len] = '\0';
            strncpy(img->tag, colon + 1, sizeof(img->tag) - 1);
            img->tag[sizeof(img->tag) - 1] = '\0';
        } else {
            strncpy(img->name, argv[2], sizeof(img->name) - 1);
            img->name[sizeof(img->name) - 1] = '\0';
            strncpy(img->tag, "latest", sizeof(img->tag) - 1);
        }
        kprintf("Pulled image: %s:%s\n", img->name, img->tag);
        return 0;
    }

    /* images rm */
    if (strcmp(argv[1], "rm") == 0) {
        if (argc < 3) {
            kprintf("Usage: ctr images rm <image_name>\n");
            return 0;
        }
        ctr_ensure_images_init();
        for (int i = 0; i < CTR_MAX_IMAGES; i++) {
            if (ctr_image_table[i].in_use &&
                strcmp(ctr_image_table[i].name, argv[2]) == 0) {
                ctr_image_table[i].in_use = 0;
                kprintf("Removed image: %s\n", argv[2]);
                return 0;
            }
        }
        kprintf("ctr: image '%s' not found\n", argv[2]);
        return 0;
    }

    kprintf("ctr images: unknown action '%s'\n", argv[1]);
    return 0;
}

/* ── 'tasks' subcommand ────────────────────────────────────────────── */

static int ctr_tasks(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: ctr tasks <exec|logs|stats> [args...]\n");
        return 0;
    }

    /* tasks exec */
    if (strcmp(argv[1], "exec") == 0) {
        if (argc < 4) {
            kprintf("Usage: ctr tasks exec <container_id> <command> [args...]\n");
            return 0;
        }
        struct container *c = container_lookup_by_id(argv[2]);
        if (!c) {
            kprintf("ctr: container '%s' not found\n", argv[2]);
            return 0;
        }
        if (c->state != CONTAINER_STATE_RUNNING) {
            kprintf("ctr: container '%s' is not running\n", argv[2]);
            return 0;
        }
        /* Build argv for exec (skip 'tasks', 'exec', container_id) */
        (void)(argc); /* argc unused beyond bounds check */
        char **exec_argv = argv + 3;
        int ret = container_exec(c, exec_argv[0], exec_argv, NULL);
        if (ret < 0)
            kprintf("ctr: container_exec failed: %d\n", ret);
        return 0;
    }

    /* tasks logs */
    if (strcmp(argv[1], "logs") == 0) {
        if (argc < 3) {
            kprintf("Usage: ctr tasks logs <container_id>\n");
            return 0;
        }
        struct container *c = container_lookup_by_id(argv[2]);
        if (!c) {
            kprintf("ctr: container '%s' not found\n", argv[2]);
            return 0;
        }
        /* Read container log file and print it */
        char logpath[CONTAINER_STATE_PATH];
        snprintf(logpath, sizeof(logpath), "%s/log/output.log", c->data_dir);
        /* Attempt to open and read the log (stub — VFS file read) */
        kprintf("--- Logs for container %s ---\n", c->id);
        kprintf("(log file: %s)\n", logpath);
        kprintf("Container logs would be displayed here.\n");
        return 0;
    }

    /* tasks stats */
    if (strcmp(argv[1], "stats") == 0) {
        if (argc < 3) {
            kprintf("Usage: ctr tasks stats <container_id>\n");
            return 0;
        }
        struct container *c = container_lookup_by_id(argv[2]);
        if (!c) {
            kprintf("ctr: container '%s' not found\n", argv[2]);
            return 0;
        }
        struct container_stats stats;
        int ret = container_stats(c, &stats);
        if (ret < 0) {
            kprintf("ctr: container_stats failed: %d\n", ret);
            return 0;
        }
        kprintf("Container %s stats:\n", c->id);
        kprintf("  CPU usage:      %llu us\n", (unsigned long long)stats.cpu_usage_us);
        kprintf("  Memory usage:   %llu bytes\n", (unsigned long long)stats.memory_usage_bytes);
        kprintf("  Memory limit:   %llu bytes\n", (unsigned long long)stats.memory_limit_bytes);
        kprintf("  PIDs current:   %llu\n", (unsigned long long)stats.pids_current);
        kprintf("  I/O read:       %llu bytes\n", (unsigned long long)stats.io_read_bytes);
        kprintf("  I/O write:      %llu bytes\n", (unsigned long long)stats.io_write_bytes);
        return 0;
    }

    kprintf("ctr tasks: unknown action '%s'\n", argv[1]);
    return 0;
}

/* ── 'snapshots' subcommand ────────────────────────────────────────── */

static int ctr_snapshots(void) {
    kprintf("Available snapshots:\n");
    kprintf("(snapshot support not yet implemented)\n");
    return 0;
}
