#ifndef STP_H
#define STP_H

#include "types.h"

/* ── 802.1D-1998 Spanning Tree Protocol ─────────────────────────────
 *
 * Prevents bridge loops by ensuring a loop-free logical topology.
 * One bridge is elected as the Root Bridge.  Each port is assigned a
 * role (Root Port, Designated Port, Alternate Port) and transitions
 * through states (Blocking → Listening → Learning → Forwarding).
 *
 * BPDU constants ──────────────────────────────────────────────────── */

#define STP_PROTOCOL_ID       0x0000
#define STP_PROTOCOL_VERSION  0x00
#define STP_BPDU_TYPE_CONFIG  0x00
#define STP_BPDU_TYPE_TCN     0x80

/* Flags in the Config BPDU flags byte */
#define STP_FLAG_TC           (1U << 1)   /* Topology Change */
#define STP_FLAG_TCA          (1U << 0)   /* Topology Change Acknowledgment */

/* STP port states (802.1D-1998, 8.4) */
#define STP_PORT_DISABLED     0
#define STP_PORT_BLOCKING     1
#define STP_PORT_LISTENING    2
#define STP_PORT_LEARNING     3
#define STP_PORT_FORWARDING   4

/* Port roles */
#define STP_ROLE_UNKNOWN      0
#define STP_ROLE_ROOT_PORT    1
#define STP_ROLE_DESIGNATED   2
#define STP_ROLE_ALTERNATE    3

/* Default STP timer values (seconds) */
#define STP_HELLO_TIME_DEF    2
#define STP_MAX_AGE_DEF       20
#define STP_FORWARD_DELAY_DEF 15
#define STP_BRIDGE_PRIORITY_DEF 32768

/* Maximum number of ports the STP can manage per bridge */
#define STP_MAX_PORTS         8

/* ── BPDU format (Configuration BPDU, 802.1D clause 9.3) ────────────
 * Total size: 35 bytes (excluding 802.3 header) */

struct stp_bpdu_config {
    uint16_t protocol_id;        /* 0x0000 */
    uint8_t  protocol_version;   /* 0x00 */
    uint8_t  bpdu_type;          /* 0x00 = Config BPDU */
    uint8_t  flags;              /* TC, TCA bits */
    uint64_t root_id;            /* priority(2) + MAC(6) */
    uint32_t root_path_cost;     /* cumulative path cost to root */
    uint64_t bridge_id;          /* sender bridge ID */
    uint16_t port_id;            /* sender port ID (priority + number) */
    uint16_t message_age;        /* in units of 1/256 second */
    uint16_t max_age;            /* in units of 1/256 second */
    uint16_t hello_time;         /* in units of 1/256 second */
    uint16_t forward_delay;      /* in units of 1/256 second */
} __attribute__((packed));

/* TCN BPDU — only 4 bytes: protocol_id(2) + version(1) + type(1) = 0x80 */
struct stp_bpdu_tcn {
    uint16_t protocol_id;        /* 0x0000 */
    uint8_t  protocol_version;   /* 0x00 */
    uint8_t  bpdu_type;          /* 0x80 = TCN BPDU */
} __attribute__((packed));

/* ── Per-port STP state ──────────────────────────────────────────── */

struct stp_port {
    int             state;           /* STP_PORT_* */
    int             role;            /* STP_ROLE_* */
    uint16_t        port_id;         /* port priority (high byte) + number (low) */
    uint32_t        path_cost;       /* cost of this port */
    uint32_t        designated_cost; /* cost of the designated bridge on this port */
    uint64_t        designated_bridge;
    uint16_t        designated_port;
    int             enabled;         /* admin state */
    /* Timers (in timer ticks) */
    uint64_t        hello_timer;     /* when to send next hello BPDU */
    uint64_t        delay_timer;     /* state transition timer */
    int             fd_while;        /* forward-delay while count */
};

/* ── Bridge STP state ────────────────────────────────────────────── */

struct stp_bridge {
    uint64_t        bridge_id;       /* own bridge ID: priority(2) + MAC(6) */
    uint64_t        root_id;         /* root bridge ID */
    uint64_t        designated_root; /* same as root_id (for speed) */
    uint32_t        root_path_cost;  /* cost of path to root */
    int             root_port;       /* port index toward root (-1 = we are root) */
    int             topology_change; /* TC flag pending */
    uint64_t        tc_timer;        /* topology change timer */
    uint64_t        hello_timer;     /* periodic hello schedule */
    /* Timer values (in ticks at TIMER_FREQ) */
    uint64_t        hello_time_ticks;
    uint64_t        max_age_ticks;
    uint64_t        forward_delay_ticks;
    uint64_t        max_age;         /* stored in 1/256s units for BPDU */
    uint64_t        hello_time;      /* stored in 1/256s units for BPDU */
    uint64_t        forward_delay;   /* stored in 1/256s units for BPDU */
    /* Ports */
    struct stp_port ports[STP_MAX_PORTS];
    int             num_ports;
    int             initialized;
    uint64_t        last_hello_tick; /* last tick we sent a hello */
};

/* ── API ─────────────────────────────────────────────────────────── */

/* Initialise STP for a bridge.  bridge_mac is 6 bytes used to form the
 * bridge ID.  bridge_id_priority is the 2-byte priority (default 32768). */
void stp_init(uint8_t *bridge_mac, int bridge_id_priority);

/* Add a port to the STP tree.  port_num is a logical index (0-based).
 * port_priority (high 4 bits of port_id) defaults to 128 if 0.
 * path_cost is the administrative cost (0 = auto from speed). */
void stp_add_port(int port_num, int port_priority, uint32_t path_cost);

/* Remove a port from STP management. */
void stp_remove_port(int port_num);

/* Process an incoming BPDU received on the given port.
 * Called from bridge_handle() when a frame's destination MAC is
 * 01:80:C2:00:00:00 (the STP multicast address). */
void stp_receive_bpdu(int port_num, const uint8_t *frame, int len);

/* Periodic tick — should be called once per timer tick (TIMER_FREQ).
 * Drives timers, state transitions, and BPDU generation. */
void stp_tick(void);

/* Send a Configuration BPDU on a specific port. */
void stp_send_config(int port_num);

/* Send a Topology Change Notification BPDU. */
void stp_send_tcn(void);

/* Query the STP state of a port.  Returns STP_PORT_* or STP_PORT_DISABLED. */
int  stp_port_state(int port_num);

/* Query whether a port is allowed to forward user traffic
 * (i.e., it is in the Forwarding state). */
static inline int stp_port_is_forwarding(int port_num) {
    return stp_port_state(port_num) == STP_PORT_FORWARDING;
}

/* Query whether a port is allowed to learn MAC addresses
 * (Forwarding or Learning state). */
static inline int stp_port_is_learning(int port_num) {
    int s = stp_port_state(port_num);
    return (s == STP_PORT_FORWARDING || s == STP_PORT_LEARNING);
}

#endif /* STP_H */
