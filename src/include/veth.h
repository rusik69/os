#ifndef VETH_H
#define VETH_H

#include "types.h"

/*
 * veth.h — Virtual Ethernet pair driver
 *
 * Creates paired virtual Ethernet interfaces.  A packet transmitted on
 * one end of the pair is received on the other end.  This is the
 * fundamental building block for connecting network namespaces and
 * container networking.
 *
 * Usage:
 *   int peer[2];
 *   if (veth_create_pair("veth0", "veth1", peer) == 0) {
 *       // peer[0] and peer[1] are the ifindices of the two ends
 *   }
 *
 * Each end is a standard net_device registered with the netif_*
 * interface and can be used with the bridge, routing, etc.
 */

/* Maximum number of veth pairs that can be created */
#define VETH_MAX_PAIRS    4

/* Number of packet slots in each veth endpoint's receive ring */
#define VETH_RING_SIZE    16

/* Maximum Ethernet frame size that veth will carry */
#define VETH_MTU          1500

/* Receive ring slot */
struct veth_ring_slot {
    uint8_t  data[VETH_MTU + 14];  /* Ethernet header + payload */
    uint16_t len;                   /* actual packet length */
    int      occupied;              /* 1 = data valid */
};

/* Per-endpoint private data */
struct veth_endpoint {
    char    name[16];              /* interface name, e.g. "veth0" */
    int     ifindex;               /* our net_device ifindex */
    int     peer_ifindex;          /* the other end of the pair */
    uint8_t mac[6];                /* MAC address */

    /* Receive ring — packets sent by our peer land here */
    struct veth_ring_slot ring[VETH_RING_SIZE];
    int                   ring_head;   /* next slot to read */
    int                   ring_tail;   /* next slot to write */
    int                   ring_count;  /* number of occupied slots */

    int active;                  /* 1 = pair has been created */
};

/* ── API ────────────────────────────────────────────────────────── */

/*
 * Create a veth pair.
 *
 * @name1:   name for the first endpoint (e.g. "veth0")
 * @name2:   name for the second endpoint (e.g. "veth1")
 * @out_ifindex:  if non-NULL, receives array[2] of the assigned ifindices
 *
 * Returns 0 on success, -1 on failure.
 */
int veth_create_pair(const char *name1, const char *name2,
                     int out_ifindex[2]);

/*
 * Destroy a veth pair by either endpoint's ifindex.
 * Both endpoints are unregistered and memory is freed.
 * Returns 0 on success, -1 if the endpoint is not a veth device.
 */
int veth_destroy(int ifindex);

/*
 * Initialise the veth subsystem.  Called once at boot.
 */
void veth_init(void);

#endif /* VETH_H */
