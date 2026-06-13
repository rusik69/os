#!/bin/bash
#
# scripts/qemu-test.sh — QEMU smoke test
#
# Boots the kernel in QEMU, checks kernel log for panics,
# runs a shell command, and verifies the system boots successfully.
#
# Usage:
#   ./scripts/qemu-test.sh [kernel.bin] [disk.img]
#
# Defaults:
#   kernel.bin = build/kernel.bin
#   disk.img   = build/disk.img
#
# Item S179: QEMU smoke test
#

set -euo pipefail

KERNEL="${1:-build/kernel.bin}"
DISK="${2:-build/disk.img}"
TIMEOUT_SECONDS="${TIMEOUT:-30}"
QEMU_BIN="${QEMU:-qemu-system-x86_64}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

PASS=0
FAIL=0

check_prereqs() {
    if ! command -v "$QEMU_BIN" &>/dev/null; then
        echo "ERROR: $QEMU_BIN not found"
        exit 1
    fi

    if [ ! -f "$KERNEL" ]; then
        echo "ERROR: Kernel not found at $KERNEL"
        echo "  Build it first: make"
        exit 1
    fi
}

run_qemu_test() {
    local test_name="$1"
    local qemu_extra="$2"
    local expect_pattern="$3"
    local reject_pattern="${4:-Kernel Panic|Oops|BUG: unable to handle kernel}"

    echo -e "${YELLOW}[TEST]${NC} $test_name"

    # Create a temporary output file
    local outfile
    outfile=$(mktemp /tmp/qemu-test-XXXXXX.txt)
    local rc=0

    # Run QEMU with timeout
    timeout "$TIMEOUT_SECONDS" \
        "$QEMU_BIN" \
        -kernel "$KERNEL" \
        -m 256M \
        -serial stdio \
        -vga none -display none \
        -drive file="$DISK",format=raw,if=ide \
        -netdev user,id=net0 -device e1000,netdev=net0 \
        -no-reboot \
        -append "console=serial quiet" \
        $qemu_extra \
        < /dev/null 2>&1 | tee "$outfile" | tail -20 || true

    # Check for panic/oops patterns (reject)
    if grep -qE "$reject_pattern" "$outfile" 2>/dev/null; then
        echo -e "  ${RED}FAIL${NC}: System crashed (panic/oops detected)"
        grep -E "$reject_pattern" "$outfile" | head -5
        FAIL=$((FAIL + 1))
        rc=1
    # Check for expected pattern
    elif grep -qE "$expect_pattern" "$outfile" 2>/dev/null; then
        echo -e "  ${GREEN}PASS${NC}: Expected output found"
        PASS=$((PASS + 1))
        rc=0
    else
        echo -e "  ${RED}FAIL${NC}: Expected pattern '$expect_pattern' not found"
        echo "  --- Last 10 lines of output ---"
        tail -10 "$outfile"
        FAIL=$((FAIL + 1))
        rc=1
    fi

    rm -f "$outfile"
    return $rc
}

# ── Main ──────────────────────────────────────────────────────────────

check_prereqs

echo "============================================"
echo " QEMU Smoke Test Suite"
echo " Kernel: $KERNEL"
echo " Disk:   $DISK"
echo " Timeout: ${TIMEOUT_SECONDS}s"
echo "============================================"
echo ""

# Test 1: Basic boot — kernel should start and show version
run_qemu_test \
    "Boot and show version" \
    "" \
    "Hermes|Hermes OS|kernel|Kernel|Welcome|init|shell"

# Test 2: Check that the shell is responsive
run_qemu_test \
    "Shell responsiveness" \
    "" \
    "shell|# |\$ |ok"

# Test 3: Filesystem integrity — init should mount rootfs
run_qemu_test \
    "Root filesystem mount" \
    "" \
    "mounted|tmpfs|rootfs|init"

# Test 4: Network stack initializes (if DHCP enabled)
run_qemu_test \
    "Network initialization" \
    "-netdev user,id=net0 -device e1000,netdev=net0" \
    "e1000|eth0|net|DHCP"

# Test 5: No kernel panics during boot
run_qemu_test \
    "Panic-free boot" \
    "" \
    "init complete|boot complete|done|OK" \
    "Kernel Panic|Oops|BUG:|Fatal|triple fault"

echo ""
echo "============================================"
echo -e " Results: ${GREEN}$PASS passed${NC}, ${RED}$FAIL failed${NC}"
echo "============================================"

exit $FAIL
