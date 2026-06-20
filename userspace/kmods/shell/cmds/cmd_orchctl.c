#define KERNEL_INTERNAL
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "container.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "process.h"

/*
 * cmd_orchctl.c — orchestration CLI (Item C198)
 *
 * Kubernetes-style orchestration control CLI for managing pods,
 * nodes, services, namespaces, and cluster operations.
 *
 * Usage: orchctl apply [-f <file>]
 *        orchctl get pods|nodes|services|namespaces [name]
 *        orchctl describe pod <name>
 *        orchctl logs <pod_name> [-c <container>]
 *        orchctl exec <pod_name> [-- <command> [args...]]
 *        orchctl cluster-info
 *        orchctl drain <node_name>
 *        orchctl cordon <node_name>
 *        orchctl uncordon <node_name>
 */

/* ── Forward declarations ──────────────────────────────────────────── */
static int orchctl_apply(int argc, char **argv);
static int orchctl_get(int argc, char **argv);
static int orchctl_describe(int argc, char **argv);
static int orchctl_logs_cmd(int argc, char **argv);
static int orchctl_exec_cmd(int argc, char **argv);
static int orchctl_cluster_info(void);
static int orchctl_drain(int argc, char **argv);
static int orchctl_cordon(int argc, char **argv);
static int orchctl_uncordon(int argc, char **argv);

/* ── Orchestration resource structures ─────────────────────────────── */

#define ORCHCTL_MAX_PODS        32
#define ORCHCTL_MAX_NODES       16
#define ORCHCTL_MAX_SERVICES    16
#define ORCHCTL_MAX_NAMESPACES  8
#define ORCHCTL_NAME_MAX        128

struct orchctl_pod {
    char name[ORCHCTL_NAME_MAX];
    char namespace[64];
    char node[ORCHCTL_NAME_MAX];
    char image[128];
    char status[16];           /* Running, Pending, Succeeded, Failed, Unknown */
    uint32_t pid;
    int  in_use;
};

struct orchctl_node {
    char name[ORCHCTL_NAME_MAX];
    char status[16];           /* Ready, NotReady, SchedulingDisabled */
    char role[32];
    uint64_t cpu_capacity;
    uint64_t mem_capacity;
    int  in_use;
};

struct orchctl_service {
    char name[ORCHCTL_NAME_MAX];
    char namespace[64];
    char cluster_ip[16];
    char type[16];             /* ClusterIP, NodePort, LoadBalancer */
    uint16_t port;
    int  in_use;
};

struct orchctl_namespace {
    char name[64];
    char status[16];           /* Active, Terminating */
    int  in_use;
};

/* ── Global state ──────────────────────────────────────────────────── */

static struct orchctl_pod       orchctl_pods[ORCHCTL_MAX_PODS];
static struct orchctl_node      orchctl_nodes[ORCHCTL_MAX_NODES];
static struct orchctl_service   orchctl_services[ORCHCTL_MAX_SERVICES];
static struct orchctl_namespace orchctl_namespaces[ORCHCTL_MAX_NAMESPACES];
static int orchctl_initialized;

static void orchctl_ensure_init(void) {
    if (orchctl_initialized) return;

    for (int i = 0; i < ORCHCTL_MAX_PODS; i++)
        orchctl_pods[i].in_use = 0;
    for (int i = 0; i < ORCHCTL_MAX_NODES; i++)
        orchctl_nodes[i].in_use = 0;
    for (int i = 0; i < ORCHCTL_MAX_SERVICES; i++)
        orchctl_services[i].in_use = 0;
    for (int i = 0; i < ORCHCTL_MAX_NAMESPACES; i++)
        orchctl_namespaces[i].in_use = 0;

    /* Seed a default namespace */
    strncpy(orchctl_namespaces[0].name, "default", 63);
    strncpy(orchctl_namespaces[0].status, "Active", 15);
    orchctl_namespaces[0].in_use = 1;

    /* Seed a local node */
    strncpy(orchctl_nodes[0].name, "osdev-node-0", ORCHCTL_NAME_MAX - 1);
    strncpy(orchctl_nodes[0].status, "Ready", 15);
    strncpy(orchctl_nodes[0].role, "control-plane", 31);
    orchctl_nodes[0].cpu_capacity = 4;
    orchctl_nodes[0].mem_capacity = 8192; /* MB */
    orchctl_nodes[0].in_use = 1;

    orchctl_initialized = 1;
}

/* ── Main dispatch ─────────────────────────────────────────────────── */

