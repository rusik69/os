/* cmd_route.c — route command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"

/* Check if netmask is contiguous (all 1s then all 0s) */
static int is_contiguous_netmask(uint32_t mask) {
    if (mask == 0) return 1; /* 0.0.0.0 is valid (default route) */
    /* Shift right until we find a 0 bit, then ensure all remaining bits are 0 */
    int found_zero = 0;
    for (int i = 31; i >= 0; i--) {
        if (mask & (1U << i)) {
            if (found_zero) return 0;  /* 1 after 0 */
        } else {
            found_zero = 1;
        }
    }
    return 1;
}

/* Check if IP is within a subnet defined by (network, mask) */
static int ip_in_subnet(uint32_t ip, uint32_t network, uint32_t mask) {
    return (ip & mask) == (network & mask);
}

/* Parse dotted-decimal IP string to uint32 */
static int parse_ip(const char *s, uint32_t *out) {
    uint32_t ip = 0;
    int octets = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            uint32_t val = 0;
            while (*s >= '0' && *s <= '9') {
                val = val * 10 + (*s - '0');
                s++;
            }
            if (val > 255) return -1;
            ip = (ip << 8) | val;
            octets++;
            if (*s == '.') s++;
            else if (*s == '\0' || *s == ' ') break;
            else return -1;
        } else {
            return -1;
        }
    }
    if (octets != 4) return -1;
    *out = ip;
    return 0;
}

/* Convert prefix/CIDR length to netmask */
static uint32_t cidr_to_mask(int prefix) {
    if (prefix < 0 || prefix > 32) return 0;
    if (prefix == 0) return 0;
    return ~((1U << (32 - prefix)) - 1);
}

void cmd_route(const char *args) {
    /* If no args, display current route table */
    if (!args || !*args) {
        uint32_t gw = libc_net_get_gateway();
        uint32_t mask = libc_net_get_mask();
        uint8_t ip[4];
        (void)ip;
        kprintf("Routing table:\n");
        kprintf("Destination     Gateway         Mask            Iface\n");
        kprintf("%-15s %-15s %u.%u.%u.%u   eth0\n",
                "0.0.0.0", "0.0.0.0",
                (unsigned int)((mask >> 24) & 0xFF), (unsigned int)((mask >> 16) & 0xFF),
                (unsigned int)((mask >> 8) & 0xFF), (unsigned int)(mask & 0xFF));
        kprintf("%-15s %u.%u.%u.%u  %u.%u.%u.%u   eth0\n",
                "0.0.0.0",
                (unsigned int)((gw >> 24) & 0xFF), (unsigned int)((gw >> 16) & 0xFF),
                (unsigned int)((gw >> 8) & 0xFF), (unsigned int)(gw & 0xFF),
                (unsigned int)((mask >> 24) & 0xFF), (unsigned int)((mask >> 16) & 0xFF),
                (unsigned int)((mask >> 8) & 0xFF), (unsigned int)(mask & 0xFF));
        return;
    }

    /* Parse: route add|del <network> <netmask> <gateway> */
    /* Simple parsing */
    const char *p = args;
    while (*p == ' ') p++;

    /* Check for 'add' subcommand */
    if (strncmp(p, "add ", 4) == 0 || strncmp(p, "add\t", 4) == 0) {
        p += 4;
        while (*p == ' ') p++;

        uint32_t network, netmask, gateway;
        char token[64];
        int ti;

        /* Parse network */
        ti = 0;
        while (*p && *p != ' ' && ti < 63) token[ti++] = *p++;
        token[ti] = '\0';
        if (parse_ip(token, &network) < 0) {
            kprintf("route: invalid network address '%s'\n", token);
            return;
        }
        while (*p == ' ') p++;

        /* Parse netmask */
        ti = 0;
        while (*p && *p != ' ' && ti < 63) token[ti++] = *p++;
        token[ti] = '\0';
        if (token[0] == '\0') {
            kprintf("route: missing netmask\n");
            return;
        }
        /* Check if it's a prefix length (e.g., /24) or dotted decimal */
        if (token[0] == '/') {
            int prefix = 0;
            const char *pp = token + 1;
            while (*pp >= '0' && *pp <= '9') {
                prefix = prefix * 10 + (*pp - '0');
                pp++;
            }
            if (prefix < 0 || prefix > 32) {
                kprintf("route: invalid prefix length %d (must be 0-32)\n", prefix);
                return;
            }
            netmask = cidr_to_mask(prefix);
        } else {
            if (parse_ip(token, &netmask) < 0) {
                kprintf("route: invalid netmask '%s'\n", token);
                return;
            }
            /* Validate contiguous mask */
            if (!is_contiguous_netmask(netmask)) {
                kprintf("route: netmask %u.%u.%u.%u is not contiguous\n",
                        (unsigned int)((netmask >> 24) & 0xFF),
                        (unsigned int)((netmask >> 16) & 0xFF),
                        (unsigned int)((netmask >> 8) & 0xFF),
                        (unsigned int)(netmask & 0xFF));
                return;
            }
        }
        while (*p == ' ') p++;

        /* Parse gateway */
        ti = 0;
        while (*p && *p != ' ' && ti < 63) token[ti++] = *p++;
        token[ti] = '\0';
        if (token[0] == '\0') {
            kprintf("route: missing gateway\n");
            return;
        }
        if (parse_ip(token, &gateway) < 0) {
            kprintf("route: invalid gateway address '%s'\n", token);
            return;
        }

        /* Validate gateway is within subnet */
        if (netmask != 0 && !ip_in_subnet(gateway, network, netmask)) {
            kprintf("route: gateway %u.%u.%u.%u is not in subnet %u.%u.%u.%u/%u.%u.%u.%u\n",
                    (unsigned int)((gateway >> 24) & 0xFF),
                    (unsigned int)((gateway >> 16) & 0xFF),
                    (unsigned int)((gateway >> 8) & 0xFF),
                    (unsigned int)(gateway & 0xFF),
                    (unsigned int)((network >> 24) & 0xFF),
                    (unsigned int)((network >> 16) & 0xFF),
                    (unsigned int)((network >> 8) & 0xFF),
                    (unsigned int)(network & 0xFF),
                    (unsigned int)((netmask >> 24) & 0xFF),
                    (unsigned int)((netmask >> 16) & 0xFF),
                    (unsigned int)((netmask >> 8) & 0xFF),
                    (unsigned int)(netmask & 0xFF));
            return;
        }

        kprintf("route: add %u.%u.%u.%0u/%u.%u.%u.%u via %u.%u.%u.%u (validated)\n",
                (unsigned int)((network >> 24) & 0xFF),
                (unsigned int)((network >> 16) & 0xFF),
                (unsigned int)((network >> 8) & 0xFF),
                (unsigned int)(network & 0xFF),
                (unsigned int)((netmask >> 24) & 0xFF),
                (unsigned int)((netmask >> 16) & 0xFF),
                (unsigned int)((netmask >> 8) & 0xFF),
                (unsigned int)(netmask & 0xFF),
                (unsigned int)((gateway >> 24) & 0xFF),
                (unsigned int)((gateway >> 16) & 0xFF),
                (unsigned int)((gateway >> 8) & 0xFF),
                (unsigned int)(gateway & 0xFF));
        kprintf("route: route added (simulated)\n");
        return;
    }

    /* Unknown subcommand */
    kprintf("route: unknown subcommand (use 'route add <network> <mask> <gateway>')\n");
}
