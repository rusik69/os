#!/usr/bin/env bash
# Verify runner security hardening
# Run as root: sudo bash scripts/runner-hardening/runner-verify.sh

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass=0
fail=0
warn=0

check_pass() { echo -e "  ${GREEN}[PASS]${NC} $1"; ((pass++)); }
check_fail() { echo -e "  ${RED}[FAIL]${NC} $1"; ((fail++)); }
check_warn() { echo -e "  ${YELLOW}[WARN]${NC} $1"; ((warn++)); }

echo "=== Runner Security Verification ==="
echo ""

echo "--- Network Isolation ---"
if iptables -L OUTPUT -n 2>/dev/null | grep -q "DROP"; then
    check_pass "iptables OUTPUT chain has DROP policy"
else
    check_fail "iptables OUTPUT chain does NOT have DROP policy"
fi

if ip6tables -L OUTPUT -n 2>/dev/null | grep -q "DROP"; then
    check_pass "ip6tables OUTPUT chain has DROP policy"
else
    check_fail "ip6tables OUTPUT chain does NOT have DROP policy"
fi

# Check DNS is allowed
if iptables -L OUTPUT -n 2>/dev/null | grep -q "dpt:53"; then
    check_pass "DNS (port 53) is allowed"
else
    check_warn "DNS may not be allowed"
fi

echo ""
echo "--- Filesystem Hardening ---"
RUNNER_DIR="${RUNNER_DIR:-/home/ubuntu/actions-runner}"

if [[ -d "$RUNNER_DIR/.credentials" ]]; then
    perms=$(stat -c %a "$RUNNER_DIR/.credentials")
    if [[ "$perms" == "700" ]]; then
        check_pass ".credentials directory is 700"
    else
        check_warn ".credentials directory is $perms (should be 700)"
    fi
else
    check_warn ".credentials directory not found"
fi

if [[ -f "$RUNNER_DIR/.runner" ]]; then
    if lsattr "$RUNNER_DIR/.runner" 2>/dev/null | grep -q "i"; then
        check_pass ".runner file has immutable attribute"
    else
        check_warn ".runner file does NOT have immutable attribute"
    fi
fi

if mount | grep -q "on /tmp .* tmpfs"; then
    check_pass "/tmp is mounted as tmpfs"
else
    check_warn "/tmp is not mounted as tmpfs"
fi

if [[ -f /etc/modprobe.d/github-runner-hardening.conf ]]; then
    check_pass "Kernel module blacklist exists"
else
    check_warn "Kernel module blacklist not found"
fi

echo ""
echo "--- Process Hardening ---"
if [[ -d /sys/fs/cgroup/github-runner ]]; then
    check_pass "Runner cgroup exists"
    if [[ -f /sys/fs/cgroup/github-runner/memory.max ]]; then
        mem=$(cat /sys/fs/cgroup/github-runner/memory.max)
        check_pass "Memory limit: $mem"
    fi
    if [[ -f /sys/fs/cgroup/github-runner/pids.max ]]; then
        pids=$(cat /sys/fs/cgroup/github-runner/pids.max)
        check_pass "PID limit: $pids"
    fi
else
    check_warn "Runner cgroup not found (may need systemd reboot)"
fi

if systemctl is-active --quiet auditd 2>/dev/null; then
    check_pass "Audit daemon is running"
else
    check_warn "Audit daemon is not running"
fi

if [[ -f /etc/audit/rules.d/github-runner.rules ]]; then
    check_pass "Audit rules for runner exist"
else
    check_warn "Audit rules for runner not found"
fi

if systemctl is-active --quiet acct 2>/dev/null; then
    check_pass "Process accounting is active"
else
    check_warn "Process accounting is not active"
fi

echo ""
echo "--- Runner Status ---"
if systemctl is-active --quiet actions.runner.rusik69-os.aws-selfhosted.service 2>/dev/null; then
    check_pass "Runner service is active"
else
    check_warn "Runner service is not active"
fi

echo ""
echo "=== Results ==="
echo -e "  ${GREEN}Passed: $pass${NC}"
echo -e "  ${YELLOW}Warnings: $warn${NC}"
echo -e "  ${RED}Failed: $fail${NC}"
echo ""

if [[ $fail -gt 0 ]]; then
    echo -e "${RED}Some checks failed. Review the output above.${NC}"
    exit 1
elif [[ $warn -gt 0 ]]; then
    echo -e "${YELLOW}Some checks have warnings. Review the output above.${NC}"
    exit 0
else
    echo -e "${GREEN}All checks passed!${NC}"
    exit 0
fi
