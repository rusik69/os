/* mrp.c — Multiple Registration Protocol (802.1ak), MVRP/MMRP */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "net.h"
#include "net_internal.h"
#include "errno.h"

/* MRP protocol definitions (802.1ak-2007) */
#define MRP_PROTOCOL_ID     0x0001   /* MRP protocol identifier */
#define MRP_ETHER_TYPE      0x88F5   /* MRP 802.1ak Ethertype (proposed) */
                                     /* Actually MVRP uses 0x88F5 for MRP */
#define MRP_VERSION         0x00

/* MRP PDU header */
struct mrp_pdu_header {
    uint16_t protocol_id;    /* 0x0001 for MRP */
    uint8_t  version;        /* 0x00 */
    /* Followed by MRP messages */
} __attribute__((packed));

/* MRP message format */
struct mrp_message {
    uint8_t  vector_header;  /* AttributeType (3 bits vector) + Length (5 bits) or FirstValue */
} __attribute__((packed));

/* MRP event types (encoded in vector) */
#define MRP_EVENT_JOIN_IN       0   /* 00 = JoinIn */
#define MRP_EVENT_JOIN_MT       1   /* 01 = JoinMt (Join Member) */
#define MRP_EVENT_LV            2   /* 10 = LV (Leave) */
#define MRP_EVENT_NEW           2   /* same encoding? depends on context */

/* MRP timer values (in centiseconds) */
#define MRP_JOIN_TIMER      20      /* 200 ms */
#define MRP_LEAVE_TIMER     60      /* 600 ms */
#define MRP_LEAVEALL_TIMER  1000    /* 10 s */

/* MRP attribute type for MVRP (802.1Q-2011) */
#define MVRP_ATTR_VID       1       /* VLAN ID */

/* MRP attribute type for MMRP (802.1ak) */
#define MMRP_ATTR_MAC       1       /* MAC address */

/* MRP participant per attribute */
struct mrp_participant {
    int      in_use;
    uint16_t attr_type;
    uint32_t attr_value;        /* VID or hashed MAC */
    int      declared;          /* 1 if declared */
    int      received;          /* 1 if received from peer */
};

/* MRP application */
struct mrp_application {
    int      registered;
    uint16_t app_protocol;      /* MVRP or MMRP */
    struct mrp_participant participants[32];
    int      nparticipants;
    uint64_t leaveall_timer;
};

#define MRP_MAX_APPS 2
static struct mrp_application mrp_apps[MRP_MAX_APPS];
static int mrp_initialised = 0;

/* MRP multicast MAC */
static const uint8_t mrp_mac_addr[6] = { 0x01, 0x80, 0xC2, 0x00, 0x00, 0x20 };

void mrp_init(void)
{
    if (mrp_initialised) return;
    memset(mrp_apps, 0, sizeof(mrp_apps));
    mrp_initialised = 1;
    kprintf("[OK] mrp: Multiple Registration Protocol initialised (MVRP/MMRP)\n");
}

/* Register an MRP application */
int mrp_register_application(uint16_t app_protocol)
{
    if (!mrp_initialised) return -ENOSYS;

    for (int i = 0; i < MRP_MAX_APPS; i++) {
        if (!mrp_apps[i].registered) {
            struct mrp_application *app = &mrp_apps[i];
            memset(app, 0, sizeof(*app));
            app->registered = 1;
            app->app_protocol = app_protocol;
            app->leaveall_timer = MRP_LEAVEALL_TIMER;
            kprintf("mrp: registered %s application\n",
                    app_protocol == MVRP_ATTR_VID ? "MVRP" : "MMRP");
            return 0;
        }
    }
    return -ENOSPC;
}

/* Declare an attribute (MVRP/MMRP join) */
int mrp_join(uint16_t app_protocol, uint32_t attr_value)
{
    if (!mrp_initialised) return -ENOSYS;

    for (int i = 0; i < MRP_MAX_APPS; i++) {
        if (!mrp_apps[i].registered || mrp_apps[i].app_protocol != app_protocol)
            continue;

        struct mrp_application *app = &mrp_apps[i];

        /* Check if already declared */
        for (int j = 0; j < app->nparticipants; j++) {
            if (app->participants[j].in_use &&
                app->participants[j].attr_value == attr_value) {
                return 0; /* already joined */
            }
        }

        if (app->nparticipants >= 32) return -ENOSPC;

        struct mrp_participant *p = &app->participants[app->nparticipants++];
        memset(p, 0, sizeof(*p));
        p->in_use = 1;
        p->attr_type = (app->app_protocol == MVRP_ATTR_VID) ? MVRP_ATTR_VID : MMRP_ATTR_MAC;
        p->attr_value = attr_value;
        p->declared = 1;

        kprintf("mrp: join attr=0x%x for %s\n", attr_value,
                app_protocol == MVRP_ATTR_VID ? "MVRP" : "MMRP");
        return 0;
    }
    return -ENOENT;
}

/* Withdraw an attribute (MVRP/MMRP leave) */
int mrp_leave(uint16_t app_protocol, uint32_t attr_value)
{
    if (!mrp_initialised) return -ENOSYS;

    for (int i = 0; i < MRP_MAX_APPS; i++) {
        if (!mrp_apps[i].registered || mrp_apps[i].app_protocol != app_protocol)
            continue;

        struct mrp_application *app = &mrp_apps[i];

        for (int j = 0; j < app->nparticipants; j++) {
            if (app->participants[j].in_use &&
                app->participants[j].attr_value == attr_value) {
                app->participants[j].declared = 0;
                kprintf("mrp: leave attr=0x%x for %s\n", attr_value,
                        app_protocol == MVRP_ATTR_VID ? "MVRP" : "MMRP");
                return 0;
            }
        }
        return -ENOENT;
    }
    return -ENOENT;
}

/* Send an MRP PDU */
int mrp_send_pdu(void)
{
    if (!mrp_initialised) return -ENOSYS;

    uint8_t pdu[128];
    struct mrp_pdu_header *hdr = (struct mrp_pdu_header *)pdu;
    hdr->protocol_id = htons(MRP_PROTOCOL_ID);
    hdr->version = MRP_VERSION;

    /* End mark */
    pdu[3] = 0x00;

    send_eth(mrp_mac_addr, MRP_ETHER_TYPE, pdu, 4);
    return 0;
}

/* Process a received MRP PDU */
int mrp_receive_pdu(const uint8_t *data, uint16_t len)
{
    if (!mrp_initialised || !data || len < 3) return -EINVAL;

    struct mrp_pdu_header *hdr = (struct mrp_pdu_header *)data;
    if (ntohs(hdr->protocol_id) != MRP_PROTOCOL_ID)
        return -EINVAL;

    kprintf("mrp: received PDU len=%d\n", len);
    return 0;
}

/* MRP timer tick */
void mrp_tick(void)
{
    if (!mrp_initialised) return;

    for (int i = 0; i < MRP_MAX_APPS; i++) {
        if (!mrp_apps[i].registered) continue;

        struct mrp_application *app = &mrp_apps[i];
        if (app->leaveall_timer > 0) {
            app->leaveall_timer--;
            if (app->leaveall_timer == 0) {
                kprintf("mrp: leaveall timeout for app_protocol=%u\n",
                        app->app_protocol);
                app->leaveall_timer = MRP_LEAVEALL_TIMER;
            }
        }
    }
}
#include "module.h"
module_init(mrp_init);