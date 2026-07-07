/*
 * clusterd — Userspace Cluster Management Daemon
 *
 * Replaces the in-kernel clustering subsystem. Runs as a userspace process
 * communicating with the kernel via netlink (NETLINK_RAS), syscalls, sockets.
 *
 * Responsibilities:
 *   - Node management: join/leave cluster, heartbeat, health reporting
 *   - Raft consensus: leader election, log replication, KV store
 *   - Gossip protocol: membership, failure detection, state sync
 *   - Overlay networking: VXLAN setup, WireGuard mesh
 *   - Network policies: ingress/egress rules via nftables
 *   - Ingress controller: NodePort, LoadBalancer, HTTP routing
 *   - Controllers: deployments, statefulsets, daemonsets
 *   - HPA/VPA: horizontal/vertical pod autoscaling
 *   - Cluster autoscaler: node add/remove
 *   - CRD management, operator framework
 *   - Security policies, RBAC, secrets
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/sysinfo.h>
#include <json-c/json.h>

/* ─── Configuration ────────────────────────────────────────────────── */

#define CLUSTERD_PORT     8376
#define API_PORT           8375
#define GOSSIP_PORT        8377
#define RAFT_PORT          8378
#define HEARTBEAT_INTERVAL 5   /* seconds */
#define GOSSIP_INTERVAL    3
#define RAFT_TIMEOUT_MIN   150 /* ms */
#define RAFT_TIMEOUT_MAX   300

/* ─── Data Structures ──────────────────────────────────────────────── */

enum node_state {
    NODE_UNKNOWN,
    NODE_ALIVE,
    NODE_SUSPECT,
    NODE_DEAD,
};

enum raft_state {
    RAFT_FOLLOWER,
    RAFT_CANDIDATE,
    RAFT_LEADER,
};

struct node {
    uint64_t    id;
    char        name[64];
    char        addr[INET6_ADDRSTRLEN];
    int         port;
    enum node_state state;
    uint64_t    incarnation;
    time_t      last_seen;
    uint64_t    cpu_usage;
    uint64_t    memory_usage;
    int         pod_count;
    pthread_mutex_t lock;
    struct node *next;
};

struct raft_log_entry {
    uint64_t    index;
    uint64_t    term;
    int         type; /* 0=command, 1=config */
    char        data[4096];
};

struct cluster_state {
    /* Node membership */
    struct node *nodes;
    int          node_count;
    uint64_t     local_node_id;
    char         local_addr[INET6_ADDRSTRLEN];

    /* Raft */
    enum raft_state raft_state;
    uint64_t    current_term;
    uint64_t    voted_for;
    uint64_t    commit_index;
    uint64_t    last_applied;
    struct raft_log_entry *log;
    int         log_count;
    int         log_capacity;
    uint64_t    elected_leader;

    /* Gossip */
    uint64_t    gossip_round;

    /* Locks */
    pthread_mutex_t state_lock;
    pthread_mutex_t log_lock;

    /* Sockets */
    int raft_fd;
    int gossip_fd;
    int api_fd;
    int nl_sock;

    /* Running */
    volatile int running;
};

static struct cluster_state g_cluster;

/* ─── Netlink Kernel Communication ──────────────────────────────────── */

static int netlink_sock_open(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_RAS);
    if (fd < 0) {
        /* Fall back to generic netlink if RAS not available */
        fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    }
    if (fd < 0) {
        perror("netlink socket");
        return -1;
    }

    struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_pid = getpid() };
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("netlink bind");
        close(fd);
        return -1;
    }
    return fd;
}

static int netlink_send(int fd, int type, const void *data, size_t len)
{
    struct nlmsghdr *nlh = malloc(NLMSG_SPACE(len));
    if (!nlh) return -1;

    nlh->nlmsg_len = NLMSG_SPACE(len);
    nlh->nlmsg_type = type;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    nlh->nlmsg_seq = rand();
    nlh->nlmsg_pid = getpid();

    struct sockaddr_nl dst = { .nl_family = AF_NETLINK, .nl_pid = 0 };
    struct iovec iov = { .iov_base = nlh, .iov_len = nlh->nlmsg_len };
    struct msghdr msg = { .msg_name = &dst, .msg_namelen = sizeof(dst), .msg_iov = &iov, .msg_iovlen = 1 };

    int ret = sendmsg(fd, &msg, 0);
    free(nlh);
    return ret;
}

