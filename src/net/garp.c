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

/* GARP participant state machine states per 802.1D-1998 Clause 12 */
enum garp_state {
    GARP_VOID = 0,
    GARP_IN,       /* Member (IN) */
    GARP_JOIN,     /* Join Pending (JP) */
    GARP_LV,       /* Leave Pending (LP) */
    GARP_LA,       /* Leave All (LA) */
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
    /* Per-802.1D state machine counters */
    int      join_empty_count;   /* Number of JoinEmpty transmissions */
    int      join_in_count;      /* Number of JoinIn transmissions */
    int      leave_count;        /* Number of Leave transmissions */
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
                /* Already registered — send JoinIn if in IN state (802.1D 12.8.2) */
                struct garp_participant *p = &app->participants[j];
                if (p->state == GARP_IN) {
                    p->state = GARP_JOIN;
                    p->join_timer = GARP_JOIN_TIMER;
                    p->new_flag = 0;
                }
                return 0;
            }
        }

        if (app->nparticipants >= 16) return -ENOSPC;

        struct garp_participant *p = &app->participants[app->nparticipants++];
        memset(p, 0, sizeof(*p));
        p->in_use = 1;
        p->attribute_type = app->attribute_type;
        p->attribute_value = attr_value;
        /* Per 802.1D: new declarations start in VOID, then receive JoinIn event */
        p->state = GARP_JOIN;  /* JoinIn event transitions VOID->JP (Join Pending) */
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
                struct garp_participant *p = &app->participants[j];
                /* 802.1D Leave event transitions to LP (Leave Pending) */
                if (p->state == GARP_IN || p->state == GARP_JOIN) {
                    p->state = GARP_LV;
                    p->leave_timer = GARP_LEAVE_TIMER;
                } else if (p->state == GARP_LV) {
                    /* Already leaving — do nothing */
                } else {
                    /* VOID or LA — just remove */
                    memset(p, 0, sizeof(*p));
                    /* Compact */
                    for (int k = j; k < app->nparticipants - 1; k++) {
                        app->participants[k] = app->participants[k + 1];
                    }
                    app->nparticipants--;
                }
                kprintf("garp: leave attr=0x%x for %s\n", attr_value,
                        app_type == GVRP_APPLICATION ? "GVRP" : "GMRP");
                return 0;
            }
        }
        return -ENOENT;
    }
    return -ENOENT;
}

/* Build a GARP PDU with proper message encoding per 802.1D Clause 12 */
static int garp_build_pdu(uint8_t *pdu, int pdu_max, struct garp_application *app)
{
    struct garp_pdu_header *hdr = (struct garp_pdu_header *)pdu;
    hdr->protocol_id = htons(GARP_PROTOCOL_ID);
    int offset = 2;

    /* Encode each participant's attribute into the PDU */
    for (int i = 0; i < app->nparticipants; i++) {
        struct garp_participant *p = &app->participants[i];
        if (!p->in_use) continue;

        /* Determine event type based on state machine */
        uint8_t event;
        switch (p->state) {
        case GARP_JOIN:
            event = p->new_flag ? GARP_EVENT_JOIN_EMPTY : GARP_EVENT_JOIN_IN;
            break;
        case GARP_IN:
            event = GARP_EVENT_JOIN_IN;
            break;
        case GARP_LV:
            event = GARP_EVENT_LEAVE_IN;
            break;
        default:
            continue;
        }

        /* Attribute type + length byte: upper 2 bits = event, lower 6 = attr len */
        if (offset + 1 + 4 > pdu_max) break; /* at least 1 byte header + 4 bytes value */
        uint8_t *msg = &pdu[offset];
        int attr_len = 4; /* All our attributes are 4 bytes */
        msg[0] = (event << 6) | (attr_len & 0x3F);
        /* Attribute value (4 bytes, network byte order) */
        uint32_t val = htonl(p->attribute_value);
        memcpy(&pdu[offset + 1], &val, 4);
        offset += 1 + attr_len;
    }

    /* End mark (all zeros) */
    if (offset + 2 <= pdu_max) {
        pdu[offset] = 0;
        pdu[offset + 1] = 0;
        offset += 2;
    }

    return offset;
}

