/* garp.c — Generic Attribute Registration Protocol (GARP), GVRP/GMRP variants */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "net.h"
#include "net_internal.h"
#include "errno.h"

/* 802.1D-1998 Clause 12: GARP constants */
#define GARP_PROTOCOL_ID   0x0001   /* GARP protocol identifier */
#define GARP_LEAVE_ALL     0        /* LeaveAll event */
#define GARP_JOIN_EMPTY    1        /* Join in empty state */
#define GARP_JOIN_IN       2        /* Join in member state */
#define GARP_LEAVE_EMPTY   3        /* Leave in empty state */
#define GARP_LEAVE_IN      4        /* Leave in member state */

/* GARP timer values (in centiseconds per 802.1D) */
#define GARP_JOIN_TIMER     20      /* 20 cs = 200 ms */
#define GARP_LEAVE_TIMER    60      /* 60 cs = 600 ms */
#define GARP_LEAVEALL_TIMER 1000    /* 1000 cs = 10 s */

/* GARP PDU format (per 802.1D Clause 12) */
struct garp_pdu_header {
    uint16_t protocol_id;    /* 0x0001 for GARP */
    /* Followed by messages */
} __attribute__((packed));

/* GARP message: type(2 bits) + attr_length(6 bits) + attribute event (one per attribute) */
struct garp_message {
    uint8_t  attr_type_len;  /* upper 2 bits = event type, lower 6 = attribute length */
    /* Followed by attribute values */
} __attribute__((packed));

/* GARP attribute event (encoding of upper 2 bits of attr_type_len) */
#define GARP_EVENT_LEAVEALL     0
#define GARP_EVENT_JOIN_EMPTY   1
#define GARP_EVENT_JOIN_IN      2
#define GARP_EVENT_LEAVE_EMPTY  3
#define GARP_EVENT_LEAVE_IN     4
#define GARP_EVENT_EMPTY        5

/* GVRP attribute types (802.1Q) */
#define GVRP_ATTR_VID       1   /* VLAN ID */

/* GMRP attribute types (802.1D) */
#define GMRP_ATTR_MAC       1   /* Group MAC address */

/* GARP participant state machine states */
enum garp_state {
    GARP_VOID = 0,
    GARP_IN,
    GARP_JOIN,
    GARP_LV,
    GARP_LA,
};

/* GARP participant (per attribute) */
struct garp_participant {
    int      in_use;
    uint16_t attribute_type;     /* GVRP_ATTR_VID or GMRP_ATTR_MAC */
    uint32_t attribute_value;    /* VID or hashed MAC */
    enum garp_state state;
    uint64_t join_timer;
    uint64_t leave_timer;
    int      new_flag;           /* TRUE if newly registered */
};

/* GARP application registration */
struct garp_application {
    uint16_t app_type;           /* GVRP or GMRP */
    uint16_t attribute_type;
    struct garp_participant participants[16];
    int      nparticipants;
    uint64_t leaveall_timer;
};

#define GARP_MAX_APPS 2
static struct garp_application garp_apps[GARP_MAX_APPS];
static int garp_initialised = 0;

/* GVRP and GMRP application type codes */
#define GVRP_APPLICATION 1
#define GMRP_APPLICATION 2

/* GARP multicast MAC addresses */
static const uint8_t garp_mac_addr[6] = { 0x01, 0x80, 0xC2, 0x00, 0x00, 0x21 };

void garp_init(void)
{
    if (garp_initialised) return;
    memset(garp_apps, 0, sizeof(garp_apps));
    garp_initialised = 1;
    kprintf("[OK] garp: GARP/GVRP/GMRP initialised (%d apps max)\n",
            GARP_MAX_APPS);
}

/* Register a GARP application (GVRP or GMRP) */
int garp_register_application(uint16_t app_type)
{
    if (!garp_initialised) return -ENOSYS;

    for (int i = 0; i < GARP_MAX_APPS; i++) {
        if (!garp_apps[i].app_type) {
            struct garp_application *app = &garp_apps[i];
            memset(app, 0, sizeof(*app));
            app->app_type = app_type;
            app->attribute_type = (app_type == GVRP_APPLICATION) ? GVRP_ATTR_VID : GMRP_ATTR_MAC;
            app->leaveall_timer = GARP_LEAVEALL_TIMER;
            kprintf("garp: registered %s application\n",
                    app_type == GVRP_APPLICATION ? "GVRP" : "GMRP");
            return 0;
        }
    }
    return -ENOSPC;
}

