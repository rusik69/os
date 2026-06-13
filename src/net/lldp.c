/* lldp.c — Link Layer Discovery Protocol (802.1AB), chassis/port ID, TLV */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "net.h"
#include "net_internal.h"
#include "net_lldp.h"
#include "errno.h"

/* LLDP TLV types (IEEE 802.1AB / IEEE 802.1AB-2009) */
#define LLDP_TLV_END             0
#define LLDP_TLV_CHASSIS_ID      1
#define LLDP_TLV_PORT_ID         2
#define LLDP_TLV_TTL             3
#define LLDP_TLV_PORT_DESC       4
#define LLDP_TLV_SYSTEM_NAME     5
#define LLDP_TLV_SYSTEM_DESC     6
#define LLDP_TLV_SYSTEM_CAP      7
#define LLDP_TLV_MANAGEMENT_ADDR 8

/* Chassis ID subtypes */
#define LLDP_CHASSIS_MAC           4   /* MAC address subtype */

/* Port ID subtypes */
#define LLDP_PORT_INTERFACE        5   /* Interface name */

/* LLDP TLV header: 7-bit type, 9-bit length */
struct lldp_tlv_header {
    uint16_t type_len;  /* upper 7 bits = type, lower 9 bits = length */
} __attribute__((packed));

/* LLDP frame with mandatory TLVs */
#define LLDP_MAX_FRAME_SIZE 1500

/* Default identity strings */
#define LLDP_DEFAULT_SYSTEM_NAME "hermes-kernel"
#define LLDP_DEFAULT_PORT_PREFIX "eth"

/* Static neighbor table */
static struct lldp_neighbor lldp_neighbors[LLDP_MAX_NEIGHBORS];
static int lldp_initialised = 0;

void lldp_init(void)
{
    if (lldp_initialised) return;
    memset(lldp_neighbors, 0, sizeof(lldp_neighbors));
    lldp_initialised = 1;
    kprintf("[OK] lldp: LLDP neighbour discovery initialised (%d neighbours max)\n",
            LLDP_MAX_NEIGHBORS);
}

/* Find a free neighbor slot */
static int neighbor_find_free(void)
{
    for (int i = 0; i < LLDP_MAX_NEIGHBORS; i++) {
        if (!lldp_neighbors[i].valid) return i;
    }
    /* Overwrite oldest entry (simple LRU) */
    uint32_t oldest_tick = (uint32_t)-1;
    int oldest_idx = 0;
    for (int i = 0; i < LLDP_MAX_NEIGHBORS; i++) {
        if (lldp_neighbors[i].last_update_tick < oldest_tick) {
            oldest_tick = lldp_neighbors[i].last_update_tick;
            oldest_idx = i;
        }
    }
    return oldest_idx;
}

/* Find a neighbor by chassis ID + port ID */
static int neighbor_find(const char *chassis_id, const char *port_id)
{
    for (int i = 0; i < LLDP_MAX_NEIGHBORS; i++) {
        if (lldp_neighbors[i].valid &&
            strcmp(lldp_neighbors[i].chassis_id, chassis_id) == 0 &&
            strcmp(lldp_neighbors[i].port_id, port_id) == 0)
            return i;
    }
    return -ENOENT;
}

/* Write a TLV: type + length header + value.
 * Returns bytes written (header + value), or negative on error. */
static int lldp_write_tlv(uint8_t *buf, int buf_offset, int buf_size,
                          uint8_t type, const uint8_t *value, uint16_t len)
{
    if (buf_offset + 2 + len > buf_size) return -ENOSPC;

    struct lldp_tlv_header *hdr = (struct lldp_tlv_header *)&buf[buf_offset];
    hdr->type_len = htons((uint16_t)((type << 9) | len));

    if (len > 0 && value)
        memcpy(&buf[buf_offset + 2], value, len);

    return 2 + len;
}

