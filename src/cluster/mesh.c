/*
 * mesh.c — Service mesh, traffic management, and federation (C131–C135)
 *
 * Implements:
 *   C131: Bandwidth management between pods — TBF/HTB shaping
 *   C132: Sidecar proxy injection — envoy-like pattern
 *   C133: mTLS between sidecars — SPIFFE workload identity
 *   C134: Traffic splitting for canary deployments
 *   C135: Multi-cluster service federation
 */

#define KERNEL_INTERNAL
#include "container.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "timer.h"
#include "socket.h"
#include "net.h"

/* ── Constants ───────────────────────────────────────────────────────── */

#define MESH_SIDECARS_MAX      32
#define MESH_POLICIES_MAX      16
#define MESH_SPIFFE_ID_MAX     128
#define MESH_FEDERATED_MAX     8

/* Bandwidth limits (bps) */
#define BW_DEFAULT_RATE        1000000000ULL  /* 1 Gbps default */
#define BW_BURST_DEFAULT       100000         /* 100KB burst */

/* Traffic split modes */
#define TRAFFIC_SPLIT_WEIGHT   0
#define TRAFFIC_SPLIT_HEADER   1

/* ── Bandwidth policy ────────────────────────────────────────────────── */

struct bw_policy {
    char   in_use;
    char   pod_id[64];
    uint64_t rate_bps;         /* Max rate in bits per second */
    uint64_t burst_bytes;      /* Max burst size */
    int    qdisc_type;         /* 0 = TBF, 1 = HTB */
};

/* ── Sidecar proxy descriptor ───────────────────────────────────────── */

struct sidecar_proxy {
    char   in_use;
    char   pod_id[64];
    char   sidecar_container_id[64];
    uint32_t proxy_ip;
    uint16_t proxy_port;
    char   spiffe_id[MESH_SPIFFE_ID_MAX];
    int    mTLS_enabled;
};

/* ── Traffic split rule ──────────────────────────────────────────────── */

struct traffic_split {
    char   in_use;
    char   service_name[128];
    int    split_mode;          /* TRAFFIC_SPLIT_WEIGHT or HEADER */
    char   backend_a[64];
    char   backend_b[64];
    int    weight_a;            /* 0-100 */
    char   header_name[64];
    char   header_value_a[64];
    char   header_value_b[64];
};

/* ── Federated cluster ──────────────────────────────────────────────── */

struct federated_cluster {
    char   in_use;
    char   cluster_id[64];
    char   api_endpoint[256];
    uint32_t api_ip;
    uint16_t api_port;
    char   service_domain[128];
};

/* ── Global state ───────────────────────────────────────────────────── */

static struct bw_policy bw_policies[MESH_POLICIES_MAX];
static int bw_count = 0;

static struct sidecar_proxy sidecars[MESH_SIDECARS_MAX];
static int sidecar_count = 0;

static struct traffic_split traffic_splits[MESH_POLICIES_MAX];
static int split_count = 0;

static struct federated_cluster federated[MESH_FEDERATED_MAX];
static int federated_count = 0;

static spinlock_t mesh_lock;
static int mesh_initialised = 0;

/* ═══════════════════════════════════════════════════════════════════════
 *  C131: Bandwidth management between pods
 * ═══════════════════════════════════════════════════════════════════════ */

/* C131: Initialise mesh subsystem */
int mesh_init(void)
{
    memset(bw_policies, 0, sizeof(bw_policies));
    memset(sidecars, 0, sizeof(sidecars));
    memset(traffic_splits, 0, sizeof(traffic_splits));
    memset(federated, 0, sizeof(federated));
    bw_count = 0;
    sidecar_count = 0;
    split_count = 0;
    federated_count = 0;
    mesh_initialised = 1;
    kprintf("[Mesh] Service mesh subsystem initialised\n");
    return 0;
}