int cmd_orchctl(int argc, char **argv) {
    orchctl_ensure_init();

    if (argc < 2) {
        kprintf("Usage: orchctl <subcommand> [args...]\n\n");
        kprintf("Kubernetes-style orchestration CLI\n\n");
        kprintf("Subcommands:\n");
        kprintf("  apply          Apply a configuration (apply -f <file>)\n");
        kprintf("  get            Display resources (pods|nodes|services|namespaces)\n");
        kprintf("  describe       Describe a resource (describe pod <name>)\n");
        kprintf("  logs           Print logs from a pod\n");
        kprintf("  exec           Execute a command in a pod\n");
        kprintf("  cluster-info   Display cluster information\n");
        kprintf("  drain          Drain a node for maintenance\n");
        kprintf("  cordon         Mark a node as unschedulable\n");
        kprintf("  uncordon       Mark a node as schedulable\n");
        return 0;
    }

    if (strcmp(argv[1], "apply") == 0)
        return orchctl_apply(argc - 1, argv + 1);
    if (strcmp(argv[1], "get") == 0)
        return orchctl_get(argc - 1, argv + 1);
    if (strcmp(argv[1], "describe") == 0)
        return orchctl_describe(argc - 1, argv + 1);
    if (strcmp(argv[1], "logs") == 0)
        return orchctl_logs_cmd(argc - 1, argv + 1);
    if (strcmp(argv[1], "exec") == 0)
        return orchctl_exec_cmd(argc - 1, argv + 1);
    if (strcmp(argv[1], "cluster-info") == 0)
        return orchctl_cluster_info();
    if (strcmp(argv[1], "drain") == 0)
        return orchctl_drain(argc - 1, argv + 1);
    if (strcmp(argv[1], "cordon") == 0)
        return orchctl_cordon(argc - 1, argv + 1);
    if (strcmp(argv[1], "uncordon") == 0)
        return orchctl_uncordon(argc - 1, argv + 1);

    kprintf("orchctl: unknown subcommand '%s'. Try 'orchctl' for help.\n", argv[1]);
    return 0;
}

/* ── 'apply' subcommand ────────────────────────────────────────────── */

static int orchctl_apply(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[1], "-f") != 0) {
        kprintf("Usage: orchctl apply -f <manifest.json>\n");
        return 0;
    }

    kprintf("Applying configuration from '%s'...\n", argv[2]);

    /* Try to find an empty pod slot and create a pod entry */
    int slot = -1;
    for (int i = 0; i < ORCHCTL_MAX_PODS; i++) {
        if (!orchctl_pods[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        kprintf("orchctl: pod table full\n");
        return 0;
    }

    /* Create a minimal pod from the manifest name */
    strncpy(orchctl_pods[slot].name, argv[2], ORCHCTL_NAME_MAX - 1);
    orchctl_pods[slot].name[ORCHCTL_NAME_MAX - 1] = '\0';
    strncpy(orchctl_pods[slot].namespace, "default", 63);
    strncpy(orchctl_pods[slot].node, "osdev-node-0", ORCHCTL_NAME_MAX - 1);
    strncpy(orchctl_pods[slot].image, "alpine:latest", 127);
    strncpy(orchctl_pods[slot].status, "Running", 15);
    orchctl_pods[slot].pid = 0;
    orchctl_pods[slot].in_use = 1;

    kprintf("Pod '%s' created in namespace '%s'\n", orchctl_pods[slot].name, "default");
    return 0;
}

/* ── 'get' subcommand ──────────────────────────────────────────────── */

static int orchctl_get_pods(void) {
    kprintf("%-40s %-16s %-24s %-12s\n", "NAME", "NAMESPACE", "NODE", "STATUS");
    kprintf("--------------------------------------------------------------------------------\n");
    int count = 0;
    for (int i = 0; i < ORCHCTL_MAX_PODS; i++) {
        if (orchctl_pods[i].in_use) {
            kprintf("%-40s %-16s %-24s %-12s\n",
                orchctl_pods[i].name,
                orchctl_pods[i].namespace,
                orchctl_pods[i].node,
                orchctl_pods[i].status);
            count++;
        }
    }
    if (count == 0) kprintf("(no pods found)\n");
    else kprintf("\nTotal: %d pod(s)\n", count);
    return 0;
}

static int orchctl_get_nodes(void) {
    kprintf("%-32s %-16s %-20s %-12s %-12s\n", "NAME", "STATUS", "ROLE", "CPU", "MEMORY(MB)");
    kprintf("--------------------------------------------------------------------------------------------\n");
    int count = 0;
    for (int i = 0; i < ORCHCTL_MAX_NODES; i++) {
        if (orchctl_nodes[i].in_use) {
            kprintf("%-32s %-16s %-20s %-12llu %-12llu\n",
                orchctl_nodes[i].name,
                orchctl_nodes[i].status,
                orchctl_nodes[i].role,
                (unsigned long long)orchctl_nodes[i].cpu_capacity,
                (unsigned long long)orchctl_nodes[i].mem_capacity);
            count++;
        }
    }
    if (count == 0) kprintf("(no nodes)\n");
    else kprintf("\nTotal: %d node(s)\n", count);
    return 0;
}

static int orchctl_get_services(void) {
    kprintf("%-32s %-16s %-20s %-16s %-8s\n", "NAME", "NAMESPACE", "CLUSTER-IP", "TYPE", "PORT");
    kprintf("---------------------------------------------------------------------------------------\n");
    int count = 0;
    for (int i = 0; i < ORCHCTL_MAX_SERVICES; i++) {
        if (orchctl_services[i].in_use) {
            kprintf("%-32s %-16s %-20s %-16s %-8u\n",
                orchctl_services[i].name,
                orchctl_services[i].namespace,
                orchctl_services[i].cluster_ip,
                orchctl_services[i].type,
                orchctl_services[i].port);
            count++;
        }
    }
    if (count == 0) kprintf("(no services)\n");
    else kprintf("\nTotal: %d service(s)\n", count);
    return 0;
}

static int orchctl_get_namespaces(void) {
    kprintf("%-32s %-16s\n", "NAME", "STATUS");
    kprintf("------------------------------------------\n");
    int count = 0;
    for (int i = 0; i < ORCHCTL_MAX_NAMESPACES; i++) {
        if (orchctl_namespaces[i].in_use) {
            kprintf("%-32s %-16s\n",
                orchctl_namespaces[i].name,
                orchctl_namespaces[i].status);
            count++;
        }
    }
    if (count == 0) kprintf("(no namespaces)\n");
    else kprintf("\nTotal: %d namespace(s)\n", count);
    return 0;
}

static int orchctl_get(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: orchctl get pods|nodes|services|namespaces [name]\n");
        return 0;
    }

    if (strcmp(argv[1], "pods") == 0)
        return orchctl_get_pods();
    if (strcmp(argv[1], "nodes") == 0)
        return orchctl_get_nodes();
    if (strcmp(argv[1], "services") == 0)
        return orchctl_get_services();
    if (strcmp(argv[1], "namespaces") == 0)
        return orchctl_get_namespaces();

    kprintf("orchctl get: unknown resource type '%s'\n", argv[1]);
    return 0;
}