/* ─── Node Management ──────────────────────────────────────────────── */

static struct node *node_find(uint64_t id)
{
    for (struct node *n = g_cluster.nodes; n; n = n->next)
        if (n->id == id) return n;
    return NULL;
}

static struct node *node_add(const char *addr, int port, const char *name)
{
    struct node *n = calloc(1, sizeof(*n));
    if (!n) return NULL;

    n->id = (uint64_t)rand() << 32 | rand();
    strncpy(n->addr, addr, sizeof(n->addr)-1);
    n->port = port;
    strncpy(n->name, name ? name : addr, sizeof(n->name)-1);
    n->state = NODE_ALIVE;
    n->incarnation = 1;
    n->last_seen = time(NULL);
    pthread_mutex_init(&n->lock, NULL);

    pthread_mutex_lock(&g_cluster.state_lock);
    n->next = g_cluster.nodes;
    g_cluster.nodes = n;
    g_cluster.node_count++;
    pthread_mutex_unlock(&g_cluster.state_lock);

    return n;
}

static void node_remove(uint64_t id)
{
    pthread_mutex_lock(&g_cluster.state_lock);
    struct node **pp = &g_cluster.nodes;
    while (*pp) {
        struct node *n = *pp;
        if (n->id == id) {
            *pp = n->next;
            pthread_mutex_destroy(&n->lock);
            free(n);
            g_cluster.node_count--;
            break;
        }
        pp = &n->next;
    }
    pthread_mutex_unlock(&g_cluster.state_lock);
}

static void node_heartbeat_check(void)
{
    time_t now = time(NULL);
    pthread_mutex_lock(&g_cluster.state_lock);
    for (struct node *n = g_cluster.nodes; n; n = n->next) {
        if (n->id == g_cluster.local_node_id) continue;
        if (now - n->last_seen > 15) {
            n->state = NODE_SUSPECT;
        }
        if (now - n->last_seen > 30) {
            n->state = NODE_DEAD;
        }
    }
    pthread_mutex_unlock(&g_cluster.state_lock);
}

/* ─── Gossip Protocol ──────────────────────────────────────────────── */

static void gossip_send_node(int fd, struct node *target)
{
    char buf[512];
    pthread_mutex_lock(&g_cluster.state_lock);
    for (struct node *n = g_cluster.nodes; n; n = n->next) {
        if (n->id == target->id) continue;
        int len = snprintf(buf, sizeof(buf),
            "NODE|%016lx|%s|%s:%d|%d|%ld|%lu|%lu|%d\n",
            n->id, n->name, n->addr, n->port,
            (int)n->state, n->incarnation,
            n->cpu_usage, n->memory_usage, n->pod_count);

        struct sockaddr_in dst = {
            .sin_family = AF_INET,
            .sin_port = htons(target->port ? target->port : GOSSIP_PORT),
        };
        inet_pton(AF_INET, target->addr, &dst.sin_addr);
        sendto(fd, buf, len, 0, (struct sockaddr *)&dst, sizeof(dst));
    }
    pthread_mutex_unlock(&g_cluster.state_lock);
}

static void gossip_handle_message(const char *msg, struct sockaddr_in *from)
{
    char type[16];
    uint64_t id;
    char name[64], addr[64];
    int port, state_val;
    uint64_t inc, cpu, mem;
    int pods;

    if (sscanf(msg, "NODE|%lx|%63[^|]|%63[^:]:%d|%d|%ld|%lu|%lu|%d",
               &id, name, addr, &port, &state_val, &inc, &cpu, &mem, &pods) >= 5) {
        pthread_mutex_lock(&g_cluster.state_lock);
        struct node *n = node_find(id);
        if (!n) {
            n = node_add(addr, port, name);
            if (n) printf("clusterd: new node %s (id=%016lx)\n", name, id);
        }
        if (n && inc > n->incarnation) {
            n->state = (enum node_state)state_val;
            n->incarnation = inc;
            n->cpu_usage = cpu;
            n->memory_usage = mem;
            n->pod_count = pods;
            n->last_seen = time(NULL);
        }
        pthread_mutex_unlock(&g_cluster.state_lock);
    }
}

