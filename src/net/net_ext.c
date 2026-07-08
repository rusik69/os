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

static int net_get_arp_cache_max(void) {
    return arp_cache_max;
}

static int net_set_arp_cache_max(int max) {
    if (max < 1 || max > 256) return -EINVAL;
    arp_cache_max = max;
    kprintf("[net] ARP cache max set to %d\n", max);
    return 0;
}

/* ── Implement: net_ext_ioctl ────────────────── */
static int net_ext_ioctl(int sock, int cmd, void *arg)
{
    if (sock < 0 || !arg) {
        kprintf("[net_ext] net_ext_ioctl: invalid parameter (sock=%d arg=%p)\n", sock, arg);
        return -EINVAL;
    }
    kprintf("[net_ext] net_ext_ioctl: sock=%d cmd=%d arg=%p (stub)\n", sock, cmd, arg);
    return -EOPNOTSUPP;
}
