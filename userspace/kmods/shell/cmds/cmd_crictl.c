#define KERNEL_INTERNAL
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "container.h"
#include "heap.h"
#include "errno.h"

/*
 * cmd_crictl.c — CRI-compatible debug CLI (Item C197)
 *
 * Provides a Kubernetes Container Runtime Interface (CRI) style debug CLI
 * for interacting with containers, pods, and images.
 *
 * Usage: crictl pods [namespace]
 *        crictl ps [-a]
 *        crictl inspect <id>
 *        crictl exec <id> <command> [args...]
 *        crictl logs <id>
 *        crictl stats [id]
 *        crictl image list|pull <name>
 */

/* ── Forward declarations ──────────────────────────────────────────── */
static int crictl_pods(int argc, char **argv);
static int crictl_ps(int argc, char **argv);
static int crictl_inspect(int argc, char **argv);
static int crictl_exec(int argc, char **argv);
static int crictl_logs(int argc, char **argv);
static int crictl_stats(int argc, char **argv);
static int crictl_image(int argc, char **argv);

/* ── Main dispatch ─────────────────────────────────────────────────── */
int cmd_crictl(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: crictl <subcommand> [args...]\n\n");
        kprintf("CRI-compatible container debug CLI\n\n");
        kprintf("Subcommands:\n");
        kprintf("  pods          List pods (optionally by namespace)\n");
        kprintf("  ps            List running containers\n");
        kprintf("  inspect       Inspect container or pod details\n");
        kprintf("  exec          Execute a command in a container\n");
        kprintf("  logs          Fetch container logs\n");
        kprintf("  stats         Show container resource statistics\n");
        kprintf("  image         Manage images (pull|list|rm)\n");
        return 0;
    }

    if (strcmp(argv[1], "pods") == 0)
        return crictl_pods(argc - 1, argv + 1);
    if (strcmp(argv[1], "ps") == 0)
        return crictl_ps(argc - 1, argv + 1);
    if (strcmp(argv[1], "inspect") == 0)
        return crictl_inspect(argc - 1, argv + 1);
    if (strcmp(argv[1], "exec") == 0)
        return crictl_exec(argc - 1, argv + 1);
    if (strcmp(argv[1], "logs") == 0)
        return crictl_logs(argc - 1, argv + 1);
    if (strcmp(argv[1], "stats") == 0)
        return crictl_stats(argc - 1, argv + 1);
    if (strcmp(argv[1], "image") == 0)
        return crictl_image(argc - 1, argv + 1);

    kprintf("crictl: unknown subcommand '%s'. Try 'crictl' for help.\n", argv[1]);
    return 0;
}

/* ── Pod helpers ───────────────────────────────────────────────────── */

/* Simple pod tracking structure */
#define CRICTL_MAX_PODS 16

struct crictl_pod {
    char    id[64];
    char    name[128];
    char    namespace[64];
    char    image[128];
    uint32_t container_pid;
    int     state;         /* CONTAINER_STATE_* */
    int     in_use;
};

static struct crictl_pod crictl_pods_table[CRICTL_MAX_PODS];
static int crictl_pods_init;

static void crictl_ensure_pods_init(void) {
    if (!crictl_pods_init) {
        for (int i = 0; i < CRICTL_MAX_PODS; i++)
            crictl_pods_table[i].in_use = 0;
        crictl_pods_init = 1;
    }
}

/* ── 'pods' subcommand ─────────────────────────────────────────────── */

static int crictl_pods(int argc, char **argv) {
    (void)argc;
    (void)argv;
    crictl_ensure_pods_init();
    kprintf("%-24s %-40s %-16s %-12s\n", "POD ID", "NAME", "NAMESPACE", "STATE");
    kprintf("--------------------------------------------------------------------------------\n");
    int count = 0;
    for (int i = 0; i < CRICTL_MAX_PODS; i++) {
        if (crictl_pods_table[i].in_use) {
            kprintf("%-24s %-40s %-16s %-12s\n",
                crictl_pods_table[i].id,
                crictl_pods_table[i].name,
                crictl_pods_table[i].namespace,
                container_state_name(crictl_pods_table[i].state));
            count++;
        }
    }
    if (count == 0)
        kprintf("(no pods)\n");
    else
        kprintf("\nTotal: %d pod(s)\n", count);
    return 0;
}

/* ── Container lookup ──────────────────────────────────────────────── */

static struct container *crictl_lookup(const char *id) {
    extern struct container container_table[CONTAINER_MAX];
    if (!id || !*id)
        return NULL;
    for (int i = 0; i < CONTAINER_MAX; i++) {
        if (container_table[i].in_use &&
            strcmp(container_table[i].id, id) == 0)
            return &container_table[i];
    }
    return NULL;
}

