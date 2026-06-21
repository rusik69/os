/* lacp.c — Link Aggregation Control Protocol (802.3ad), LACP PDU exchange */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "net.h"
#include "net_internal.h"
#include "errno.h"

/* 802.3ad LACP constants */
#define LACP_VERSION        1
#define LACP_ETHER_TYPE     0x8809   /* Slow Protocols */
#define LACP_SUBTYPE        0x01     /* LACP subtype in slow protocol header */

/* LACP PDU formats per 802.3ax (formerly 802.3ad) */

/* Actor/Partner information fields */
struct lacp_port_params {
    uint8_t  system_priority[2];
    uint8_t  system_mac[6];
    uint8_t  key[2];
    uint8_t  port_priority[2];
    uint8_t  port_number[2];
    uint8_t  state_flags;    /* LACP state */
    uint8_t  reserved[3];
} __attribute__((packed));

/* LACP PDU (without slow protocol header) */
struct lacp_pdu {
    uint8_t  subtype;                 /* 0x01 = LACP */
    uint8_t  version;                 /* 0x01 */
    uint8_t  tlv_actor_type;          /* 0x01 = Actor Information */
    uint8_t  tlv_actor_len;           /* 0x14 = 20 bytes */
    struct lacp_port_params actor;
    uint8_t  tlv_partner_type;        /* 0x02 = Partner Information */
    uint8_t  tlv_partner_len;         /* 0x14 */
    struct lacp_port_params partner;
    uint8_t  tlv_collector_type;      /* 0x03 = Collector Information */
    uint8_t  tlv_collector_len;       /* 0x10 = 16 bytes */
    uint16_t collector_max_delay;
    uint8_t  collector_reserved[12];
    uint8_t  tlv_terminator;          /* 0x00 */
    uint8_t  terminator_len;          /* 0x00 */
    uint8_t  reserved_data[50];       /* padding to match minimum length */
} __attribute__((packed));

/* LACP state flags (actor/partner state_flags) */
#define LACP_STATE_ACTIVE      0x01   /* Active LACP */
#define LACP_STATE_SHORT_TIMER 0x02   /* Short timeout (3x) */
#define LACP_STATE_AGGREGATING 0x04   /* Capable of aggregation */
#define LACP_STATE_SYNC        0x08   /* In sync */
#define LACP_STATE_COLLECTING  0x10   /* Collecting */
#define LACP_STATE_DISTRIBUTING 0x20  /* Distributing */
#define LACP_STATE_DEFAULTED   0x40   /* Using default partner info */
#define LACP_STATE_EXPIRED     0x80   /* Timer expired */

/* TLV types in LACP PDU */
#define LACP_TLV_ACTOR_INFO     0x01
#define LACP_TLV_PARTNER_INFO   0x02
#define LACP_TLV_COLLECTOR_INFO 0x03
#define LACP_TLV_TERMINATOR     0x00

/* LACP periodic machine states */
enum lacp_periodic_state {
    LACP_PERIODIC_NO_TX = 0,    /* not transmitting */
    LACP_PERIODIC_FAST,         /* fast periodic (1 s) */
    LACP_PERIODIC_SLOW,         /* slow periodic (30 s) */
};

/* Port aggregation state */
struct lacp_port {
    int      in_use;
    uint16_t port_number;
    struct lacp_port_params actor;
    struct lacp_port_params partner;
    uint8_t  state_flags;       /* actor state */
    enum lacp_periodic_state periodic_state;
    int      aggregated;        /* 1 if part of LAG */
    uint32_t partner_system_prio;
    uint64_t partner_system;
    uint16_t partner_key;
    uint16_t partner_port;
    uint16_t partner_port_prio;
    /* Timer values in ticks */
    uint64_t current_while_timer;
    uint64_t periodic_timer;
    uint64_t wait_while_timer;
};

#define LACP_MAX_PORTS 8
static struct lacp_port lacp_ports[LACP_MAX_PORTS];
static int lacp_enabled = 0;

/* LACP multicast MAC (01:80:C2:00:00:02) */
static const uint8_t lacp_dmac[6] = { 0x01, 0x80, 0xC2, 0x00, 0x00, 0x02 };

