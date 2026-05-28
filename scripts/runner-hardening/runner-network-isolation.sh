#!/usr/bin/env bash
# Network isolation for GitHub Actions runner
# Blocks all outbound traffic except GitHub APIs, apt mirrors, and DNS

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'
log() { echo -e "${GREEN}[+]${NC} $1"; }

echo "=== Network Isolation ==="

# Flush existing rules
iptables -F OUTPUT 2>/dev/null || true
ip6tables -F OUTPUT 2>/dev/null || true

# Default: DROP all outbound
iptables -P OUTPUT DROP
ip6tables -P OUTPUT DROP

# Allow loopback
iptables -A OUTPUT -o lo -j ACCEPT
ip6tables -A OUTPUT -o lo -j ACCEPT

# Allow established connections (return traffic)
iptables -A OUTPUT -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
ip6tables -A OUTPUT -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT

# Allow DNS (UDP/TCP port 53)
iptables -A OUTPUT -p udp --dport 53 -j ACCEPT
iptables -A OUTPUT -p tcp --dport 53 -j ACCEPT
ip6tables -A OUTPUT -p udp --dport 53 -j ACCEPT
ip6tables -A OUTPUT -p tcp --dport 53 -j ACCEPT

# Allow HTTPS to GitHub
iptables -A OUTPUT -p tcp -d 140.82.112.0/20 --dport 443 -j ACCEPT
iptables -A OUTPUT -p tcp -d 185.199.108.0/22 --dport 443 -j ACCEPT
iptables -A OUTPUT -p tcp -d 192.30.252.0/22 --dport 443 -j ACCEPT
iptables -A OUTPUT -p tcp -d 140.82.112.0/20 --dport 22 -j ACCEPT
ip6tables -A OUTPUT -p tcp -d 2606:4700::/32 --dport 443 -j ACCEPT
ip6tables -A OUTPUT -p tcp -d 2a04:4e42::/32 --dport 443 -j ACCEPT

# Allow HTTPS to apt mirrors (Ubuntu)
iptables -A OUTPUT -p tcp -d 185.125.190.0/24 --dport 443 -j ACCEPT
iptables -A OUTPUT -p tcp -d 91.189.91.0/24 --dport 443 -j ACCEPT
iptables -A OUTPUT -p tcp -d 185.125.190.36 --dport 443 -j ACCEPT

# Allow HTTPS to GitHub Actions artifact storage (required for cache)
iptables -A OUTPUT -p tcp -d 140.82.114.0/20 --dport 443 -j ACCEPT

# Allow HTTPS to GitHub Actions API (runner registration, job polling)
iptables -A OUTPUT -p tcp -d 140.82.112.0/20 --dport 443 -j ACCEPT
iptables -A OUTPUT -p tcp -d 185.199.108.0/22 --dport 443 -j ACCEPT

# Allow HTTPS to ccache / GitHub
iptables -A OUTPUT -p tcp -d 140.82.112.0/20 --dport 443 -j ACCEPT

# Block outbound ICMP (except allow from established)
iptables -A OUTPUT -p icmp --icmp-type echo-request -j DROP
iptables -A OUTPUT -p icmp --icmp-type echo-reply -j ACCEPT

# Log blocked attempts (rate limited)
iptables -A OUTPUT -m limit --limit 5/min -j LOG --log-prefix "IPT-DROP: " --log-level 4

# Show rules
log "Applied iptables rules:"
iptables -L OUTPUT -n --line-numbers
echo ""

log "Network isolation applied."
