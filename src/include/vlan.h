#ifndef VLAN_H
#define VLAN_H

#include "types.h"

/* 802.1Q header */
struct vlan_header {
    uint16_t tpid;      /* 0x8100 */
    uint16_t tci;       /* PCP+DEI+VID (12-bit VID) */
} __attribute__((packed));

#define VLAN_TPID     0x8100
#define VLAN_VID_MASK 0x0FFF
#define VLAN_MAX_VID  4096

/* Default VLAN ID */
#define VLAN_DEFAULT_VID 1

/* ── API ────────────────────────────────────────────────────────── */

void vlan_init(void);
int  vlan_add_vid(uint16_t vid);
int  vlan_remove_vid(uint16_t vid);
int  vlan_has_vid(uint16_t vid);
uint16_t vlan_parse_tci(const uint8_t *frame, int len);
int  vlan_tag_frame(uint8_t *frame, int len, uint16_t vid);

#endif /* VLAN_H */