void lacp_init(void)
{
    if (lacp_enabled) return;
    memset(lacp_ports, 0, sizeof(lacp_ports));
    lacp_enabled = 1;
    kprintf("[OK] lacp: Link Aggregation Control Protocol initialised (%d ports max)\n",
            LACP_MAX_PORTS);
}

/* Add a port to LACP control */
int lacp_add_port(uint16_t port_number, const uint8_t *mac)
{
    if (!lacp_enabled) return -ENOSYS;

    for (int i = 0; i < LACP_MAX_PORTS; i++) {
        if (!lacp_ports[i].in_use) {
            struct lacp_port *p = &lacp_ports[i];
            memset(p, 0, sizeof(*p));
            p->in_use = 1;
            p->port_number = port_number;

            /* Initialise actor parameters with system MAC */
            p->actor.system_priority[0] = 0x80;  /* default priority 0x8000 */
            p->actor.system_priority[1] = 0x00;
            memcpy(p->actor.system_mac, mac, 6);
            p->actor.key[0] = 0x00;
            p->actor.key[1] = 0x01;
            p->actor.port_priority[0] = 0x80;
            p->actor.port_priority[1] = 0x00;
            p->actor.port_number[0] = (port_number >> 8) & 0xFF;
            p->actor.port_number[1] = port_number & 0xFF;
            p->actor.state_flags = LACP_STATE_ACTIVE | LACP_STATE_AGGREGATING;

            p->state_flags = p->actor.state_flags;
            p->periodic_state = LACP_PERIODIC_SLOW;
            p->periodic_timer = 30;  /* slow = 30 ticks */

            kprintf("lacp: port %u added\n", port_number);
            return 0;
        }
    }
    return -ENOSPC;
}

/* Remove a port from LACP control */
int lacp_remove_port(uint16_t port_number)
{
    for (int i = 0; i < LACP_MAX_PORTS; i++) {
        if (lacp_ports[i].in_use && lacp_ports[i].port_number == port_number) {
            memset(&lacp_ports[i], 0, sizeof(lacp_ports[i]));
            kprintf("lacp: port %u removed\n", port_number);
            return 0;
        }
    }
    return -ENOENT;
}

