#ifndef XDP_H
#define XDP_H

#include "types.h"
#include "netdevice.h"

/*
 * ── eXpress Data Path (XDP) ──────────────────────────────────────
 *
 * XDP is a programmable network hook that runs before the kernel's
 * main receive processing.  An XDP program can decide to DROP, PASS,
 * or TX (bounce back out the same interface) a packet.
 *
 * This is a minimal implementation:
 *   - XDP actions: XDP_ABORTED(0), XDP_DROP(1), XDP_PASS(2), XDP_TX(3)
 *   - Per-interface attachment via function pointer table
 *   - xdp_attach(ifname, program_fn)
 *   - xdp_detach(ifname)
 *   - xdp_run() called from net_rx_dispatch (see net.c)
 *   - Default: XDP_PASS (normal stack processing)
 *   - Sysfs exposure at /sys/class/net/<iface>/xdp/attached
 */

/* ── XDP actions ──────────────────────────────────────────────────── */
#define XDP_ABORTED  0
#define XDP_DROP     1
#define XDP_PASS     2
#define XDP_TX       3

/* ── XDP program type ─────────────────────────────────────────────── */
/* An XDP program receives the raw Ethernet frame (data, len) and the
 * incoming interface index.  It returns an XDP action code. */
typedef int (*xdp_program_fn)(const uint8_t *data, uint16_t len,
                               int ifindex);

/* ── Per-interface XDP attachment ──────────────────────────────────── */
#define XDP_MAX_ATTACHMENTS 8

struct xdp_attachment {
    int             ifindex;       /* interface index (-1 = free slot) */
    xdp_program_fn  program;       /* the XDP program (NULL = none) */
};

/* ── API ──────────────────────────────────────────────────────────── */

/* Initialise the XDP subsystem. */
void xdp_init(void);

/* Attach an XDP program to an interface by name.
 * @ifname:   interface name (e.g. "eth0")
 * @program:  XDP program function pointer
 * Returns 0 on success, -1 on failure. */
int xdp_attach(const char *ifname, xdp_program_fn program);

/* Detach the XDP program from an interface by name.
 * Returns 0 on success, -1 if no program was attached. */
int xdp_detach(const char *ifname);

/* Run the XDP program for the given interface on the packet.
 * Called from the network receive path (net_rx_dispatch).
 * Returns XDP_PASS if no program is attached, otherwise the program's result. */
int xdp_run(const uint8_t *data, uint16_t len, int ifindex);

#endif /* XDP_H */
