#define KERNEL_INTERNAL
#include "xdp.h"
#include "netdevice.h"
#include "printf.h"
#include "string.h"
#include "sysfs.h"
#include "spinlock.h"

/*
 * ── XDP Implementation ─────────────────────────────────────────────
 *
 * Provides a per-interface XDP program hook that runs in the receive
 * path (net_rx_dispatch) before the main protocol dispatch.
 *
 * The XDP hook is called immediately after basic Ethernet header
 * validation.  The program may inspect/modify the packet and return
 * an action (XDP_PASS, XDP_DROP, XDP_TX, XDP_ABORTED).
 *
 * Currently, the default return for un-attached interfaces is XDP_PASS
 * (normal processing continues).
 */

/* ── Global state ─────────────────────────────────────────────────── */

static struct xdp_attachment g_xdp_attachments[XDP_MAX_ATTACHMENTS];
static int g_xdp_initialized = 0;
static spinlock_t g_xdp_lock = SPINLOCK_INIT;

/* ── Initialisation ───────────────────────────────────────────────── */

void xdp_init(void)
{
    if (g_xdp_initialized) return;

    for (int i = 0; i < XDP_MAX_ATTACHMENTS; i++) {
        g_xdp_attachments[i].ifindex = -1;
        g_xdp_attachments[i].program = NULL;
    }

    g_xdp_initialized = 1;
    kprintf("[OK] XDP initialized\n");
}

/* ── Find / free slot helpers ─────────────────────────────────────── */

static int xdp_find_by_ifindex(int ifindex)
{
    for (int i = 0; i < XDP_MAX_ATTACHMENTS; i++) {
        if (g_xdp_attachments[i].ifindex == ifindex)
            return i;
    }
    return -1;
}

static int xdp_find_free_slot(void)
{
    for (int i = 0; i < XDP_MAX_ATTACHMENTS; i++) {
        if (g_xdp_attachments[i].ifindex < 0)
            return i;
    }
    return -1;
}

/* ── Attach / detach ──────────────────────────────────────────────── */

int xdp_attach(const char *ifname, xdp_program_fn program)
{
    if (!ifname || !program) return -1;
    if (!g_xdp_initialized) return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_xdp_lock, &irq_flags);

    /* Resolve interface name */
    int ifindex = netif_name_to_index(ifname);
    if (ifindex < 0) {
        spinlock_irqsave_release(&g_xdp_lock, irq_flags);
        return -1;
    }

    /* Check if already attached */
    int idx = xdp_find_by_ifindex(ifindex);
    if (idx >= 0) {
        /* Replace existing program */
        g_xdp_attachments[idx].program = program;
        spinlock_irqsave_release(&g_xdp_lock, irq_flags);
        return 0;
    }

    /* Find empty slot */
    idx = xdp_find_free_slot();
    if (idx < 0) {
        spinlock_irqsave_release(&g_xdp_lock, irq_flags);
        return -1;
    }

    g_xdp_attachments[idx].ifindex = ifindex;
    g_xdp_attachments[idx].program = program;

    /* Create /sys/class/net/<ifname>/xdp/ directory */
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/net/%s/xdp", ifname);
    sysfs_create_dir(path);

    /* Create attached file */
    char att_path[80];
    snprintf(att_path, sizeof(att_path), "/sys/class/net/%s/xdp/attached", ifname);
    sysfs_create_file(att_path, "1\n");

    kprintf("[XDP] Program attached to %s (ifindex %d, slot %d)\n",
            ifname, ifindex, idx);

    spinlock_irqsave_release(&g_xdp_lock, irq_flags);
    return 0;
}

int xdp_detach(const char *ifname)
{
    if (!ifname) return -1;
    if (!g_xdp_initialized) return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_xdp_lock, &irq_flags);

    int ifindex = netif_name_to_index(ifname);
    if (ifindex < 0) {
        spinlock_irqsave_release(&g_xdp_lock, irq_flags);
        return -1;
    }

    int idx = xdp_find_by_ifindex(ifindex);
    if (idx < 0) {
        spinlock_irqsave_release(&g_xdp_lock, irq_flags);
        return -1;
    }

    /* Remove sysfs entries */
    char att_path[80];
    snprintf(att_path, sizeof(att_path), "/sys/class/net/%s/xdp/attached", ifname);
    sysfs_remove(att_path);

    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "/sys/class/net/%s/xdp", ifname);
    sysfs_remove(dir_path);

    g_xdp_attachments[idx].ifindex = -1;
    g_xdp_attachments[idx].program = NULL;

    kprintf("[XDP] Program detached from %s\n", ifname);

    spinlock_irqsave_release(&g_xdp_lock, irq_flags);
    return 0;
}

/* ── XDP run — called from net_rx_dispatch ─────────────────────────── */

int xdp_run(const uint8_t *data, uint16_t len, int ifindex)
{
    if (!g_xdp_initialized)
        return XDP_PASS;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_xdp_lock, &irq_flags);

    int idx = xdp_find_by_ifindex(ifindex);
    xdp_program_fn program = (idx >= 0) ? g_xdp_attachments[idx].program : NULL;

    spinlock_irqsave_release(&g_xdp_lock, irq_flags);

    if (!program)
        return XDP_PASS;

    return program(data, len, ifindex);
}
#include "module.h"
module_init(xdp_init);
