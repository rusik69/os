/* cmd_ebtables.c — Ethernet bridge firewall */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "types.h"
#include "netdevice.h"

/* ── Bridge FDB entry (mirror of bridge.h) ───────────────────── */
#define BRIDGE_FDB_SIZE 64
#define BRIDGE_MAX_PORTS 32

struct bridge_fdb_entry {
    uint8_t  mac[6];
    int      port;
    uint64_t learn_tick;
    int      valid;
};

/* ── External bridge API (from net/bridge.c) ─────────────────── */
int  bridge_init(void);
int  bridge_add_port(int port_iface);
int  bridge_remove_port(int port_iface);
int  bridge_fdb_lookup(const uint8_t *mac);

static void ebtables_usage(void)
{
    kprintf("Usage: ebtables -L\n");
    kprintf("Show Ethernet bridge forwarding state.\n");
}

void cmd_ebtables(const char *args)
{
    if (!args || !*args) {
        ebtables_usage();
        return;
    }

    while (*args == ' ') args++;

    if (strncmp(args, "-L", 2) == 0) {
        kprintf("Bridge table: mac table\n");
        kprintf("port no mac addr            is local?       ageing timer\n");
        kprintf("====== ==================== =============   ============\n");

        /* Show registered net devices as bridge ports */
        int count = 0;
        for (int i = 0; i < NETDEV_MAX; i++) {
            struct net_device *nd = netif_get(i);
            if (!nd) continue;
            kprintf("Port %d: %-6s  MAC %02x:%02x:%02x:%02x:%02x:%02x  MTU %d\n",
                    i, nd->name,
                    (unsigned int)nd->mac[0], (unsigned int)nd->mac[1],
                    (unsigned int)nd->mac[2], (unsigned int)nd->mac[3],
                    (unsigned int)nd->mac[4], (unsigned int)nd->mac[5],
                    nd->mtu);
            count++;
        }

        if (count == 0) {
            kprintf("  (no bridge interfaces registered)\n");
        }

        kprintf("\n%d bridge interfaces total\n", count);
        kprintf("Bridge filtering: ebtables rules not supported in this kernel.\n");
    } else {
        kprintf("ebtables: unknown option '%s'\n", args);
        ebtables_usage();
    }
}