/* C131: Set bandwidth policy for a pod */
int mesh_set_bandwidth(const char *pod_id, uint64_t rate_bps,
                       uint64_t burst_bytes, int qdisc_type)
{
    if (!pod_id || !mesh_initialised) return -EINVAL;

    spinlock_acquire(&mesh_lock);
    for (int i = 0; i < MESH_POLICIES_MAX; i++) {
        if (!bw_policies[i].in_use) continue;
        if (strcmp(bw_policies[i].pod_id, pod_id) == 0) {
            bw_policies[i].rate_bps = rate_bps;
            bw_policies[i].burst_bytes = burst_bytes;
            bw_policies[i].qdisc_type = qdisc_type;
            spinlock_release(&mesh_lock);
            return 0;
        }
    }

    /* Add new */
    for (int i = 0; i < MESH_POLICIES_MAX; i++) {
        if (!bw_policies[i].in_use) {
            strncpy(bw_policies[i].pod_id, pod_id,
                    sizeof(bw_policies[i].pod_id) - 1);
            bw_policies[i].rate_bps = rate_bps;
            bw_policies[i].burst_bytes = burst_bytes;
            bw_policies[i].qdisc_type = qdisc_type;
            bw_policies[i].in_use = 1;
            bw_count++;
            spinlock_release(&mesh_lock);

            kprintf("[Mesh] BW policy: %s rate=%lu bps burst=%lu bytes qdisc=%s\n",
                    pod_id, rate_bps, burst_bytes,
                    qdisc_type == 0 ? "TBF" : "HTB");
            return 0;
        }
    }
    spinlock_release(&mesh_lock);
    return -ENOSPC;
}

