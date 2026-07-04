# ── Network protocol modules — obj-m entries for net/ .ko files ─────────────
#
# This file is included by src/modules/Makefile.modules.
# All entries use obj-m += to append to the global module list.
#
# Categories:
#   Tunnels     — ipip, gre, vxlan, bridge
#   Transport   — sctp, dccp, mptcp
#   Security    — macsec, wireguard, ipsec, pfkey
#   L2/Control  — igmp, stp, lacp, garp, mrp, lldp
#   Higher/L4   — rds, vsock, openvswitch, xdp, can
#   Domain      — af_packet, af_unix, 6lowpan, ipoib
#   Netfilter   — netfilter, netfilter_hooks, nf_tables, conntrack, conntrack_helpers
#   DNS/QoS     — dns_cache, dns_server, pkt_sched, fq_codel, cake
#   TCP CC      — tcp_bbr2, tcp_bbr3
#   Other       — netlink, ntp, smtp, dhcp

# ── Network protocol modules (M60) ──────────────────────────────────────────
obj-m += net/ipip.ko
obj-m += net/gre.ko
obj-m += net/vxlan.ko
obj-m += net/bridge.ko

# ── Additional network protocol modules ─────────────────────────────────────
obj-m += net/sctp.ko
sctp-objs := net/sctp net/sctp_sm net/sctp_tsn
obj-m += net/dccp.ko
obj-m += net/mptcp.ko
obj-m += net/macsec.ko
obj-m += net/wireguard.ko
obj-m += net/ipsec.ko
obj-m += net/pfkey.ko
obj-m += net/igmp.ko
obj-m += net/stp.ko
obj-m += net/lacp.ko
obj-m += net/garp.ko
obj-m += net/mrp.ko
obj-m += net/lldp.ko
obj-m += net/rds.ko
obj-m += net/vsock.ko
obj-m += net/openvswitch.ko
obj-m += net/xdp.ko
obj-m += net/can.ko
obj-m += net/af_packet.ko
obj-m += net/af_unix.ko
obj-m += net/6lowpan.ko
obj-m += net/ipoib.ko

# ── Netfilter / conntrack ───────────────────────────────────────────────────
obj-m += net/netfilter.ko
obj-m += net/netfilter_hooks.ko
obj-m += net/nf_tables.ko
obj-m += net/conntrack.ko
obj-m += net/conntrack_helpers.ko

# ── DNS / QoS / Scheduling ──────────────────────────────────────────────────
obj-m += net/dns_cache.ko
obj-m += net/pkt_sched.ko
obj-m += net/fq_codel.ko
obj-m += net/cake.ko

# ── TCP congestion control ──────────────────────────────────────────────────
obj-m += net/tcp_bbr2.ko
obj-m += net/tcp_bbr3.ko

# ── Other net modules ───────────────────────────────────────────────────────
obj-m += net/netlink.ko
obj-m += net/dns_server.ko
obj-m += net/ntp.ko
obj-m += net/smtp.ko
obj-m += net/dhcp.ko
