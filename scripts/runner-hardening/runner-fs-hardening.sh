#!/usr/bin/env bash
# Filesystem hardening for GitHub Actions runner
# Protects runner binaries, config, and system files

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'
log() { echo -e "${GREEN}[+]${NC} $1"; }

echo "=== Filesystem Hardening ==="

RUNNER_DIR="${RUNNER_DIR:-/home/ubuntu/actions-runner}"
RUNNER_USER="${RUNNER_USER:-ubuntu}"

# 1. Lock down runner binary directory
log "Locking down runner binary directory..."
chmod 755 "$RUNNER_DIR"
chmod 700 "$RUNNER_DIR/.credentials"
chmod 600 "$RUNNER_DIR/.runner" 2>/dev/null || true
chmod 600 "$RUNNER_DIR/.credentials_rsaparams" 2>/dev/null || true

# Make runner config files immutable (prevent tampering)
chattr +i "$RUNNER_DIR/.runner" 2>/dev/null || true
chattr +i "$RUNNER_DIR/.credentials" 2>/dev/null || true

# 2. Remove SUID/SGID binaries from runner work dir after each run
log "Setting up SUID cleanup on work dirs..."
# This is handled by the runner service hook in runner-service-hardening.sh

# 3. Create a restrictive umask for the runner
log "Setting runner umask..."
# Add to runner's shell profile
cat >> /home/${RUNNER_USER}/.bashrc << 'EOF'

# Runner security: restrictive umask
umask 0077
EOF

# 4. Restrict /proc/sys access (prevent info leaks)
log "Restricting /proc/sys access..."
chmod 700 /proc/sys
chmod 700 /proc/sys/kernel
chmod 700 /proc/sys/net
chmod 700 /proc/sys/vm

# 5. Create tmpfs mounts for sensitive directories
log "Setting up tmpfs mounts..."
# Mount /tmp with noexec
if ! mount | grep -q "on /tmp .* tmpfs"; then
    mount -t tmpfs -o size=512M,noexec,nosuid,nodev tmpfs /tmp 2>/dev/null || true
fi

# 6. Remove unnecessary kernel modules
log "Blacklisting unnecessary kernel modules..."
cat > /etc/modprobe.d/github-runner-hardening.conf << 'EOF'
# Blacklist modules that could be exploited
blacklist cramfs
blacklist freevxfs
blacklist hfs
blacklist hfsplus
blacklist udf
blacklist dccp
blacklist sctp
blacklist rds
blacklist tipc
EOF

log "Filesystem hardening applied."