/* Add an attribute (join) */
int garp_join(uint16_t app_type, uint32_t attr_value)
{
    if (!garp_initialised) return -ENOSYS;

    for (int i = 0; i < GARP_MAX_APPS; i++) {
        if (garp_apps[i].app_type != app_type) continue;

        struct garp_application *app = &garp_apps[i];

        /* Check if already registered */
        for (int j = 0; j < app->nparticipants; j++) {
            if (app->participants[j].in_use &&
                app->participants[j].attribute_value == attr_value) {
                return 0; /* already joined */
            }
        }

        if (app->nparticipants >= 16) return -ENOSPC;

        struct garp_participant *p = &app->participants[app->nparticipants++];
        memset(p, 0, sizeof(*p));
        p->in_use = 1;
        p->attribute_type = app->attribute_type;
        p->attribute_value = attr_value;
        p->state = GARP_IN;
        p->join_timer = GARP_JOIN_TIMER;
        p->new_flag = 1;

        kprintf("garp: join attr=0x%x for %s\n", attr_value,
                app_type == GVRP_APPLICATION ? "GVRP" : "GMRP");
        return 0;
    }
    return -ENOENT;
}

/* Remove an attribute (leave) */
int garp_leave(uint16_t app_type, uint32_t attr_value)
{
    if (!garp_initialised) return -ENOSYS;

    for (int i = 0; i < GARP_MAX_APPS; i++) {
        if (garp_apps[i].app_type != app_type) continue;

        struct garp_application *app = &garp_apps[i];

        for (int j = 0; j < app->nparticipants; j++) {
            if (app->participants[j].in_use &&
                app->participants[j].attribute_value == attr_value) {
                memset(&app->participants[j], 0, sizeof(app->participants[j]));
                /* Compact the array */
                for (int k = j; k < app->nparticipants - 1; k++) {
                    app->participants[k] = app->participants[k + 1];
                }
                app->nparticipants--;
                kprintf("garp: leave attr=0x%x for %s\n", attr_value,
                        app_type == GVRP_APPLICATION ? "GVRP" : "GMRP");
                return 0;
            }
        }
        return -ENOENT;
    }
    return -ENOENT;
}

/* Send a GARP PDU */
int garp_send_pdu(uint16_t app_type)
{
    (void)app_type;
    if (!garp_initialised) return -ENOSYS;

    /* Build a minimal GARP PDU (simplified — just protocol ID + end marker) */
    uint8_t pdu[64];
    struct garp_pdu_header *hdr = (struct garp_pdu_header *)pdu;
    hdr->protocol_id = htons(GARP_PROTOCOL_ID);

    /* End mark (all zeros) — simplify to a single zero byte */
    pdu[2] = 0;
    pdu[3] = 0;

    /* Send to GARP multicast address */
    send_eth(garp_mac_addr, ETH_TYPE_IP, pdu, 4);
    return 0;
}

/* Process a received GARP PDU */
int garp_receive_pdu(const uint8_t *data, uint16_t len)
{
    if (!garp_initialised || !data || len < 2) return -EINVAL;

    struct garp_pdu_header *hdr = (struct garp_pdu_header *)data;
    if (ntohs(hdr->protocol_id) != GARP_PROTOCOL_ID)
        return -EINVAL;

    /* Simplified parsing — just log reception */
    kprintf("garp: received PDU len=%d\n", len);
    return 0;
}

/* Timer tick for GARP state machines — call periodically */
void garp_tick(void)
{
    if (!garp_initialised) return;

    for (int i = 0; i < GARP_MAX_APPS; i++) {
        if (!garp_apps[i].app_type) continue;

        struct garp_application *app = &garp_apps[i];

        /* LeaveAll timer */
        if (app->leaveall_timer > 0) {
            app->leaveall_timer--;
            if (app->leaveall_timer == 0) {
                kprintf("garp: leaveall timeout for %s\n",
                        app->app_type == GVRP_APPLICATION ? "GVRP" : "GMRP");
                app->leaveall_timer = GARP_LEAVEALL_TIMER;
            }
        }

        /* Per-participant timers */
        for (int j = 0; j < app->nparticipants; j++) {
            struct garp_participant *p = &app->participants[j];
            if (!p->in_use) continue;

            if (p->join_timer > 0) p->join_timer--;
            if (p->leave_timer > 0) p->leave_timer--;
        }
    }
}
#include "module.h"
module_init(garp_init);