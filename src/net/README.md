# Networking Subsystem

**Path:** `src/net/`
**Headers:** `src/include/net.h`, `src/include/socket.h`, `src/include/net_internal.h`

The networking subsystem implements a layered in-kernel TCP/IP stack with a
BSD socket API, extensive transport protocols (TCP with 8+ congestion control
algorithms, UDP, SCTP, DCCP), IPv4/IPv6, netfilter packet filtering with
connection tracking and NAT, XDP fast path, tunneling (IPIP/GRE/VXLAN/IPsec),
and support for multiple NIC drivers.

## Architecture

```
Socket Layer (socket.c, socket_ext.c) — AF_INET, AF_INET6, AF_UNIX, AF_PACKET, AF_NETLINK, AF_CAN, AF_TIPC, AF_XDP
     ↕
Transport Layer — TCP (net_tcp.c), UDP (net_udp.c), SCTP (sctp.c), DCCP (dccp.c)
     + congestion control modules: cubic, bbr, bbr2, vegas, westwood, hybla, illinois, bic
     ↕
Network Layer — IPv4 (net.c), IPv6 (ipv6.c), ARP, ICMP, IGMP, routing
     + tunnels: IPIP, GRE, VXLAN, IPsec, L2TP, PPTP, WireGuard
     ↕
Netfilter (netfilter.c, nf_tables.c, conntrack.c) — PREROUTING/LOCAL_IN/FORWARD/LOCAL_OUT/POSTROUTING
     ↕
Link Layer — Ethernet, VLAN 802.1Q, bridging, STP, LACP, LLDP, MACsec
     ↕
Netdevice / Drivers — netdevice.c + NIC drivers in src/drivers/
```

## File Descriptions