static void *gossip_thread(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("gossip socket"); return NULL; }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(GOSSIP_PORT),
        .sin_addr = { .s_addr = INADDR_ANY },
    };
    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("gossip bind");
        close(fd);
        return NULL;
    }

    char buf[2048];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);

    while (g_cluster.running) {
        /* Send gossip to a random node */
        pthread_mutex_lock(&g_cluster.state_lock);
        struct node *target = NULL;
        int count = g_cluster.node_count;
        if (count > 1) {
            int idx = rand() % count;
            int i = 0;
            for (struct node *n = g_cluster.nodes; n; n = n->next, i++) {
                if (n->id != g_cluster.local_node_id && i == idx) {
                    target = n;
                    break;
                }
            }
        }
        pthread_mutex_unlock(&g_cluster.state_lock);

        if (target) gossip_send_node(fd, target);

        /* Receive gossip */
        struct timeval tv = { .tv_sec = GOSSIP_INTERVAL, .tv_usec = 0 };
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);

        if (select(fd+1, &fds, NULL, NULL, &tv) > 0) {
            int n = recvfrom(fd, buf, sizeof(buf)-1, 0, (struct sockaddr *)&from, &from_len);
            if (n > 0) {
                buf[n] = '\0';
                gossip_handle_message(buf, &from);
            }
        }

        node_heartbeat_check();
        g_cluster.gossip_round++;
    }

    close(fd);
    return NULL;
}

/* ─── Raft Consensus ────────────────────────────────────────────────── */

static void raft_append_entry(uint64_t term, int type, const char *data)
{
    pthread_mutex_lock(&g_cluster.log_lock);
    if (g_cluster.log_count >= g_cluster.log_capacity) {
        int new_cap = g_cluster.log_capacity ? g_cluster.log_capacity * 2 : 64;
        struct raft_log_entry *new_log = realloc(g_cluster.log, new_cap * sizeof(*new_log));
        if (!new_log) { pthread_mutex_unlock(&g_cluster.log_lock); return; }
        g_cluster.log = new_log;
        g_cluster.log_capacity = new_cap;
    }

    struct raft_log_entry *e = &g_cluster.log[g_cluster.log_count++];
    e->index = g_cluster.log_count;
    e->term = term;
    e->type = type;
    strncpy(e->data, data, sizeof(e->data)-1);
    pthread_mutex_unlock(&g_cluster.log_lock);
}

static void raft_become_follower(uint64_t term)
{
    g_cluster.current_term = term;
    g_cluster.raft_state = RAFT_FOLLOWER;
    g_cluster.voted_for = 0;
}

static void raft_become_candidate(void)
{
    g_cluster.raft_state = RAFT_CANDIDATE;
    g_cluster.current_term++;
    g_cluster.voted_for = g_cluster.local_node_id;
    raft_append_entry(g_cluster.current_term, 1, "{}");
}

static void raft_become_leader(void)
{
    g_cluster.raft_state = RAFT_LEADER;
    g_cluster.elected_leader = g_cluster.local_node_id;
    printf("clusterd: elected as leader (term %lu)\n", g_cluster.current_term);
}

static int raft_send_request_vote(int fd, struct node *target)
{
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "RV|%lu|%lu|%lu|%d\n",
        g_cluster.current_term, g_cluster.local_node_id,
        g_cluster.log_count ? g_cluster.log[g_cluster.log_count-1].index : 0,
        g_cluster.log_count ? g_cluster.log[g_cluster.log_count-1].term : 0);

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(RAFT_PORT),
    };
    inet_pton(AF_INET, target->addr, &dst.sin_addr);
    return sendto(fd, buf, len, 0, (struct sockaddr *)&dst, sizeof(dst));
}