/* Parse a TLV. Returns the type, sets *len and *value pointer. */
static int lldp_parse_tlv(const uint8_t *frame, uint16_t frame_len,
                          uint16_t offset, uint16_t *out_len,
                          const uint8_t **out_value)
{
    if (offset + 2 > frame_len) return -1;

    struct lldp_tlv_header *hdr = (struct lldp_tlv_header *)&frame[offset];
    uint16_t tlv_raw = ntohs(hdr->type_len);
    uint8_t  type   = (tlv_raw >> 9) & 0x7F;
    uint16_t length = tlv_raw & 0x01FF;

    if (offset + 2 + length > frame_len) return -1;

    *out_len   = length;
    *out_value = (length > 0) ? &frame[offset + 2] : NULL;
    return type;
}

/* Send an LLDP frame */
int lldp_send(int ifindex)
{
    if (!lldp_initialised) return -ENOSYS;

    uint8_t frame[LLDP_MAX_FRAME_SIZE];
    int offset = 0;
    int ret;

    /* 1. Chassis ID (MAC address subtype) */
    {
        uint8_t chassis_tlv[7];
        chassis_tlv[0] = LLDP_CHASSIS_MAC;
        memcpy(&chassis_tlv[1], net_our_mac, 6);
        ret = lldp_write_tlv(frame, offset, sizeof(frame),
                             LLDP_TLV_CHASSIS_ID, chassis_tlv, 7);
        if (ret < 0) return ret;
        offset += ret;
    }

    /* 2. Port ID (interface name subtype) */
    {
        char port_str[16];
        int n = snprintf(port_str, sizeof(port_str), "%s%d",
                         LLDP_DEFAULT_PORT_PREFIX, ifindex);
        if (n < 0 || n >= (int)sizeof(port_str))
            n = (int)sizeof(port_str) - 1;
        uint8_t port_tlv[16];
        port_tlv[0] = LLDP_PORT_INTERFACE;
        memcpy(&port_tlv[1], port_str, (size_t)n);
        ret = lldp_write_tlv(frame, offset, sizeof(frame),
                             LLDP_TLV_PORT_ID, port_tlv, (uint16_t)(1 + n));
        if (ret < 0) return ret;
        offset += ret;
    }

    /* 3. TTL (2 bytes, in seconds) */
    {
        uint16_t ttl_val = htons(120);
        ret = lldp_write_tlv(frame, offset, sizeof(frame),
                             LLDP_TLV_TTL, (uint8_t *)&ttl_val, 2);
        if (ret < 0) return ret;
        offset += ret;
    }

    /* 4. System Name */
    {
        const char *sysname = LLDP_DEFAULT_SYSTEM_NAME;
        ret = lldp_write_tlv(frame, offset, sizeof(frame),
                             LLDP_TLV_SYSTEM_NAME,
                             (const uint8_t *)sysname, (uint16_t)strlen(sysname));
        if (ret < 0) return ret;
        offset += ret;
    }

    /* 5. System Description */
    {
        const char *desc = "Hermes OS Kernel (802.1AB LLDP)";
        ret = lldp_write_tlv(frame, offset, sizeof(frame),
                             LLDP_TLV_SYSTEM_DESC,
                             (const uint8_t *)desc, (uint16_t)strlen(desc));
        if (ret < 0) return ret;
        offset += ret;
    }

    /* End TLV */
    {
        ret = lldp_write_tlv(frame, offset, sizeof(frame),
                             LLDP_TLV_END, NULL, 0);
        if (ret < 0) return ret;
        offset += ret;
    }

    /* Send to LLDP multicast MAC (01:80:C2:00:00:0E) */
    {
        uint8_t lldp_mcast[6] = { 0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E };
        send_eth(lldp_mcast, ETH_TYPE_LLDP, frame, (uint16_t)offset);
    }

    kprintf("lldp: sent LLDP frame on ifindex %d (%d bytes)\n", ifindex, offset);
    return 0;
}