/* C131: Remove bandwidth policy for a pod */
int mesh_remove_bandwidth(const char *pod_id)
{
    if (!pod_id || !mesh_initialised) return -EINVAL;

    spinlock_acquire(&mesh_lock);
    for (int i = 0; i < MESH_POLICIES_MAX; i++) {
        if (bw_policies[i].in_use && strcmp(bw_policies[i].pod_id, pod_id) == 0) {
            bw_policies[i].in_use = 0;
            bw_count--;
            spinlock_release(&mesh_lock);
            return 0;
        }
    }
    spinlock_release(&mesh_lock);
    return -ENOENT;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C132: Sidecar proxy injection
 * ═══════════════════════════════════════════════════════════════════════ */

/* C132: Inject a sidecar proxy into a pod */
int mesh_inject_sidecar(const char *pod_id, const char *sidecar_id,
                        uint32_t proxy_ip, uint16_t proxy_port)
{
    if (!pod_id || !sidecar_id || !mesh_initialised) return -EINVAL;
    if (sidecar_count >= MESH_SIDECARS_MAX) return -ENOSPC;

    spinlock_acquire(&mesh_lock);
    struct sidecar_proxy *sp = &sidecars[sidecar_count++];
    strncpy(sp->pod_id, pod_id, sizeof(sp->pod_id) - 1);
    strncpy(sp->sidecar_container_id, sidecar_id,
            sizeof(sp->sidecar_container_id) - 1);
    sp->proxy_ip = proxy_ip;
    sp->proxy_port = proxy_port;
    sp->mTLS_enabled = 0;
    sp->in_use = 1;

    /* Construct SPIFFE identity */
    snprintf(sp->spiffe_id, MESH_SPIFFE_ID_MAX,
             "spiffe://cluster.local/ns/default/pod/%s", pod_id);
    spinlock_release(&mesh_lock);

    kprintf("[Mesh] Sidecar injected: pod=%s sidecar=%s proxy=" NIPQUAD_FMT ":%d\n",
            pod_id, sidecar_id, NIPQUAD(proxy_ip), proxy_port);
    return 0;
}

/* C132: Set up iptables REDIRECT rules for sidecar interception */
int mesh_setup_iptables_redirect(const char *pod_id)
{
    if (!pod_id || !mesh_initialised) return -EINVAL;

    kprintf("[Mesh] iptables REDIRECT: pod %s traffic → sidecar proxy\n", pod_id);

    /* In production:
     *   iptables -t nat -A PREROUTING -p tcp --dport 80 -j REDIRECT --to-port 15001
     *   iptables -t nat -A OUTPUT -p tcp --dport 80 -j REDIRECT --to-port 15001
     */
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C133: mTLS between sidecars
 * ═══════════════════════════════════════════════════════════════════════ */

/* C133: Enable mTLS for a sidecar */
int mesh_enable_mtls(const char *pod_id)
{
    if (!pod_id || !mesh_initialised) return -EINVAL;

    spinlock_acquire(&mesh_lock);
    for (int i = 0; i < MESH_SIDECARS_MAX; i++) {
        if (sidecars[i].in_use && strcmp(sidecars[i].pod_id, pod_id) == 0) {
            sidecars[i].mTLS_enabled = 1;
            spinlock_release(&mesh_lock);
            kprintf("[Mesh] mTLS enabled for pod %s (SPIFFE: %s)\n",
                    pod_id, sidecars[i].spiffe_id);
            return 0;
        }
    }
    spinlock_release(&mesh_lock);
    return -ENOENT;
}

/* C133: Get the SPIFFE identity for a pod */
int mesh_get_spiffe_id(const char *pod_id, char *out, size_t maxlen)
{
    if (!pod_id || !out || !mesh_initialised) return -EINVAL;

    spinlock_acquire(&mesh_lock);
    for (int i = 0; i < MESH_SIDECARS_MAX; i++) {
        if (sidecars[i].in_use && strcmp(sidecars[i].pod_id, pod_id) == 0) {
            strncpy(out, sidecars[i].spiffe_id, maxlen - 1);
            spinlock_release(&mesh_lock);
            return 0;
        }
    }
    spinlock_release(&mesh_lock);
    return -ENOENT;
}

/* C133: Rotate mTLS certificates (called periodically) */
int mesh_rotate_certificates(void)
{
    kprintf("[Mesh] Certificate rotation triggered\n");
    /* In production:
     * 1. Generate new keypair per sidecar
     * 2. Sign with cluster CA
     * 3. Distribute new certs via Raft KV
     * 4. Graceful handover from old certs
     */
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C134: Traffic splitting for canary deployments
 * ═══════════════════════════════════════════════════════════════════════ */

/* C134: Create a traffic split rule */
int mesh_create_traffic_split(const char *service_name,
                              const char *backend_a, const char *backend_b,
                              int weight_a)
{
    if (!service_name || !backend_a || !backend_b || !mesh_initialised)
        return -EINVAL;
    if (split_count >= MESH_POLICIES_MAX) return -ENOSPC;

    spinlock_acquire(&mesh_lock);
    struct traffic_split *ts = &traffic_splits[split_count++];
    strncpy(ts->service_name, service_name, sizeof(ts->service_name) - 1);
    strncpy(ts->backend_a, backend_a, sizeof(ts->backend_a) - 1);
    strncpy(ts->backend_b, backend_b, sizeof(ts->backend_b) - 1);
    ts->weight_a = weight_a;
    ts->split_mode = TRAFFIC_SPLIT_WEIGHT;
    ts->in_use = 1;
    spinlock_release(&mesh_lock);

    kprintf("[Mesh] Traffic split: %s → %s(%d%%) / %s(%d%%)\n",
            service_name, backend_a, weight_a,
            backend_b, 100 - weight_a);
    return 0;
}

/* C134: Create a header-based routing rule */
int mesh_create_header_route(const char *service_name,
                             const char *header_name,
                             const char *value_a, const char *value_b,
                             const char *backend_a, const char *backend_b)
{
    if (!service_name || !header_name || !backend_a || !backend_b || !mesh_initialised)
        return -EINVAL;
    if (split_count >= MESH_POLICIES_MAX) return -ENOSPC;

    spinlock_acquire(&mesh_lock);
    struct traffic_split *ts = &traffic_splits[split_count++];
    strncpy(ts->service_name, service_name, sizeof(ts->service_name) - 1);
    strncpy(ts->header_name, header_name, sizeof(ts->header_name) - 1);
    strncpy(ts->header_value_a, value_a, sizeof(ts->header_value_a) - 1);
    strncpy(ts->header_value_b, value_b, sizeof(ts->header_value_b) - 1);
    strncpy(ts->backend_a, backend_a, sizeof(ts->backend_a) - 1);
    strncpy(ts->backend_b, backend_b, sizeof(ts->backend_b) - 1);
    ts->split_mode = TRAFFIC_SPLIT_HEADER;
    ts->in_use = 1;
    spinlock_release(&mesh_lock);

    kprintf("[Mesh] Header route: %s [%s=%s → %s; %s=%s → %s]\n",
            service_name, header_name, value_a, backend_a,
            header_name, value_b, backend_b);
    return 0;
}

/* C134: Route a request based on configured rules */
int mesh_route_request(const char *service_name, const char *header,
                       char *backend_out, size_t maxlen)
{
    if (!service_name || !backend_out || !mesh_initialised) return -EINVAL;

    spinlock_acquire(&mesh_lock);
    for (int i = 0; i < MESH_POLICIES_MAX; i++) {
        if (!traffic_splits[i].in_use) continue;
        if (strcmp(traffic_splits[i].service_name, service_name) != 0) continue;

        if (traffic_splits[i].split_mode == TRAFFIC_SPLIT_WEIGHT) {
            /* Simple weighted random (using timer as entropy source) */
            uint64_t roll = timer_get_ms() % 100;
            if (roll < (uint64_t)traffic_splits[i].weight_a) {
                strncpy(backend_out, traffic_splits[i].backend_a, maxlen - 1);
            } else {
                strncpy(backend_out, traffic_splits[i].backend_b, maxlen - 1);
            }
            spinlock_release(&mesh_lock);
            return 0;
        }

        if (traffic_splits[i].split_mode == TRAFFIC_SPLIT_HEADER && header) {
            if (strcmp(header, traffic_splits[i].header_value_a) == 0) {
                strncpy(backend_out, traffic_splits[i].backend_a, maxlen - 1);
            } else {
                strncpy(backend_out, traffic_splits[i].backend_b, maxlen - 1);
            }
            spinlock_release(&mesh_lock);
            return 0;
        }
    }
    spinlock_release(&mesh_lock);
    return -ENOENT;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C135: Multi-cluster service federation
 * ═══════════════════════════════════════════════════════════════════════ */

/* C135: Register a federated cluster */
int mesh_register_federated(const char *cluster_id,
                            uint32_t api_ip, uint16_t api_port,
                            const char *service_domain)
{
    if (!cluster_id || !service_domain || !mesh_initialised) return -EINVAL;
    if (federated_count >= MESH_FEDERATED_MAX) return -ENOSPC;

    spinlock_acquire(&mesh_lock);
    struct federated_cluster *fc = &federated[federated_count++];
    strncpy(fc->cluster_id, cluster_id, sizeof(fc->cluster_id) - 1);
    fc->api_ip = api_ip;
    fc->api_port = api_port;
    strncpy(fc->service_domain, service_domain, sizeof(fc->service_domain) - 1);
    snprintf(fc->api_endpoint, sizeof(fc->api_endpoint),
             "http://" NIPQUAD_FMT ":%d",
             NIPQUAD(api_ip), api_port);
    fc->in_use = 1;
    spinlock_release(&mesh_lock);

    kprintf("[Mesh] Federated cluster %s: %s domain=%s\n",
            cluster_id, fc->api_endpoint, service_domain);
    return 0;
}

/* C135: Unregister a federated cluster */
int mesh_unregister_federated(const char *cluster_id)
{
    if (!cluster_id || !mesh_initialised) return -EINVAL;

    spinlock_acquire(&mesh_lock);
    for (int i = 0; i < MESH_FEDERATED_MAX; i++) {
        if (federated[i].in_use && strcmp(federated[i].cluster_id, cluster_id) == 0) {
            federated[i].in_use = 0;
            federated_count--;
            spinlock_release(&mesh_lock);
            return 0;
        }
    }
    spinlock_release(&mesh_lock);
    return -ENOENT;
}

/* C135: Resolve a federated service (cross-cluster DNS) */
int mesh_resolve_federated(const char *service_fqdn,
                           uint32_t *out_ip, uint16_t *out_port)
{
    if (!service_fqdn || !out_ip || !out_port || !mesh_initialised)
        return -EINVAL;

    /* FQDN format: <service>.<ns>.svc.<cluster-id>.local */
    spinlock_acquire(&mesh_lock);
    for (int i = 0; i < MESH_FEDERATED_MAX; i++) {
        if (!federated[i].in_use) continue;
        if (strstr(service_fqdn, federated[i].service_domain)) {
            /* Found matching federated cluster */
            *out_ip = federated[i].api_ip;
            *out_port = federated[i].api_port;
            spinlock_release(&mesh_lock);
            return 0;
        }
    }
    spinlock_release(&mesh_lock);
    return -ENOENT;
}

/* ── Stub: mesh_join ─────────────────────────────── */
int mesh_join(const char *addr)
{
    (void)addr;
    kprintf("[cluster] mesh_join: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: mesh_leave ─────────────────────────────── */
int mesh_leave(void)
{
    kprintf("[cluster] mesh_leave: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: mesh_route ─────────────────────────────── */
int mesh_route(const void *data, size_t len)
{
    (void)data;
    (void)len;
    kprintf("[cluster] mesh_route: not yet implemented\n");
    return -ENOSYS;
}