/* ── 'describe' subcommand ─────────────────────────────────────────── */

static int orchctl_describe(int argc, char **argv) {
    if (argc < 3) {
        kprintf("Usage: orchctl describe pod <name>\n");
        return 0;
    }

    if (strcmp(argv[1], "pod") == 0) {
        for (int i = 0; i < ORCHCTL_MAX_PODS; i++) {
            if (orchctl_pods[i].in_use && strcmp(orchctl_pods[i].name, argv[2]) == 0) {
                kprintf("Pod:          %s\n", orchctl_pods[i].name);
                kprintf("Namespace:    %s\n", orchctl_pods[i].namespace);
                kprintf("Node:         %s\n", orchctl_pods[i].node);
                kprintf("Image:        %s\n", orchctl_pods[i].image);
                kprintf("Status:       %s\n", orchctl_pods[i].status);
                kprintf("PID:          %u\n", orchctl_pods[i].pid);
                return 0;
            }
        }
        kprintf("orchctl: pod '%s' not found\n", argv[2]);
        return 0;
    }

    kprintf("orchctl describe: unsupported resource type '%s'\n", argv[1]);
    return 0;
}

/* ── 'logs' subcommand ─────────────────────────────────────────────── */

static int orchctl_logs_cmd(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: orchctl logs <pod_name> [-c <container>]\n");
        return 0;
    }

    const char *pod_name = argv[1];
    const char *container_name = NULL;
    if (argc >= 4 && strcmp(argv[2], "-c") == 0)
        container_name = argv[3];

    for (int i = 0; i < ORCHCTL_MAX_PODS; i++) {
        if (orchctl_pods[i].in_use && strcmp(orchctl_pods[i].name, pod_name) == 0) {
            kprintf("--- Logs for pod '%s'", pod_name);
            if (container_name)
                kprintf(", container '%s'", container_name);
            kprintf(" ---\n");
            /* Find associated container by matching names */
            extern struct container container_table[CONTAINER_MAX];
            int found = 0;
            for (int j = 0; j < CONTAINER_MAX; j++) {
                if (container_table[j].in_use &&
                    strcmp(container_table[j].name, pod_name) == 0) {
                    char logpath[CONTAINER_STATE_PATH];
                    snprintf(logpath, sizeof(logpath), "%s/log/output.log",
                             container_table[j].data_dir);
                    kprintf("(log path: %s)\n", logpath);
                    found = 1;
                    break;
                }
            }
            if (!found)
                kprintf("(no log data available)\n");
            return 0;
        }
    }
    kprintf("orchctl: pod '%s' not found\n", pod_name);
    return 0;
}

/* ── 'exec' subcommand ─────────────────────────────────────────────── */