/* Process a received LLDP frame */
int lldp_receive(const uint8_t *frame, uint16_t len)
{
    if (!lldp_initialised || !frame || len == 0) return -EINVAL;

    uint16_t offset = 0;
    char chassis_id[LLDP_CHASSIS_ID_MAX] = {0};
    char port_id[LLDP_PORT_ID_MAX] = {0};
    char system_name[LLDP_SYSTEM_NAME_MAX] = {0};
    uint16_t ttl = 0;
    int chassis_found = 0, port_found = 0, ttl_found = 0;

    while (offset < len) {
        uint16_t tlv_len;
        const uint8_t *tlv_value;
        int type = lldp_parse_tlv(frame, len, offset, &tlv_len, &tlv_value);
        if (type < 0) {
            kprintf("lldp: malformed TLV at offset %d\n", offset);
            return -EINVAL;
        }

        if (type == LLDP_TLV_END) break;

        switch (type) {
        case LLDP_TLV_CHASSIS_ID:
            if (tlv_len > 0) {
                uint16_t id_len = tlv_len - 1;
                if (id_len > LLDP_CHASSIS_ID_MAX - 1)
                    id_len = LLDP_CHASSIS_ID_MAX - 1;
                memcpy(chassis_id, tlv_value + 1, id_len);
                chassis_id[id_len] = '\0';
                chassis_found = 1;
            }
            break;

        case LLDP_TLV_PORT_ID:
            if (tlv_len > 0) {
                uint16_t id_len = tlv_len - 1;
                if (id_len > LLDP_PORT_ID_MAX - 1)
                    id_len = LLDP_PORT_ID_MAX - 1;
                memcpy(port_id, tlv_value + 1, id_len);
                port_id[id_len] = '\0';
                port_found = 1;
            }
            break;

        case LLDP_TLV_TTL:
            if (tlv_len >= 2) {
                ttl = ntohs(*(uint16_t *)tlv_value);
                ttl_found = 1;
            }
            break;

        case LLDP_TLV_SYSTEM_NAME:
            if (tlv_len > 0) {
                uint16_t name_len = tlv_len;
                if (name_len > LLDP_SYSTEM_NAME_MAX - 1)
                    name_len = LLDP_SYSTEM_NAME_MAX - 1;
                memcpy(system_name, tlv_value, name_len);
                system_name[name_len] = '\0';
            }
            break;

        default:
            break;
        }

        offset += 2 + tlv_len;
    }

    if (!chassis_found || !port_found || !ttl_found) {
        kprintf("lldp: incomplete mandatory TLVs (chassis=%d port=%d ttl=%d)\n",
                chassis_found, port_found, ttl_found);
        return -EINVAL;
    }

    /* Update neighbor table */
    int idx = neighbor_find(chassis_id, port_id);
    if (idx < 0) idx = neighbor_find_free();

    struct lldp_neighbor *neighbor = &lldp_neighbors[idx];
    neighbor->valid = 1;
    strncpy(neighbor->chassis_id, chassis_id, LLDP_CHASSIS_ID_MAX - 1);
    neighbor->chassis_id[LLDP_CHASSIS_ID_MAX - 1] = '\0';
    strncpy(neighbor->port_id, port_id, LLDP_PORT_ID_MAX - 1);
    neighbor->port_id[LLDP_PORT_ID_MAX - 1] = '\0';
    if (system_name[0])
        strncpy(neighbor->system_name, system_name, LLDP_SYSTEM_NAME_MAX - 1);
    neighbor->system_name[LLDP_SYSTEM_NAME_MAX - 1] = '\0';
    neighbor->ttl = ttl;
    neighbor->last_update_tick = 0;

    kprintf("lldp: discovered neighbor chassis=%s port=%s name=%s ttl=%u\n",
            chassis_id, port_id, system_name, ttl);
    return 0;
}

/* Get neighbor table */
struct lldp_neighbor *lldp_get_neighbor_table(int *count)
{
    if (!lldp_initialised) {
        if (count) *count = 0;
        return NULL;
    }

    if (count) {
        int c = 0;
        for (int i = 0; i < LLDP_MAX_NEIGHBORS; i++) {
            if (lldp_neighbors[i].valid) c++;
        }
        *count = c;
    }
    return lldp_neighbors;
}

/* Age out expired neighbors — call periodically */
void lldp_age_neighbors(void)
{
    if (!lldp_initialised) return;

    for (int i = 0; i < LLDP_MAX_NEIGHBORS; i++) {
        if (lldp_neighbors[i].valid) {
            /* Simple expiry: if ttl is small and tick is old, expire.
             * In a real implementation we'd check last_update_tick + ttl. */
            if (lldp_neighbors[i].ttl == 0) {
                memset(&lldp_neighbors[i], 0, sizeof(lldp_neighbors[i]));
            }
        }
    }
}
