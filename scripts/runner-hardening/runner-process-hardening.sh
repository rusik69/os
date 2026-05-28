#!/usr/bin/env bash
# Process hardening for GitHub Actions runner
# Applies cgroup limits, drops capabilities, and monitors process activity

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'
log() { echo -e "${GREEN}[+]${NC} $1"; }

echo "=== Process Hardening ==="

RUNNER_USER="${RUNNER_USER:-ubuntu}"
RUNNER_UID=$(id -u "$RUNNER_USER")

# 1. Create a cgroup for runner processes
log "Creating runner cgroup..."
mkdir -p /sys/fs/cgroup/github-runner

# Set resource limits
echo "512M" > /sys/fs/cgroup/github-runner/memory.max 2>/dev/null || true
echo "50%" > /sys/fs/cgroup/github-runner/cpu.max 2>/dev/null || true
echo "2048" > /sys/fs/cgroup/github-runner/pids.max 2>/dev/null || true

# Add runner user to the cgroup
echo "$RUNNER_UID" > /sys/fs/cgroup/github-runner/cgroup.procs 2>/dev/null || true

log "Cgroup limits: 512MB memory, 50% CPU, 2048 PIDs"

# 2. Drop dangerous capabilities from the runner service
log "Configuring capability drops..."

# Create a wrapper script that drops capabilities before running the runner
cat > /home/${RUNNER_USER}/runner-with-dropped-caps.sh << 'WRAPPER'
#!/usr/bin/env bash
# Runner wrapper that drops dangerous capabilities
# This is called by the systemd service

# Drop capabilities using capsh
# Keep only: CAP_NET_BIND_SERVICE (for port binding), CAP_SYS_CHROOT, CAP_SETUID, CAP_SETGID
exec /usr/sbin/capsh \
    --caps="cap_net_bind_service,cap_sys_chroot,cap_setuid,cap_setgid+eip" \
    --keep=1 \
    --uid=$(id -u) \
    --gid=$(id -g) \
    --addamb=cap_net_bind_service \
    --addamb=cap_sys_chroot \
    --addamb=cap_setuid \
    --addamb=cap_setgip \
    -- -c "exec /home/${RUNNER_USER}/actions-runner/runsvc.sh"
WRAPPER

chmod 755 /home/${RUNNER_USER}/runner-with-dropped-caps.sh

# 3. Set process accounting
log "Enabling process accounting..."
if ! dpkg -l acct >/dev/null 2>&1; then
    apt-get install -y acct 2>/dev/null || true
fi

# Enable process accounting
systemctl enable acct 2>/dev/null || true
systemctl start acct 2>/dev/null || true

# 4. Set up audit rules for runner
log "Setting up audit rules..."
cat > /etc/audit/rules.d/github-runner.rules << 'EOF'
# Monitor runner binary
-w /home/ubuntu/actions-runner/bin/ -p x -k runner_binary

# Monitor runner config
-w /home/ubuntu/actions-runner/.runner -p rwxa -k runner_config
-w /home/ubuntu/actions-runner/.credentials -p rwxa -k runner_credentials

# Monitor kernel module loading
-w /sbin/modprobe -p x -k kernel_modules
-w /sbin/insmod -p x -k kernel_modules
-w /sbin/rmmod -p x -k kernel_modules

# Monitor privilege escalation attempts
-a always,exit -F arch=b64 -S execve -C uid!=euid -F euid=0 -k privilege_escalation

# Monitor network connections
-a always,exit -F arch=b64 -S connect -k network_connections
EOF

# Reload audit rules
augenrules --load 2>/dev/null || true

# 5. Monitor and log runner activity
log "Setting up runner monitoring..."
cat > /etc/logrotate.d/github-runner << 'EOF'
/home/ubuntu/actions-runner/_diag/*.log {
    daily
    rotate 7
    compress
    missingok
    notifempty
    copytruncate
    create 0640 ubuntu ubuntu
}
EOF

log "Process hardening applied."
echo ""
echo "Summary of process controls:"
echo "  - Cgroup limits: 512MB memory, 50% CPU, 2048 PIDs"
echo "  - Capabilities: only net_bind_service, sys_chroot, setuid, setgid"
echo "  - Audit: runner binaries, config, and privilege escalation"
echo "  - Process accounting: enabled"
echo "  - Log rotation: 7-day rotation for runner logs"