/* Send a GARP PDU */
int garp_send_pdu(uint16_t app_type)
{
    if (!garp_initialised) return -ENOSYS;

    for (int i = 0; i < GARP_MAX_APPS; i++) {
        if (garp_apps[i].app_type != app_type) continue;

        struct garp_application *app = &garp_apps[i];
        uint8_t pdu[256];
        int len = garp_build_pdu(pdu, sizeof(pdu), app);
        if (len > 2) {
            send_eth(garp_mac_addr, ETH_TYPE_IP, pdu, (uint16_t)len);
        }
        return 0;
    }
    return -ENOENT;
}

/* Process a received GARP PDU with full parsing per 802.1D */
int garp_receive_pdu(const uint8_t *data, uint16_t len)
{
    if (!garp_initialised || !data || len < 2) return -EINVAL;

    struct garp_pdu_header *hdr = (struct garp_pdu_header *)data;
    if (ntohs(hdr->protocol_id) != GARP_PROTOCOL_ID)
        return -EINVAL;

    /* Parse messages */
    int offset = 2;
    while (offset + 1 < len) {
        uint8_t type_len = data[offset];
        uint8_t event = (type_len >> 6) & 0x03;
        uint8_t attr_len = type_len & 0x3F;

        /* End mark */
        if (type_len == 0 && attr_len == 0) break;

        if (offset + 1 + attr_len > len) break;

        uint32_t attr_value = 0;
        if (attr_len >= 4) {
            memcpy(&attr_value, &data[offset + 1], 4);
            attr_value = ntohl(attr_value);
        }

        kprintf("garp: received event=%d attr_len=%d attr=0x%x\n",
                event, attr_len, attr_value);

        /* Process event according to 802.1D participant state machine */
        for (int i = 0; i < GARP_MAX_APPS; i++) {
            if (!garp_apps[i].app_type) continue;
            struct garp_application *app = &garp_apps[i];

            for (int j = 0; j < app->nparticipants; j++) {
                struct garp_participant *p = &app->participants[j];
                if (!p->in_use || p->attribute_value != attr_value) continue;

                switch (event) {
                case GARP_EVENT_JOIN_EMPTY:
                case GARP_EVENT_JOIN_IN:
                    /* Join event: transition to IN state */
                    if (p->state == GARP_VOID || p->state == GARP_LA) {
                        p->state = GARP_IN;
                    } else if (p->state == GARP_LV) {
                        /* Cancel leave */
                        p->state = GARP_IN;
                        p->leave_timer = 0;
                    }
                    break;
                case GARP_EVENT_LEAVE_EMPTY:
                case GARP_EVENT_LEAVE_IN:
                    /* Leave event: transition to VOID */
                    p->state = GARP_VOID;
                    p->join_timer = 0;
                    p->leave_timer = 0;
                    break;
                default:
                    break;
                }
            }
        }

        offset += 1 + attr_len;
    }

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
                /* Send LeaveAll PDU */
                uint8_t pdu[4];
                struct garp_pdu_header *hdr = (struct garp_pdu_header *)pdu;
                hdr->protocol_id = htons(GARP_PROTOCOL_ID);
                pdu[2] = 0x00; /* LeaveAll event with length 0 */
                pdu[3] = 0x00; /* End mark */
                send_eth(garp_mac_addr, ETH_TYPE_IP, pdu, 4);
                app->leaveall_timer = GARP_LEAVEALL_TIMER;
            }
        }

        /* Per-participant state machines */
        for (int j = 0; j < app->nparticipants; j++) {
            struct garp_participant *p = &app->participants[j];
            if (!p->in_use) continue;

            /* Join timer: on expiry, send JoinIn PDU and transition */
            if (p->join_timer > 0) {
                p->join_timer--;
                if (p->join_timer == 0) {
                    /* 802.1D: JP timer expiry -> send JoinIn, go to IN */
                    if (p->state == GARP_JOIN) {
                        p->state = GARP_IN;
                        p->new_flag = 0;
                        /* Optionally send PDU here */
                        garp_send_pdu(app->app_type);
                    }
                }
            }

            /* Leave timer: on expiry, remove attribute */
            if (p->leave_timer > 0) {
                p->leave_timer--;
                if (p->leave_timer == 0) {
                    /* 802.1D: LP timer expiry -> send Leave, go to VOID */
                    if (p->state == GARP_LV) {
                        p->state = GARP_VOID;
                        p->in_use = 0; /* Mark for removal */
                        garp_send_pdu(app->app_type);
                    }
                }
            }
        }

        /* Clean up removed participants */
        int write = 0;
        for (int read = 0; read < app->nparticipants; read++) {
            if (app->participants[read].in_use) {
                if (write != read)
                    app->participants[write] = app->participants[read];
                write++;
            }
        }
        app->nparticipants = write;
    }
}
#include "module.h"
module_init(garp_init);