static void *raft_thread(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("raft socket"); return NULL; }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(RAFT_PORT),
        .sin_addr = { .s_addr = INADDR_ANY },
    };
    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("raft bind");
        close(fd);
        return NULL;
    }

    char buf[4096];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    struct timeval tv;

    while (g_cluster.running) {
        int timeout_ms = RAFT_TIMEOUT_MIN + rand() % (RAFT_TIMEOUT_MAX - RAFT_TIMEOUT_MIN);

        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);

        int ret = select(fd+1, &fds, NULL, NULL, &tv);

        if (ret == 0) {
            /* Timeout — if follower, become candidate */
            if (g_cluster.raft_state == RAFT_FOLLOWER) {
                raft_become_candidate();
                pthread_mutex_lock(&g_cluster.state_lock);
                for (struct node *n = g_cluster.nodes; n; n = n->next) {
                    if (n->id != g_cluster.local_node_id)
                        raft_send_request_vote(fd, n);
                }
                pthread_mutex_unlock(&g_cluster.state_lock);
            }
            continue;
        }

        int n = recvfrom(fd, buf, sizeof(buf)-1, 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) continue;
        buf[n] = '\0';

        /* Parse Raft messages */
        uint64_t term, candidate_id, last_log_idx, last_log_term;
        if (sscanf(buf, "RV|%lu|%lu|%lu|%lu", &term, &candidate_id, &last_log_idx, &last_log_term) == 4) {
            /* RequestVote */
            char resp[128];
            int vote = 0;
            if (term > g_cluster.current_term) raft_become_follower(term);
            if (term >= g_cluster.current_term && g_cluster.voted_for == 0) {
                g_cluster.voted_for = candidate_id;
                vote = 1;
            }
            int len = snprintf(resp, sizeof(resp), "VR|%lu|%lu|%d\n", g_cluster.current_term, g_cluster.local_node_id, vote);
            sendto(fd, resp, len, 0, (struct sockaddr *)&from, sizeof(from));
        }

        uint64_t from_term, from_id;
        int granted;
        if (sscanf(buf, "VR|%lu|%lu|%d", &from_term, &from_id, &granted) == 3) {
            if (granted && g_cluster.raft_state == RAFT_CANDIDATE) {
                /* Count votes — simplified: single vote = leader */
                raft_become_leader();
            }
        }
    }

    close(fd);
    return NULL;
}

/* ─── REST API Server ──────────────────────────────────────────────── */

static void api_handle_request(int client_fd)
{
    char buf[8192], resp[16384];
    int n = read(client_fd, buf, sizeof(buf)-1);
    if (n <= 0) { close(client_fd); return; }
    buf[n] = '\0';

    /* Parse HTTP request */
    char method[16], path[256];
    sscanf(buf, "%15s %255s", method, path);

    if (strcmp(path, "/api/v1/nodes") == 0 && strcmp(method, "GET") == 0) {
        /* Node list */
        int off = snprintf(resp, sizeof(resp),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n[");
        pthread_mutex_lock(&g_cluster.state_lock);
        int first = 1;
        for (struct node *n = g_cluster.nodes; n; n = n->next) {
            off += snprintf(resp+off, sizeof(resp)-off,
                "%s{\"id\":\"%016lx\",\"name\":\"%s\",\"addr\":\"%s:%d\","
                "\"state\":%d,\"cpu\":%lu,\"mem\":%lu,\"pods\":%d}",
                first ? "" : ",", n->id, n->name, n->addr, n->port,
                (int)n->state, n->cpu_usage, n->memory_usage, n->pod_count);
            first = 0;
        }
        pthread_mutex_unlock(&g_cluster.state_lock);
        off += snprintf(resp+off, sizeof(resp)-off, "]\n");
        write(client_fd, resp, off);
    } else if (strcmp(path, "/healthz") == 0) {
        const char *ok = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nok\n";
        write(client_fd, ok, strlen(ok));
    } else {
        const char *not_found = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n\r\nNot Found\n";
        write(client_fd, not_found, strlen(not_found));
    }

    close(client_fd);
}

static void *api_thread(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("api socket"); return NULL; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(API_PORT),
        .sin_addr = { .s_addr = INADDR_ANY },
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("api bind");
        close(fd);
        return NULL;
    }
    listen(fd, 128);

    while (g_cluster.running) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int client_fd = accept4(fd, (struct sockaddr *)&client, &client_len, SOCK_CLOEXEC);
        if (client_fd < 0) continue;
        api_handle_request(client_fd);
    }

    close(fd);
    return NULL;
}

/* ─── Kernel Communication ─────────────────────────────────────────── */