/* ── 'ps' subcommand ───────────────────────────────────────────────── */

static int crictl_ps(int argc, char **argv) {
    int show_all = 0;
    if (argc >= 2 && strcmp(argv[1], "-a") == 0)
        show_all = 1;

    extern struct container container_table[CONTAINER_MAX];
    kprintf("%-24s %-12s %-12s %-24s\n", "CONTAINER ID", "STATE", "PID", "NAME");
    kprintf("------------------------------------------------------------------\n");
    int count = 0;
    for (int i = 0; i < CONTAINER_MAX; i++) {
        if (!container_table[i].in_use)
            continue;
        if (!show_all && container_table[i].state != CONTAINER_STATE_RUNNING)
            continue;
        kprintf("%-24s %-12s %-12u %-24s\n",
            container_table[i].id,
            container_state_name(container_table[i].state),
            container_table[i].init_pid,
            container_table[i].name);
        count++;
    }
    if (count == 0)
        kprintf("(no running containers)\n");
    else
        kprintf("\nTotal: %d container(s)\n", count);
    return 0;
}

/* ── 'inspect' subcommand ──────────────────────────────────────────── */

static int crictl_inspect(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: crictl inspect <container_id>\n");
        return 0;
    }
    struct container *c = crictl_lookup(argv[1]);
    if (!c) {
        kprintf("crictl: container '%s' not found\n", argv[1]);
        return 0;
    }

    kprintf("Container %s:\n", c->id);
    kprintf("  Name:        %s\n", c->name[0] ? c->name : "(unnamed)");
    kprintf("  State:       %s\n", container_state_name(c->state));
    kprintf("  PID:         %u\n", c->init_pid);
    kprintf("  Created by:  %u\n", c->creator_pid);
    kprintf("  Data dir:    %s\n", c->data_dir);
    kprintf("  Rootfs:      %s\n", c->rootfs_path);
    kprintf("  Memory:      %llu bytes\n", (unsigned long long)c->memory_limit);
    kprintf("  CPU shares:  %llu\n", (unsigned long long)c->cpu_shares);
    kprintf("  PIDs limit:  %u\n", c->pids_limit);
    return 0;
}

/* ── 'exec' subcommand ─────────────────────────────────────────────── */

static int crictl_exec(int argc, char **argv) {
    if (argc < 3) {
        kprintf("Usage: crictl exec <container_id> <command> [args...]\n");
        return 0;
    }
    struct container *c = crictl_lookup(argv[1]);
    if (!c) {
        kprintf("crictl: container '%s' not found\n", argv[1]);
        return 0;
    }
    if (c->state != CONTAINER_STATE_RUNNING) {
        kprintf("crictl: container '%s' is not running\n", argv[1]);
        return 0;
    }
    (void)(argc); /* argc unused beyond bounds check */
    char **exec_argv = argv + 2;
    int ret = container_exec(c, exec_argv[0], exec_argv, NULL);
    if (ret < 0)
        kprintf("crictl: exec failed: %d\n", ret);
    return 0;
}

/* ── 'logs' subcommand ─────────────────────────────────────────────── */

static int crictl_logs(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: crictl logs <container_id>\n");
        return 0;
    }
    struct container *c = crictl_lookup(argv[1]);
    if (!c) {
        kprintf("crictl: container '%s' not found\n", argv[1]);
        return 0;
    }
    kprintf("--- Logs for %s ---\n", c->id);
    char logpath[CONTAINER_STATE_PATH];
    snprintf(logpath, sizeof(logpath), "%s/log/output.log", c->data_dir);
    char logbuf[4096];
    uint32_t logsize = 0;
    int ret = vfs_read(logpath, logbuf, sizeof(logbuf) - 1, &logsize);
    if (ret == 0 && logsize > 0) {
        logbuf[logsize < sizeof(logbuf) ? logsize : sizeof(logbuf) - 1] = '\0';
        kprintf("%s", logbuf);
        if (logsize >= sizeof(logbuf) - 1)
            kprintf("\n...(truncated)");
    } else {
        kprintf("(no log data found at %s)\n", logpath);
    }
    return 0;
}

/* ── 'stats' subcommand ────────────────────────────────────────────── */

