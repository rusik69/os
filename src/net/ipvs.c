/* ipvs.c — IP Virtual Server with connection tracking + NAT */

#define KERNEL_INTERNAL
#include "ipvs.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "timer.h"

static struct ipvs_virtual g_virtuals[IPVS_MAX_VIRTUALS];
static int g_num_virtuals = 0;
static int ipvs_initialized = 0;

/* Real server storage */
static struct ipvs_real g_reals[IPVS_MAX_VIRTUALS * IPVS_MAX_REALS];
static int g_num_reals = 0;

/* ══════════════════════════════════════════════════════════════════════
 * Connection tracking hash table
 * ══════════════════════════════════════════════════════════════════════ */

static struct ip_vs_conn *g_conn_hash[IPVS_CONN_HASH_SIZE];
static int g_conn_count = 0;

/* Simple hash from IP:port tuple */
static uint32_t conn_hash_4tuple(uint32_t c_ip, uint16_t c_port,
                                  uint32_t v_ip, uint16_t v_port)
{
    uint32_t h = c_ip ^ v_ip;
    h = h * 31 + c_port;
    h = h * 31 + v_port;
    return h & (IPVS_CONN_HASH_SIZE - 1);
}

struct ip_vs_conn *ip_vs_conn_new(uint32_t c_ip, uint16_t c_port,
                                    uint32_t v_ip, uint16_t v_port,
                                    uint32_t d_ip, uint16_t d_port,
                                    uint8_t protocol)
{
    if (!ipvs_initialized) return NULL;

    /* Check for existing connection (avoid duplicates) */
    struct ip_vs_conn *existing = ip_vs_conn_lookup(c_ip, c_port, v_ip, v_port, protocol);
    if (existing)
        return existing;

    struct ip_vs_conn *conn = (struct ip_vs_conn *)kmalloc(sizeof(struct ip_vs_conn));
    if (!conn) return NULL;

    conn->c_ip = c_ip;
    conn->c_port = c_port;
    conn->v_ip = v_ip;
    conn->v_port = v_port;
    conn->d_ip = d_ip;
    conn->d_port = d_port;
    conn->protocol = protocol;
    conn->state = IP_VS_CONN_ESTAB;
    conn->nat_mode = IPVS_NAT_MASQ;
    conn->orig_src_ip = c_ip;
    conn->orig_src_port = c_port;
    conn->orig_dst_ip = v_ip;
    conn->orig_dst_port = v_port;
    conn->expiry = timer_get_ms() + IPVS_CONN_TIMEOUT_MS;
    conn->next = NULL;

    uint32_t idx = conn_hash_4tuple(c_ip, c_port, v_ip, v_port);
    conn->next = g_conn_hash[idx];
    g_conn_hash[idx] = conn;
    g_conn_count++;

    return conn;
}

struct ip_vs_conn *ip_vs_conn_lookup(uint32_t c_ip, uint16_t c_port,
                                      uint32_t v_ip, uint16_t v_port,
                                      uint8_t protocol)
{
    if (!ipvs_initialized) return NULL;

    uint32_t idx = conn_hash_4tuple(c_ip, c_port, v_ip, v_port);
    struct ip_vs_conn *conn = g_conn_hash[idx];

    while (conn) {
        if (conn->c_ip == c_ip &&
            conn->c_port == c_port &&
            conn->v_ip == v_ip &&
            conn->v_port == v_port &&
            conn->protocol == protocol) {
            /* Refresh expiry on active lookup */
            conn->expiry = timer_get_ms() + IPVS_CONN_TIMEOUT_MS;
            return conn;
        }
        conn = conn->next;
    }
    return NULL;
}

void ip_vs_conn_expire(struct ip_vs_conn *conn)
{
    if (!conn) return;

    uint32_t idx = conn_hash_4tuple(conn->c_ip, conn->c_port,
                                     conn->v_ip, conn->v_port);
    struct ip_vs_conn **pp = &g_conn_hash[idx];
    while (*pp) {
        if (*pp == conn) {
            *pp = conn->next;
            g_conn_count--;
            conn->state = IP_VS_CONN_CLOSE;
            kfree(conn);
            return;
        }
        pp = &(*pp)->next;
    }
}

void ip_vs_conn_cleanup(void)
{
    if (!ipvs_initialized) return;

    uint64_t now = timer_get_ms();
    for (int i = 0; i < IPVS_CONN_HASH_SIZE; i++) {
        struct ip_vs_conn **pp = &g_conn_hash[i];
        while (*pp) {
            if ((*pp)->expiry <= now) {
                struct ip_vs_conn *expired = *pp;
                *pp = expired->next;
                g_conn_count--;
                kfree(expired);
            } else {
                pp = &(*pp)->next;
            }
        }
    }
}

int ip_vs_conn_count(void)
{
    return g_conn_count;
}