/* ── garp_join_group: public wrapper around garp_join ── */
int garp_join_group(uint16_t app_type, uint32_t attr_value)
{
    return garp_join(app_type, attr_value);
}
/* ── garp_leave_group: public wrapper around garp_leave ── */
int garp_leave_group(uint16_t app_type, uint32_t attr_value)
{
    return garp_leave(app_type, attr_value);
}
/* ── garp_handle_join: handle received Join PDU for an attribute ── */
int garp_handle_join(uint16_t app_type, uint32_t attr_value)
{
    if (!garp_initialised) return -ENOSYS;
    kprintf("[garp] garp_handle_join: app_type=%u attr=0x%x\n", app_type, attr_value);
    /* Inbound Join: ensure attribute is in our participant table.
     * If not present, add it as an observed remote declaration. */
    for (int i = 0; i < GARP_MAX_APPS; i++) {
        if (garp_apps[i].app_type != app_type) continue;
        struct garp_application *app = &garp_apps[i];
        for (int j = 0; j < app->nparticipants; j++) {
            if (app->participants[j].in_use &&
                app->participants[j].attribute_value == attr_value) {
                /* Already known — transition to IN if leaving */
                if (app->participants[j].state == GARP_LV)
                    app->participants[j].state = GARP_IN;
                return 0;
            }
        }
        /* Not found — add as remote participant */
        if (app->nparticipants < 16) {
            struct garp_participant *p = &app->participants[app->nparticipants++];
            memset(p, 0, sizeof(*p));
            p->in_use = 1;
            p->attribute_type = app->attribute_type;
            p->attribute_value = attr_value;
            p->state = GARP_IN;
        }
        return 0;
    }
    return -ENOENT;
}
/* ── garp_handle_leave: handle received Leave PDU for an attribute ── */
int garp_handle_leave(uint16_t app_type, uint32_t attr_value)
{
    if (!garp_initialised) return -ENOSYS;
    kprintf("[garp] garp_handle_leave: app_type=%u attr=0x%x\n", app_type, attr_value);
    /* Inbound Leave: remove the attribute from participant table */
    for (int i = 0; i < GARP_MAX_APPS; i++) {
        if (garp_apps[i].app_type != app_type) continue;
        struct garp_application *app = &garp_apps[i];
        for (int j = 0; j < app->nparticipants; j++) {
            if (app->participants[j].in_use &&
                app->participants[j].attribute_value == attr_value) {
                memset(&app->participants[j], 0, sizeof(struct garp_participant));
                /* Compact */
                for (int k = j; k < app->nparticipants - 1; k++)
                    app->participants[k] = app->participants[k + 1];
                app->nparticipants--;
                return 0;
            }
        }
    }
    return -ENOENT;
}
/* ── garp_unregister: remove application and all its attributes ── */
int garp_unregister(uint16_t app_type)
{
    if (!garp_initialised) return -ENOSYS;
    for (int i = 0; i < GARP_MAX_APPS; i++) {
        if (garp_apps[i].app_type == app_type) {
            memset(&garp_apps[i], 0, sizeof(struct garp_application));
            kprintf("[garp] garp_unregister: unregistered app_type=%u\n", app_type);
            return 0;
        }
    }
    return -ENOENT;
}

/* ── garp_transmit ────────────────── */
int garp_transmit(void *dev)
{
    (void)dev;
    if (!garp_initialised) return -ENOSYS;
    /* Transmit all registered applications' PDUs */
    for (int i = 0; i < GARP_MAX_APPS; i++) {
        if (garp_apps[i].app_type) {
            garp_send_pdu(garp_apps[i].app_type);
        }
    }
    return 0;
}
