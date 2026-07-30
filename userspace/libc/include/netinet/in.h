/*
 * netinet/in.h — Internet address family definitions (POSIX.1-2017)
 *
 * This header defines the IPv4 socket address structures, protocol
 * number constants, and utility macros used by AF_INET sockets.
 * Every program that communicates over IPv4 should include this
 * header (directly or via <sys/socket.h>) to obtain:
 *
 *   • struct in_addr       — 4-byte IPv4 address (in network byte order)
 *   • struct sockaddr_in   — AF_INET socket address (IP + port)
 *   • INADDR_* constants   — Special-purpose IPv4 addresses
 *   • IPPROTO_* constants  — IP protocol numbers (TCP, UDP, ICMP, etc.)
 *   • INET_ADDRSTRLEN      — Buffer size large enough for any IPv4 string
 *
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │  Address representation                                            │
 * ├─────────────────────────────────────────────────────────────────────┤
 * │  IPv4 addresses are stored in network byte order (big-endian)      │
 * │  in a 32-bit unsigned integer.  The struct in_addr wraps this      │
 * │  value so that callers cannot accidentally confuse an IP address   │
 * │  with an arbitrary uint32_t.  Use sin_addr.s_addr to access the    │
 * │  raw big-endian value.                                             │
 * │                                                                     │
 * │  struct in_addr { uint32_t s_addr; };   // IPv4 address (big-endian)│
 * │  struct sockaddr_in {                                               │
 * │      uint16_t sin_family;   // AF_INET (must be 2)                 │
 * │      uint16_t sin_port;     // Port number (big-endian)            │
 * │      struct in_addr sin_addr; // IPv4 address (big-endian)         │
 * │      char sin_zero[8];     // Padding; must be zero                │
 * │  };                                                                 │
 * └─────────────────────────────────────────────────────────────────────┘
 *
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │  Byte-order conversion                                             │
 * ├─────────────────────────────────────────────────────────────────────┤
 * │  Network byte order is big-endian.  Use these helpers (from        │
 * │  <arpa/inet.h> or <endian.h>) to convert between host and network  │
 * │  byte order:                                                       │
 * │                                                                     │
 * │    htons()  — Host to network (uint16_t, for port numbers)         │
 * │    htonl()  — Host to network (uint32_t, for IPv4 addresses)       │
 * │    ntohs()  — Network to host (uint16_t)                           │
 * │    ntohl()  — Network to host (uint32_t)                           │
 * │                                                                     │
 * │  String conversion (from <arpa/inet.h>):                           │
 * │    inet_pton(AF_INET, "192.168.1.1", &sin_addr)  — text → binary  │
 * │    inet_ntop(AF_INET, &sin_addr, buf, INET_ADDRSTRLEN) — binary → │
 * │        text (dotted-decimal)                                       │
 * └─────────────────────────────────────────────────────────────────────┘
 *
 * Reference: POSIX.1-2017, <netinet/in.h> section,
 *            IANA Protocol Number Assignment (RFC 790 / RFC 1700),
 *            and Linux ip(7) / inet(3) man-pages.
 */

#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#include <stdint.h>

/* ================================================================= */
/*  IP protocol numbers — used in the protocol field of IPv4 headers */
/*  and passed as the third argument to socket(AF_INET, SOCK_*, id). */
/*  Values are defined by IANA and are shared across all modern OSes. */
/* ================================================================= */

/* Internet Protocol numbers (excerpt — see IANA for full list) */
#define IPPROTO_IP          0   /* IP-over-IP (pseudo-protocol for raw IP) */
#define IPPROTO_ICMP        1   /* Internet Control Message Protocol       */
#define IPPROTO_IGMP        2   /* Internet Group Management Protocol      */
#define IPPROTO_TCP         6   /* Transmission Control Protocol          */
#define IPPROTO_UDP         17  /* User Datagram Protocol                 */
#define IPPROTO_IPV6        41  /* IPv6 encapsulation (tunnel)            */
#define IPPROTO_ICMPV6      58  /* ICMP for IPv6                          */
#define IPPROTO_RAW         255 /* Raw IP socket (no protocol header)     */

/* ================================================================= */
/*  IP option / level constants                                       */
/* ================================================================= */

#define SOL_IP              IPPROTO_IP   /* IP-level socket options     */

/* ================================================================= */
/*  Address structures                                                */
/* ================================================================= */

/*
 * struct in_addr — IPv4 address in network byte order.
 *
 * The single member s_addr holds the 32-bit IPv4 address in big-endian
 * byte order.  Use htonl() to convert a host-order address, or
 * inet_pton(AF_INET, ...) to parse a dotted-decimal string.
 *
 * Example:
 *   struct in_addr addr;
 *   addr.s_addr = htonl(0x7f000001);   // 127.0.0.1 (localhost)
 *   inet_pton(AF_INET, "127.0.0.1", &addr);  // equivalent
 */
struct in_addr {
    uint32_t s_addr;    /* IPv4 address in network byte order */
};