/* ══════════════════════════════════════════════════════════════════════
 * NAT translation
 * ══════════════════════════════════════════════════════════════════════ */

int ipvs_nat_in(struct ip_vs_conn *conn, uint32_t *ip, uint16_t *port)
{
    if (!conn || !ip || !port)
        return -1;

    /* Save original destination for reverse translation */
    conn->orig_dst_ip = *ip;
    conn->orig_dst_port = *port;

    /* Rewrite destination to real server */
    *ip = conn->d_ip;
    *port = conn->d_port;

    /* Save original source for SNAT return */
    conn->orig_src_ip = conn->c_ip;
    conn->orig_src_port = conn->c_port;

    return 0;
}

int ipvs_nat_out(struct ip_vs_conn *conn, uint32_t *ip, uint16_t *port)
{
    if (!conn || !ip || !port)
        return -1;

    /* Reverse DNAT: restore virtual service IP as source */
    *ip = conn->v_ip;
    *port = conn->v_port;

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * Existing virtual/real server management (unchanged from original)
 * ══════════════════════════════════════════════════════════════════════ */

int ipvs_init(void) {
    memset(g_virtuals, 0, sizeof(g_virtuals));
    memset(g_reals, 0, sizeof(g_reals));
    memset(g_conn_hash, 0, sizeof(g_conn_hash));
    g_num_virtuals = 0;
    g_num_reals = 0;
    g_conn_count = 0;
    ipvs_initialized = 1;
    kprintf("[OK] IPVS initialized (connection tracking + NAT)\n");
    return 0;
}

int ipvs_add_virtual(uint32_t vip, uint16_t port, uint8_t protocol) {
    if (!ipvs_initialized) return -1;
    if (g_num_virtuals >= IPVS_MAX_VIRTUALS) return -1;

    struct ipvs_virtual *v = &g_virtuals[g_num_virtuals];
    v->vip = vip;
    v->port = port;
    v->protocol = protocol;
    v->active = 1;
    v->num_reals = 0;
    v->rr_next = 0;
    g_num_virtuals++;
    return 0;
}

int ipvs_del_virtual(uint32_t vip, uint16_t port) {
    if (!ipvs_initialized) return -1;
    for (int i = 0; i < g_num_virtuals; i++) {
        if (g_virtuals[i].vip == vip && g_virtuals[i].port == port) {
            g_virtuals[i].active = 0;
            for (int j = i; j < g_num_virtuals - 1; j++)
                g_virtuals[j] = g_virtuals[j + 1];
            g_num_virtuals--;
            return 0;
        }
    }
    return -1;
}

static struct ipvs_virtual *ipvs_find_virtual(uint32_t vip, uint16_t port) {
    for (int i = 0; i < g_num_virtuals; i++) {
        if (g_virtuals[i].vip == vip && g_virtuals[i].port == port && g_virtuals[i].active)
            return &g_virtuals[i];
    }
    return NULL;
}

int ipvs_add_real(uint32_t vip, uint16_t port, uint32_t rip, uint16_t rport, int weight) {
    if (!ipvs_initialized) return -1;
    struct ipvs_virtual *v = ipvs_find_virtual(vip, port);
    if (!v) return -1;
    if (v->num_reals >= IPVS_MAX_REALS) return -1;

    if (g_num_reals >= IPVS_MAX_VIRTUALS * IPVS_MAX_REALS) return -1;

    struct ipvs_real *r = &g_reals[g_num_reals];
    r->rip = rip;
    r->port = rport;
    r->weight = weight > 0 ? weight : 1;
    r->active_conns = 0;

    v->reals[v->num_reals++] = r;
    g_num_reals++;
    return 0;
}

int ipvs_del_real(uint32_t vip, uint16_t port, uint32_t rip, uint16_t rport) {
    if (!ipvs_initialized) return -1;
    struct ipvs_virtual *v = ipvs_find_virtual(vip, port);
    if (!v) return -1;

    for (int i = 0; i < v->num_reals; i++) {
        if (v->reals[i]->rip == rip && v->reals[i]->port == rport) {
            for (int j = i; j < v->num_reals - 1; j++)
                v->reals[j] = v->reals[j + 1];
            v->num_reals--;
            return 0;
        }
    }
    return -1;
}

int ipvs_get_dest(uint32_t vip, uint16_t port, uint32_t *rip_out, uint16_t *rport_out) {
    if (!ipvs_initialized || !rip_out || !rport_out) return -1;

    struct ipvs_virtual *v = ipvs_find_virtual(vip, port);
    if (!v || v->num_reals == 0) return -1;

    /* Round-robin scheduling */
    int idx = v->rr_next % v->num_reals;
    v->rr_next = (v->rr_next + 1) % v->num_reals;

    *rip_out = v->reals[idx]->rip;
    *rport_out = v->reals[idx]->port;
    return 0;
}