static void sync_state_to_kernel(void)
{
    /* Use netlink to sync cluster state back to kernel */
    json_object *root = json_object_new_object();
    json_object_object_add(root, "node_id", json_object_new_int64(g_cluster.local_node_id));
    json_object_object_add(root, "leader", json_object_new_int64(g_cluster.elected_leader));
    json_object_object_add(root, "term", json_object_new_int64(g_cluster.current_term));

    json_object *nodes_arr = json_object_new_array();
    pthread_mutex_lock(&g_cluster.state_lock);
    for (struct node *n = g_cluster.nodes; n; n = n->next) {
        json_object *jnode = json_object_new_object();
        json_object_object_add(jnode, "id", json_object_new_int64(n->id));
        json_object_object_add(jnode, "name", json_object_new_string(n->name));
        json_object_object_add(jnode, "state", json_object_new_int(n->state));
        json_object_object_add(jnode, "addr", json_object_new_string(n->addr));
        json_object_array_add(nodes_arr, jnode);
    }
    pthread_mutex_unlock(&g_cluster.state_lock);
    json_object_object_add(root, "nodes", nodes_arr);

    const char *json_str = json_object_to_json_string(root);
    if (g_cluster.nl_sock >= 0)
        netlink_send(g_cluster.nl_sock, 1, json_str, strlen(json_str)+1);
    json_object_put(root);
}

/* ─── Heartbeat ─────────────────────────────────────────────────────── */

static void *heartbeat_thread(void *arg)
{
    (void)arg;
    while (g_cluster.running) {
        sleep(HEARTBEAT_INTERVAL);
        node_heartbeat_check();
        sync_state_to_kernel();
    }
    return NULL;
}

/* ─── Signal Handling ──────────────────────────────────────────────── */

static void sig_handler(int sig)
{
    (void)sig;
    g_cluster.running = 0;
}

/* ─── Initialization ───────────────────────────────────────────────── */

static void load_config(void)
{
    /* Parse /etc/clusterd.conf or environment */
    const char *addr = getenv("CLUSTERD_ADDR");
    if (!addr) addr = "127.0.0.1";
    strncpy(g_cluster.local_addr, addr, sizeof(g_cluster.local_addr)-1);

    const char *peers = getenv("CLUSTERD_PEERS");
    if (peers) {
        char *copy = strdup(peers);
        char *tok = strtok(copy, ",");
        while (tok) {
            char paddr[64]; int pport = GOSSIP_PORT;
            if (sscanf(tok, "%63[^:]:%d", paddr, &pport) >= 1) {
                char pname[64];
                snprintf(pname, sizeof(pname), "peer-%s", paddr);
                node_add(paddr, pport, pname);
            }
            tok = strtok(NULL, ",");
        }
        free(copy);
    }

    /* Add self */
    char self_name[64];
    snprintf(self_name, sizeof(self_name), "node-%s", addr);
    struct node *self = node_add(addr, GOSSIP_PORT, self_name);
    if (self) g_cluster.local_node_id = self->id;
}

int main(int argc, char **argv)
{
    printf("clusterd: starting cluster management daemon\n");

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    srand(time(NULL) ^ getpid());
    g_cluster.running = 1;
    pthread_mutex_init(&g_cluster.state_lock, NULL);
    pthread_mutex_init(&g_cluster.log_lock, NULL);
    g_cluster.log_capacity = 64;
    g_cluster.log = malloc(g_cluster.log_capacity * sizeof(*g_cluster.log));
    if (!g_cluster.log) {
        printf("clusterd: out of memory for log buffer\n");
        return 1;
    }
    g_cluster.nl_sock = netlink_sock_open();

    load_config();

    /* Start threads */
    pthread_t gossip_tid, raft_tid, api_tid, hb_tid;

    if (argc > 1 && strcmp(argv[1], "--no-gossip") != 0)
        pthread_create(&gossip_tid, NULL, gossip_thread, NULL);

    if (argc > 1 && strcmp(argv[1], "--no-raft") != 0)
        pthread_create(&raft_tid, NULL, raft_thread, NULL);

    pthread_create(&api_tid, NULL, api_thread, NULL);
    pthread_create(&hb_tid, NULL, heartbeat_thread, NULL);

    printf("clusterd: running (addr=%s, api=:%d)\n",
           g_cluster.local_addr, API_PORT);

    /* Wait for termination */
    pthread_join(hb_tid, NULL);
    g_cluster.running = 0;

    pthread_join(api_tid, NULL);
    if (argc > 1 && strcmp(argv[1], "--no-raft") != 0)
        pthread_join(raft_tid, NULL);
    if (argc > 1 && strcmp(argv[1], "--no-gossip") != 0)
        pthread_join(gossip_tid, NULL);

    if (g_cluster.nl_sock >= 0) close(g_cluster.nl_sock);

    printf("clusterd: stopped\n");
    return 0;
}