/* Build and send an LACP PDU */
int lacp_send_pdu(uint16_t port_number)
{
    if (!lacp_enabled) return -ENOSYS;

    /* Find the port */
    int idx = -1;
    for (int i = 0; i < LACP_MAX_PORTS; i++) {
        if (lacp_ports[i].in_use && lacp_ports[i].port_number == port_number) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -ENOENT;

    struct lacp_port *p = &lacp_ports[idx];
    struct lacp_pdu pdu;
    memset(&pdu, 0, sizeof(pdu));

    pdu.subtype = LACP_SUBTYPE;
    pdu.version = LACP_VERSION;

    /* Actor information */
    pdu.tlv_actor_type = LACP_TLV_ACTOR_INFO;
    pdu.tlv_actor_len  = 20;
    pdu.actor = p->actor;

    /* Partner information (use our defaults if not learned) */
    pdu.tlv_partner_type = LACP_TLV_PARTNER_INFO;
    pdu.tlv_partner_len  = 20;
    pdu.partner = p->partner;

    /* Collector information */
    pdu.tlv_collector_type = LACP_TLV_COLLECTOR_INFO;
    pdu.tlv_collector_len  = 16;
    pdu.collector_max_delay = htons(0);

    /* Terminator */
    pdu.tlv_terminator = LACP_TLV_TERMINATOR;
    pdu.terminator_len = 0;

    /* Send via Ethernet slow protocol */
    send_eth(lacp_dmac, LACP_ETHER_TYPE, &pdu, sizeof(pdu));
    kprintf("lacp: sent PDU on port %u\n", port_number);
    return 0;
}

/* Process a received LACP PDU */
int lacp_receive_pdu(uint16_t port_number, const struct lacp_pdu *pdu, uint16_t len)
{
    if (!lacp_enabled || !pdu || len < sizeof(struct lacp_pdu))
        return -EINVAL;

    if (pdu->subtype != LACP_SUBTYPE || pdu->version != LACP_VERSION)
        return -EINVAL;

    /* Find our port */
    int idx = -1;
    for (int i = 0; i < LACP_MAX_PORTS; i++) {
        if (lacp_ports[i].in_use && lacp_ports[i].port_number == port_number) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -ENOENT;

    struct lacp_port *p = &lacp_ports[idx];

    /* Update partner information from received actor info */
    p->partner = pdu->actor;
    p->partner_system_prio = (pdu->actor.system_priority[0] << 8) |
                              pdu->actor.system_priority[1];
    memcpy(&p->partner_system, pdu->actor.system_mac, 6);
    p->partner_key = (pdu->actor.key[0] << 8) | pdu->actor.key[1];
    p->partner_port = (pdu->actor.port_number[0] << 8) | pdu->actor.port_number[1];
    p->partner_port_prio = (pdu->actor.port_priority[0] << 8) | pdu->actor.port_priority[1];

    /* Actor/Partner state tracking per 802.3ad */
    uint8_t rx_state = pdu->actor.state_flags;

    /* Update our actor state based on received partner state */
    if (rx_state & LACP_STATE_SYNC) {
        /* Partner is in sync — we can start collecting/distributing */
        p->actor.state_flags |= LACP_STATE_SYNC;
        p->actor.state_flags |= LACP_STATE_COLLECTING;
        p->actor.state_flags |= LACP_STATE_DISTRIBUTING;
        p->aggregated = 1;
    } else {
        p->actor.state_flags &= ~LACP_STATE_SYNC;
        p->actor.state_flags &= ~LACP_STATE_COLLECTING;
        p->actor.state_flags &= ~LACP_STATE_DISTRIBUTING;
        p->aggregated = 0;
    }

    /* Track timeout mode */
    if (rx_state & LACP_STATE_SHORT_TIMER) {
        p->periodic_state = LACP_PERIODIC_FAST;
        p->periodic_timer = 1;  /* 1 second */
        p->current_while_timer = 3;  /* Short timeout: 3 seconds */
    } else {
        p->periodic_state = LACP_PERIODIC_SLOW;
        p->periodic_timer = 30; /* 30 seconds */
        p->current_while_timer = 90; /* Long timeout: 90 seconds */
    }

    /* Clear defaulted/expired flags since we received a valid PDU */
    p->actor.state_flags &= ~LACP_STATE_DEFAULTED;
    p->actor.state_flags &= ~LACP_STATE_EXPIRED;

    kprintf("lacp: received PDU on port %u, partner system=%02x:%02x:%02x:%02x:%02x:%02x "
            "state=0x%02x aggregated=%d\n",
            port_number,
            pdu->actor.system_mac[0], pdu->actor.system_mac[1],
            pdu->actor.system_mac[2], pdu->actor.system_mac[3],
            pdu->actor.system_mac[4], pdu->actor.system_mac[5],
            rx_state, p->aggregated);

    return 0;
}

/* Periodic LACP tick — call from timer */
void lacp_tick(void)
{
    if (!lacp_enabled) return;

    for (int i = 0; i < LACP_MAX_PORTS; i++) {
        if (!lacp_ports[i].in_use) continue;

        struct lacp_port *p = &lacp_ports[i];

        /* Decrement periodic timer */
        if (p->periodic_timer > 0) {
            p->periodic_timer--;
            if (p->periodic_timer == 0) {
                /* Send LACP PDU periodically */
                lacp_send_pdu(p->port_number);
                p->periodic_timer = (p->periodic_state == LACP_PERIODIC_FAST) ? 1 : 30;
            }
        }
    }
}
#include "module.h"
module_init(lacp_init);
/* ── Stub: lacp_send ─────────────────────────────── */
int lacp_send(void *dev, const void *pdu, size_t len)
{
    (void)dev;
    (void)pdu;
    (void)len;
    kprintf("[lacp] lacp_send: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: lacp_recv ─────────────────────────────── */
int lacp_recv(void *dev, const void *pdu, size_t len)
{
    (void)dev;
    (void)pdu;
    (void)len;
    kprintf("[lacp] lacp_recv: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: lacp_update_state ─────────────────────────────── */
int lacp_update_state(void *port, int state)
{
    (void)port;
    (void)state;
    kprintf("[lacp] lacp_update_state: not yet implemented\n");
    return -ENOSYS;
}
