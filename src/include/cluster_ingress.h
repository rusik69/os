#ifndef CLUSTER_INGRESS_H
#define CLUSTER_INGRESS_H

#include "types.h"

/*
 * Cluster ingress — service exposure and HTTP ingress routing.
 *
 * Supports three modes:
 *   NodePort      — expose a service on a host port (30000–32767)
 *   LoadBalancer  — assign an external IP from a configurable pool
 *   HTTP Ingress  — route HTTP requests based on hostname + path
 */

/* ── Constants ──────────────────────────────────────────────────────── */

#define INGRESS_RULES_MAX      64
#define INGRESS_HOSTNAME_MAX   256
#define INGRESS_PATH_MAX       256
#define INGRESS_SERVICE_MAX    128
#define INGRESS_IP_POOL_MAX    16
#define NODEPORT_MIN           30000
#define NODEPORT_MAX           32767

/* ── Ingress rule ───────────────────────────────────────────────────── */

struct ingress_rule {
    char     in_use;
    char     hostname[INGRESS_HOSTNAME_MAX];
    char     path[INGRESS_PATH_MAX];
    char     service[INGRESS_SERVICE_MAX];
    uint16_t service_port;
    uint16_t node_port;       /* NodePort: 0 if not assigned */
    uint32_t external_ip;     /* LoadBalancer: 0 if not assigned */
};

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * ingress_init() — Initialise the ingress subsystem.
 * @pool_start: First IP in the external-address pool (network order).
 * @pool_end:   Last IP in the external-address pool (network order).
 * Returns 0 on success.
 */
int ingress_init(uint32_t pool_start, uint32_t pool_end);

/**
 * ingress_add_rule() — Register an ingress routing rule.
 * @hostname:    Hostname to match (e.g. "myapp.example.com").
 * @path:        URL path prefix (e.g. "/api").
 * @service:     Target service name.
 * @service_port: Target service port.
 * @mode:        0=NodePort, 1=LoadBalancer, 2=HTTP.
 * Returns the assigned port (NodePort) or external IP (LoadBalancer)
 * on success, negative on error.
 */
int ingress_add_rule(const char *hostname, const char *path,
                     const char *service, uint16_t service_port,
                     int mode);

/**
 * ingress_remove_rule() — Remove an ingress rule by hostname + path.
 */
int ingress_remove_rule(const char *hostname, const char *path);

/**
 * ingress_handle_request() — Route an incoming request.
 * @hostname:    Request Host header.
 * @path:        Request URL path.
 * @service_out: Output buffer for matched service name.
 * @port_out:    Output for the target port.
 * Returns 0 on match, -ENOENT if no rule matches.
 */
int ingress_handle_request(const char *hostname, const char *path,
                           char *service_out, uint16_t *port_out);

/**
 * ingress_get_nodeport() — Allocate a NodePort from the range.
 * Returns the port number on success, negative on error.
 */
int ingress_get_nodeport(void);

/**
 * ingress_release_nodeport() — Release a previously-allocated NodePort.
 */
void ingress_release_nodeport(uint16_t port);

#endif /* CLUSTER_INGRESS_H */
