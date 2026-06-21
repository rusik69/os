#define KERNEL_INTERNAL
#include "types.h"
#include "errno.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"

/* ── ARP cache size tunable ──────────────────────────────────────────── *
 * Allow runtime adjustment of the ARP cache maximum size.
 */

/* Current ARP cache maximum (default: 16 from net_internal.h) */
static int arp_cache_max = ARP_CACHE_SIZE;

int net_get_arp_cache_max(void) {
    return arp_cache_max;
}

int net_set_arp_cache_max(int max) {
    if (max < 1 || max > 256) return -EINVAL;
    arp_cache_max = max;
    kprintf("[net] ARP cache max set to %d\n", max);
    return 0;
}

/* ── Implement: net_ext_ioctl ────────────────── */
int net_ext_ioctl(int sock, int cmd, void *arg)
{
    kprintf("[net_ext] net_ext_ioctl: stub (basic)\n");
    return -EOPNOTSUPP;
}
