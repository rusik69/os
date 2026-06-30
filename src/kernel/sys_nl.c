/*
 * sys_nl.c — Netlink syscall dispatch layer
 *
 * Provides the syscall-level entry points for AF_NETLINK socket
 * operations.  Netlink sockets are used for kernel-userspace
 * communication (netlink(7), NETLINK_ROUTE, NETLINK_GENERIC, etc.).
 *
 * Most netlink operations share the generic socket dispatch path
 * (sys_socket_impl, sys_bind_impl, etc.), but protocol-family-specific
 * validation, kernel-side dispatching, and RTNETLINK handling live here.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "errno.h"
#include "netlink.h"
#include "socket.h"
#include "module.h"
#include "printf.h"

/* ── Module information ────────────────────────────────────────────── */

MODULE_LICENSE("MIT");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Netlink syscall dispatch layer (AF_NETLINK)");
MODULE_AUTHOR("OS Kernel Team");

/* ── Netlink socket validation ─────────────────────────────────────── */

/*
 * Validate that a socket fd is an AF_NETLINK socket.
 * Returns the socket pointer on success, NULL on error (sets errno).
 */
struct socket *sys_nl_get_sock(int sockfd)
{
	struct socket *s = sock_get(sockfd);
	if (!s) {
		return NULL; /* errno already -EBADF from sock_get */
	}
	if (s->domain != AF_NETLINK) {
		return NULL; /* -EINVAL: not a netlink socket */
	}
	return s;
}

/*
 * Get the netlink protocol family for a socket fd.
 * Returns the NETLINK_* protocol number, or -EINVAL if not a netlink socket.
 */
int sys_nl_get_protocol(int sockfd)
{
	struct socket *s = sys_nl_get_sock(sockfd);
	if (!s)
		return -EINVAL;
	return netlink_get_protocol(sockfd);
}

/* ── Initialization / registration ─────────────────────────────────── */

/*
 * Initialize the netlink syscall layer.
 * Called once during kernel init.  Ensures the AF_NETLINK subsystem
 * is ready before any userspace process can create netlink sockets.
 */
void __init sys_nl_init(void)
{
	af_netlink_init();
	kprintf("[OK] sys_nl: netlink syscall layer initialized\n");
}
