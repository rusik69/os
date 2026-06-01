/* vlan.c — 802.1Q VLAN support */

#define KERNEL_INTERNAL
#include "vlan.h"
#include "net.h"
#include "printf.h"
#include "string.h"

/* VLAN bitmap: which VLANs are active */
static uint16_t vlan_active[VLAN_MAX_VID / 16];
static int vlan_initialized = 0;

void vlan_init(void) {
    memset(vlan_active, 0, sizeof(vlan_active));
    /* Default VLAN 1 is always active */
    vlan_active[VLAN_DEFAULT_VID / 16] |= (1 << (VLAN_DEFAULT_VID % 16));
    vlan_initialized = 1;
    kprintf("[OK] VLAN initialized (default VID 1)\\n");
}

int vlan_add_vid(uint16_t vid) {
    if (!vlan_initialized) return -1;
    if (vid >= VLAN_MAX_VID) return -1;
    vlan_active[vid / 16] |= (1 << (vid % 16));
    return 0;
}

int vlan_remove_vid(uint16_t vid) {
    if (!vlan_initialized) return -1;
    if (vid >= VLAN_MAX_VID || vid == VLAN_DEFAULT_VID) return -1;
    vlan_active[vid / 16] &= ~(1 << (vid % 16));
    return 0;
}

int vlan_has_vid(uint16_t vid) {
    if (!vlan_initialized) return 0;
    if (vid >= VLAN_MAX_VID) return 0;
    return (vlan_active[vid / 16] >> (vid % 16)) & 1;
}

uint16_t vlan_parse_tci(const uint8_t *frame, int len) {
    if (!frame || len < 18) return 0;  /* eth(14) + vlan(4) */
    struct vlan_header *vh = (struct vlan_header *)(frame + 12);
    if (ntohs(vh->tpid) != VLAN_TPID) return 0;
    return ntohs(vh->tci) & VLAN_VID_MASK;
}

int vlan_tag_frame(uint8_t *frame, int len, uint16_t vid) {
    if (!frame || len < 14) return -1;
    if (len + 4 > 1518) return -1;  /* MTU check */

    /* Shift payload by 4 bytes to make room for VLAN header */
    memmove(frame + 18, frame + 14, len - 14);

    struct vlan_header *vh = (struct vlan_header *)(frame + 12);
    vh->tpid = htons(VLAN_TPID);
    vh->tci  = htons(vid & VLAN_VID_MASK);

    return len + 4;
}
