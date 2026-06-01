/* ipvs.c — IP Virtual Server */

#define KERNEL_INTERNAL
#include "ipvs.h"
#include "printf.h"
#include "string.h"

static struct ipvs_virtual g_virtuals[IPVS_MAX_VIRTUALS];
static int g_num_virtuals = 0;
static int ipvs_initialized = 0;

/* Real server storage */
static struct ipvs_real g_reals[IPVS_MAX_VIRTUALS * IPVS_MAX_REALS];
static int g_num_reals = 0;

int ipvs_init(void) {
    memset(g_virtuals, 0, sizeof(g_virtuals));
    memset(g_reals, 0, sizeof(g_reals));
    g_num_virtuals = 0;
    g_num_reals = 0;
    ipvs_initialized = 1;
    kprintf("[OK] IPVS initialized\\n");
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