static int crictl_stats(int argc, char **argv) {
    if (argc < 2) {
        /* Print stats for all running containers */
        extern struct container container_table[CONTAINER_MAX];
        kprintf("%-24s %-16s %-16s %-16s\n", "CONTAINER ID", "CPU (us)", "MEMORY", "PIDS");
        kprintf("----------------------------------------------------------------------\n");
        for (int i = 0; i < CONTAINER_MAX; i++) {
            if (!container_table[i].in_use ||
                container_table[i].state != CONTAINER_STATE_RUNNING)
                continue;
            struct container_stats stats;
            if (container_stats(&container_table[i], &stats) == 0) {
                kprintf("%-24s %-16llu %-16llu %-16llu\n",
                    container_table[i].id,
                    (unsigned long long)stats.cpu_usage_us,
                    (unsigned long long)stats.memory_usage_bytes,
                    (unsigned long long)stats.pids_current);
            }
        }
        return 0;
    }

    /* Stats for a specific container */
    struct container *c = crictl_lookup(argv[1]);
    if (!c) {
        kprintf("crictl: container '%s' not found\n", argv[1]);
        return 0;
    }
    struct container_stats stats;
    int ret = container_stats(c, &stats);
    if (ret < 0) {
        kprintf("crictl: stats failed: %d\n", ret);
        return 0;
    }
    kprintf("Container %s:\n", c->id);
    kprintf("  CPU usage:       %llu us\n", (unsigned long long)stats.cpu_usage_us);
    kprintf("  Memory usage:    %llu bytes\n", (unsigned long long)stats.memory_usage_bytes);
    kprintf("  Memory limit:    %llu bytes\n", (unsigned long long)stats.memory_limit_bytes);
    kprintf("  PIDs current:    %llu\n", (unsigned long long)stats.pids_current);
    kprintf("  PIDs limit:      %llu\n", (unsigned long long)stats.pids_limit);
    kprintf("  I/O read:        %llu bytes\n", (unsigned long long)stats.io_read_bytes);
    kprintf("  I/O write:       %llu bytes\n", (unsigned long long)stats.io_write_bytes);
    return 0;
}

/* ── 'image' subcommand ────────────────────────────────────────────── */

/* Image tracking */
#define CRICTL_MAX_IMAGES 32
static struct {
    char name[128];
    char tag[64];
    int  in_use;
} crictl_images[CRICTL_MAX_IMAGES];
static int crictl_images_init;

static void crictl_ensure_images_init(void) {
    if (!crictl_images_init) {
        for (int i = 0; i < CRICTL_MAX_IMAGES; i++)
            crictl_images[i].in_use = 0;
        crictl_images_init = 1;
    }
}

static int crictl_image(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: crictl image list|pull <name>\n");
        return 0;
    }

    /* image list */
    if (strcmp(argv[1], "list") == 0) {
        crictl_ensure_images_init();
        kprintf("%-48s %-16s\n", "IMAGE", "TAG");
        kprintf("--------------------------------------------------------------\n");
        int count = 0;
        for (int i = 0; i < CRICTL_MAX_IMAGES; i++) {
            if (crictl_images[i].in_use) {
                kprintf("%-48s %-16s\n", crictl_images[i].name, crictl_images[i].tag);
                count++;
            }
        }
        if (count == 0) kprintf("(no images)\n");
        else kprintf("\nTotal: %d image(s)\n", count);
        return 0;
    }

    /* image pull */
    if (strcmp(argv[1], "pull") == 0) {
        if (argc < 3) {
            kprintf("Usage: crictl image pull <image_name>[:tag]\n");
            return 0;
        }
        crictl_ensure_images_init();
        int slot = -1;
        for (int i = 0; i < CRICTL_MAX_IMAGES; i++) {
            if (!crictl_images[i].in_use) { slot = i; break; }
        }
        if (slot < 0) {
            kprintf("crictl: image table full\n");
            return 0;
        }
        const char *colon = strchr(argv[2], ':');
        if (colon) {
            int n = (int)(colon - argv[2]);
            if (n > 127) n = 127;
            strncpy(crictl_images[slot].name, argv[2], (size_t)n);
            crictl_images[slot].name[n] = '\0';
            strncpy(crictl_images[slot].tag, colon + 1, 63);
            crictl_images[slot].tag[63] = '\0';
        } else {
            strncpy(crictl_images[slot].name, argv[2], 127);
            crictl_images[slot].name[127] = '\0';
            strncpy(crictl_images[slot].tag, "latest", 63);
        }
        crictl_images[slot].in_use = 1;
        kprintf("Pulled image: %s:%s\n", crictl_images[slot].name, crictl_images[slot].tag);
        return 0;
    }

    kprintf("crictl image: unknown action '%s'\n", argv[1]);
    return 0;
}