/*
 * struct sockaddr_in — AF_INET socket address (RFC 793 §3.1).
 *
 * This structure is passed to bind(), connect(), sendto(), and
 * accept()-like calls when the address family is AF_INET.  The
 * structure is laid out so that it can be cast to/from struct sockaddr
 * for protocol-independent code.
 *
 * Fields:
 *   sin_family — Must always be set to AF_INET (2).  The kernel
 *                uses this to verify the address family.
 *
 *   sin_port   — Port number in network byte order (big-endian).
 *                Use htons(port) to convert from host order.
 *
 *   sin_addr   — IPv4 address in network byte order.  Use
 *                INADDR_ANY for wildcard bind, INADDR_LOOPBACK
 *                for localhost, or inet_pton() for a specific IP.
 *
 *   sin_zero   — Padding bytes that must be zeroed before use.
 *                memset(&addr, 0, sizeof(addr)) is the simplest
 *                way to ensure this.
 *
 * Layout (16 bytes total, same as struct sockaddr):
 *    ┌─────────┬──────────┬──────────────┬──────────────┐
 *    │ family  │  port    │    addr      │   zero       │
 *    │  2 bytes│  2 bytes │  4 bytes     │  8 bytes     │
 *    └─────────┴──────────┴──────────────┴──────────────┘
 *    Offset:   0          2              6              14
 *
 * Example:
 *   struct sockaddr_in addr;
 *   memset(&addr, 0, sizeof(addr));
 *   addr.sin_family = AF_INET;
 *   addr.sin_port   = htons(80);            // HTTP
 *   addr.sin_addr.s_addr = htonl(0xc0a80101); // 192.168.1.1
 */
struct sockaddr_in {
    uint16_t sin_family;     /* Address family: always AF_INET (2)   */
    uint16_t sin_port;       /* Port number in network byte order    */
    struct in_addr sin_addr; /* IPv4 address in network byte order   */
    char sin_zero[8];        /* Padding; must be zero-filled         */
};

/* ================================================================= */
/*  Special IPv4 addresses                                            */
/* ================================================================= */

/*
 * INADDR_ANY — Wildcard address (0.0.0.0).
 *
 * When binding a socket, using INADDR_ANY causes the socket to
 * listen on all available network interfaces.  This is the most
 * common choice for server applications that should accept
 * connections on any IP address assigned to the host.
 *
 *   addr.sin_addr.s_addr = htonl(INADDR_ANY);
 *
 * Value: 0.0.0.0 in network byte order (0x00000000).
 */
#define INADDR_ANY          ((uint32_t) 0x00000000)

/*
 * INADDR_LOOPBACK — Loopback address (127.0.0.1).
 *
 * The loopback address routes packets back to the local machine
 * without leaving the host.  Used for inter-process communication
 * on the same machine and for testing.
 *
 *   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
 *
 * Value: 127.0.0.1 in network byte order (0x7f000001).
 */
#define INADDR_LOOPBACK     ((uint32_t) 0x7f000001)

/*
 * INADDR_BROADCAST — Broadcast address (255.255.255.255).
 *
 * Packets sent to this address are broadcast to all hosts on the
 * local network segment.  The socket must have SO_BROADCAST enabled.
 *
 *   addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
 *
 * Value: 255.255.255.255 in network byte order (0xffffffff).
 */
#define INADDR_BROADCAST    ((uint32_t) 0xffffffff)

/*
 * INADDR_NONE — Error sentinel (255.255.255.255).
 *
 * Historically returned by inet_addr() on error.  Because this
 * overlaps with INADDR_BROADCAST, modern code should use
 * inet_pton() instead of inet_addr().
 *
 * Value: same as INADDR_BROADCAST (0xffffffff).
 */
#define INADDR_NONE         ((uint32_t) 0xffffffff)

/*
 * INADDR_UNSPEC_GROUP — Unspecified multicast group (224.0.0.0).
 *
 * The first IPv4 multicast address in the 224.0.0.0/4 range.
 * 224.0.0.1 is the "all hosts" group; 224.0.0.2 is "all routers".
 */
#define INADDR_UNSPEC_GROUP ((uint32_t) 0xe0000000)

/* ================================================================= */
/*  Buffer sizes                                                      */
/* ================================================================= */

/*
 * INET_ADDRSTRLEN — Minimum buffer size to hold any IPv4 address
 * in dotted-decimal string format (including the NUL terminator).
 *
 * The longest possible IPv4 string is "255.255.255.255" (15 chars)
 * plus a terminating NUL byte → 16 characters.
 *
 * Usage:
 *   char buf[INET_ADDRSTRLEN];
 *   inet_ntop(AF_INET, &addr, buf, sizeof(buf));
 */
#define INET_ADDRSTRLEN     16

/*
 * INET6_ADDRSTRLEN — Minimum buffer size to hold any IPv6 address
 * in string format (including the NUL terminator).
 *
 * The longest IPv6 string is "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"
 * (39 chars) plus a NUL byte → 46 characters.
 * Defined here for convenience; <netinet/in.h> in POSIX guarantees both.
 */
#define INET6_ADDRSTRLEN    46

/* ================================================================= */
/*  System-scope multicast socket options                             */
/* ================================================================= */

/*
 * IP_MULTICAST_TTL / IP_MULTICAST_LOOP / IP_ADD_MEMBERSHIP /
 * IP_DROP_MEMBERSHIP — IGMP multicast socket options.
 * Used with setsockopt(SOL_IP, ...).
 */
#define IP_MULTICAST_IF     32   /* set outgoing multicast interface   */
#define IP_MULTICAST_TTL    33   /* set multicast TTL (default 1)     */
#define IP_MULTICAST_LOOP   34   /* enable/disable loopback of mcast  */
#define IP_ADD_MEMBERSHIP   35   /* join a multicast group             */
#define IP_DROP_MEMBERSHIP  36   /* leave a multicast group            */

/*
 * struct ip_mreq — Multicast group request structure.
 * Used with IP_ADD_MEMBERSHIP / IP_DROP_MEMBERSHIP.
 */
struct ip_mreq {
    struct in_addr imr_multiaddr;   /* multicast group to join/leave   */
    struct in_addr imr_interface;   /* local interface (INADDR_ANY)    */
};

#endif /* _NETINET_IN_H */
