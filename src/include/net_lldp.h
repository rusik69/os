#ifndef NET_LLDP_H
#define NET_LLDP_H

#include "types.h"

/* LLDP Ethernet type (IEEE 802.1AB) */
#define ETH_TYPE_LLDP 0x88CC

/* Maximum number of tracked neighbors */
#define LLDP_MAX_NEIGHBORS 16

/* Maximum lengths for LLDP TLV fields */
#define LLDP_CHASSIS_ID_MAX  32
#define LLDP_PORT_ID_MAX     32
#define LLDP_SYSTEM_NAME_MAX 64

/* LLDP neighbor descriptor */
struct lldp_neighbor {
    int      valid;
    char     chassis_id[LLDP_CHASSIS_ID_MAX];
    char     port_id[LLDP_PORT_ID_MAX];
    char     system_name[LLDP_SYSTEM_NAME_MAX];
    uint16_t ttl;                /* time-to-live in seconds */
    uint32_t last_update_tick;   /* system tick when last received */
};

/* Send an LLDP frame on the given interface index.
 * Populates mandatory TLVs (chassis ID, port ID, TTL, system name).
 * Returns 0 on success, negative on error. */
int lldp_send(int ifindex);

/* Process a received LLDP frame.
 * frame: pointer to the raw Ethernet frame payload (starting after Ethernet header).
 * len: length of the frame payload.
 * Returns 0 on success, negative on error. */
int lldp_receive(const uint8_t *frame, uint16_t len);

/* Get the current neighbor table.
 * Returns pointer to the static neighbour array; *count is set to the number
 * of valid entries. */
struct lldp_neighbor *lldp_get_neighbor_table(int *count);

/* Initialise the LLDP subsystem. */
void lldp_init(void);

#endif /* NET_LLDP_H */