| File | Description |
|------|-------------|
| **Core Stack** | |
| `net.c` | Network layer — IPv4 routing (static table), IP fragmentation/reassembly, ICMP echo/error, ARP cache (16 entries, 300s timeout), IP forwarding control |
| `net_tcp.c` | TCP — full 11-state state machine, 16-entry connection table, congestion control (Reno/CUBIC/BBR/BIC/Vegas/Westwood/Hybla/Illinois), RACK loss detection, TFO, SYN cookies, SACK, MD5, Nagle, keepalive, window scaling, MPTCP |
| `net_udp.c` | UDP — connection table with up to MAX_UDP_BINDINGS, connected sockets, broadcast/multicast, checksum verification |
| `ipv6.c` | IPv6 — SLAAC via Router Advertisements, NDP, ICMPv6, autoloads ipv6 module on AF_INET6 socket creation |
| `socket.c` | BSD socket API — socket/bind/connect/listen/accept/send/recv/poll/select, fd mapping at offset 100, 32-entry socket table |
| `socket_ext.c` | Socket extensions — ancillary data (CMSG), SO_PASSCRED, SO_PEERCRED, timestamping |
| `netdevice.c` | Netdevice layer — NIC registration with name/MAC/callbacks, link state, MTU, multi-queue RSS, interrupt moderation, RPS/RFS |
| **Transport & Congestion Control** | |
| `tcp_cubic.c` | CUBIC congestion control — hybrid slow start, TCP-friendly mode |
| `tcp_bbr.c` | BBR congestion control — v1 model-based bandwidth/delay probing |
| `tcp_bbr2.c` | BBRv2 congestion control — v2 with ECN and packet loss tolerance |
| `tcp_vegas.c` | Vegas congestion control — proactive delay-based congestion avoidance |
| `tcp_westwood.c` | Westwood+ — bandwidth estimation using ACK rate for slow start/congestion avoidance |
| `tcp_hybla.c` | Hybla — TCP for high-latency satellite links |
| `tcp_illinois.c` | Illinois — congestion control using queue delay and loss |
| `tcp_bic.c` | BIC — binary increase congestion control (predecessor to CUBIC) |
| `sctp.c` | SCTP — Stream Control Transmission Protocol, multi-homing, multi-streaming |
| `dccp.c` | DCCP — Datagram Congestion Control Protocol, unreliable with congestion control |
| **Netfilter & Security** | |
| `netfilter.c` | Packet filtering — five hook points, priority-sorted handler chains, 64 static rules, NAT (SNAT/DNAT), SOCKS5 proxy |
| `nf_tables.c` | nf_tables interface — kernel side of netlink nf_tables protocol |
| `conntrack.c` | Connection tracking — 256 concurrent connections, TCP/UDP/ICMP state machines, tuple lookup, protocol-specific timeouts, FTP/SIP helpers |
| `conntrack_helpers.c` | Connection tracking helpers — ALG support for FTP and SIP protocols |
| `socks5.c` | SOCKS5 proxy client — TCP-connect tunneling, username/password auth, remote DNS resolution |
| `ipsec.c` | IPsec — ESP/AH protocols, Security Association management, encryption + authentication |
| `pfkey.c` | PF_KEY socket — key management API for IPsec SA/SPD manipulation |
| `wireguard.c` | WireGuard — modern VPN protocol, Noise Protocol Framework, ChaCha20Poly1305 |
| **Tunnels** | |
| `ipip.c` | IPIP tunnel — RFC 2003, IP over IP encapsulation |
| `gre.c` | GRE tunnel — RFC 2784, Generic Routing Encapsulation |
| `vxlan.c` | VXLAN — RFC 7348, Virtual Extensible LAN, VTEP functionality |
| `l2tp.c` | L2TPv3 — Layer 2 Tunneling Protocol v3, UDP/IP encapsulation |
| `pptp.c` | PPTP — Point-to-Point Tunneling Protocol (RFC 2637) |
| `6lowpan.c` | 6LoWPAN — IPv6 over IEEE 802.15.4, header compression, fragmentation |
| **Link Layer** | |
| `vlan.c` | VLAN — IEEE 802.1Q, VLAN tagging/untagging, filter by VID |
| `bridge.c` | Ethernet bridge — STP, IGMP snooping, FDB learning, port isolation |
| `stp.c` | Spanning Tree Protocol — IEEE 802.1D, BPDU processing, port states |
| `lacp.c` | LACP — IEEE 802.3ad Link Aggregation Control Protocol |
| `lldp.c` | LLDP — IEEE 802.1AB Link Layer Discovery Protocol |
| `macsec.c` | MACsec — IEEE 802.1AE link-layer encryption, GCM-AES-128 |
| `garp.c` | GARP — Generic Attribute Registration Protocol |
| `mrp.c` | MRP — Multiple Registration Protocol |
| **Advanced** | |
| `xdp.c` | XDP — eXpress Data Path, BPF-based early packet processing at driver level, supports XDP_DROP/PASS/TX, AF_XDP zero-copy |
| `rps.c` | Receive Packet Steering — flow hash-based packet distribution across CPUs |
| `pkt_sched.c` | Packet scheduler — queuing disciplines, TBF (token bucket), pfifo_fast |
| `fq_codel.c` | FQ-CoDel — Fair Queuing with Controlled Delay AQM |
| `cake.c` | CAKE — Common Applications Kept Enhanced, integrated AQM + shaping |
| `openvswitch.c` | Open vSwitch — flow table-based switching with kernel datapath |
| `ipvs.c` | IPVS — IP Virtual Server, Layer-4 load balancing (RR, WRR, LC, WLC, SH, DH) |
| `bonding.c` | (handled via driver) — see `src/drivers/bonding.c` |
| **Socket Families** | |
| `af_unix.c` | AF_UNIX — Unix domain sockets, SOCK_STREAM and SOCK_DGRAM, path-based addressing |
| `af_packet.c` | AF_PACKET — raw packet sockets, SOCK_RAW and SOCK_DGRAM |
| `netlink.c` | AF_NETLINK — kernel-userspace communication, used by routing/ipsec/nf_tables |
| `can.c` | AF_CAN — SocketCAN protocol, RAW and BCM sockets |
| `tipc.c` | AF_TIPC — Transparent Inter-Process Communication, cluster-oriented messaging |
| `tun.c` | TUN/TAP — userspace packet injection, virtual Ethernet device |
| `veth.c` | VETH — virtual Ethernet pair for network namespaces |
| `vsock.c` | VSOCK — VM sockets for host-guest communication |
| `ipoib.c` | IPoIB — IP over InfiniBand |
| **Protocols & Services** | |
| `dhcp.c` | DHCP client — dynamic address configuration, lease management |
| `dns_cache.c` | DNS cache — local caching resolver, configurable TTL |
| `dns_server.c` | DNS server — authoritative DNS server built into kernel |
| `ntp.c` | NTP client — Network Time Protocol synchronization |
| `httpd.c` | HTTP server — simple embedded HTTP server for management |
| `smtp.c` | SMTP client — email sending capability |
| `sshd.c` | SSH daemon — secure shell server within kernel |
| `telnetd.c` | Telnet daemon — telnet server for remote console access |
| `rds.c` | RDS — Reliable Datagram Sockets |
| `igmp.c` | IGMP — Internet Group Management Protocol for multicast |
| `net_ns.c` | Network namespaces — per-namespace network stack isolation |
| `mptcp.c` | MPTCP — Multi-Path TCP subflows over multiple network paths |

## Key Conventions

- **Socket table:** 32 entries max, file descriptors start at offset 100.
  Protocol dispatch via `socket_ops` table keyed by address family.
- **TCP connection table:** 16 entries, indexed by a hash-like search.
  Full 11-state FIN/WAIT/TIME_WAIT machine with timer-driven retransmit.
- **ARP cache:** 16 entries with 300s timeout, 3-retry probe, pending
  resolution queue (8 frames max).
- **Netfilter:** Five hook points evaluated in order. Each hook fires
  priority-sorted handler chains. Connection tracking performs tuple-based
  lookup with protocol-specific state machines and timeouts.
- **Routing:** Static routing table with `RT_MAX_ENTRIES`. Forwarding
  controlled by `/proc/sys/net/ipv4/ip_forward`.
- **NAPI:** NIC drivers use NAPI polling for interrupt mitigation.
  RPS/RFS distribute packets across CPUs by flow hash.