static int orchctl_exec_cmd(int argc, char **argv) {
    if (argc < 3) {
        kprintf("Usage: orchctl exec <pod_name> -- <command> [args...]\n");
        return 0;
    }

    const char *pod_name = argv[1];
    int cmd_start = 2;
    if (argc > 2 && strcmp(argv[2], "--") == 0)
        cmd_start = 3;
    if (cmd_start >= argc) {
        kprintf("orchctl: no command specified\n");
        return 0;
    }

    for (int i = 0; i < ORCHCTL_MAX_PODS; i++) {
        if (orchctl_pods[i].in_use && strcmp(orchctl_pods[i].name, pod_name) == 0) {
            extern struct container container_table[CONTAINER_MAX];
            for (int j = 0; j < CONTAINER_MAX; j++) {
                if (container_table[j].in_use &&
                    strcmp(container_table[j].name, pod_name) == 0) {
                    int ret = container_exec(&container_table[j],
                        argv[cmd_start], argv + cmd_start, NULL);
                    if (ret < 0)
                        kprintf("orchctl: exec failed: %d\n", ret);
                    return 0;
                }
            }
            kprintf("orchctl: no container found for pod '%s'\n", pod_name);
            return 0;
        }
    }
    kprintf("orchctl: pod '%s' not found\n", pod_name);
    return 0;
}

/* ── 'cluster-info' subcommand ─────────────────────────────────────── */

static int orchctl_cluster_info(void) {
    kprintf("Kubernetes-style orchestration cluster info\n");
    kprintf("============================================\n\n");

    kprintf("Control plane:\n");
    for (int i = 0; i < ORCHCTL_MAX_NODES; i++) {
        if (orchctl_nodes[i].in_use) {
            kprintf("  %s: %s\n", orchctl_nodes[i].name, orchctl_nodes[i].status);
        }
    }

    kprintf("\nResources:\n");
    int pod_count = 0, svc_count = 0, ns_count = 0;
    for (int i = 0; i < ORCHCTL_MAX_PODS; i++)
        if (orchctl_pods[i].in_use) pod_count++;
    for (int i = 0; i < ORCHCTL_MAX_SERVICES; i++)
        if (orchctl_services[i].in_use) svc_count++;
    for (int i = 0; i < ORCHCTL_MAX_NAMESPACES; i++)
        if (orchctl_namespaces[i].in_use) ns_count++;
    kprintf("  Pods:       %d\n", pod_count);
    kprintf("  Services:   %d\n", svc_count);
    kprintf("  Namespaces: %d\n", ns_count);

    kprintf("\nKubernetes master is running at http://localhost:6443\n");
    kprintf("CoreDNS is running at http://localhost:53\n");
    return 0;
}

/* ── 'drain', 'cordon', 'uncordon' subcommands ─────────────────────── */

static int orchctl_find_node(const char *name) {
    for (int i = 0; i < ORCHCTL_MAX_NODES; i++) {
        if (orchctl_nodes[i].in_use && strcmp(orchctl_nodes[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int orchctl_drain(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: orchctl drain <node_name>\n");
        return 0;
    }
    int idx = orchctl_find_node(argv[1]);
    if (idx < 0) {
        kprintf("orchctl: node '%s' not found\n", argv[1]);
        return 0;
    }
    kprintf("Draining node '%s'...\n", argv[1]);
    /* Remove pods from this node */
    for (int i = 0; i < ORCHCTL_MAX_PODS; i++) {
        if (orchctl_pods[i].in_use &&
            strcmp(orchctl_pods[i].node, argv[1]) == 0) {
            kprintf("  Evicting pod '%s'...\n", orchctl_pods[i].name);
            orchctl_pods[i].in_use = 0;
        }
    }
    strncpy(orchctl_nodes[idx].status, "SchedulingDisabled", 15);
    kprintf("Node '%s' drained and marked unschedulable\n", argv[1]);
    return 0;
}

static int orchctl_cordon(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: orchctl cordon <node_name>\n");
        return 0;
    }
    int idx = orchctl_find_node(argv[1]);
    if (idx < 0) {
        kprintf("orchctl: node '%s' not found\n", argv[1]);
        return 0;
    }
    strncpy(orchctl_nodes[idx].status, "SchedulingDisabled", 15);
    kprintf("Node '%s' cordoned (marked unschedulable)\n", argv[1]);
    return 0;
}

static int orchctl_uncordon(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: orchctl uncordon <node_name>\n");
        return 0;
    }
    int idx = orchctl_find_node(argv[1]);
    if (idx < 0) {
        kprintf("orchctl: node '%s' not found\n", argv[1]);
        return 0;
    }
    strncpy(orchctl_nodes[idx].status, "Ready", 15);
    kprintf("Node '%s' uncordoned (marked schedulable)\n", argv[1]);
    return 0;
}
