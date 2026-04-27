#!/bin/sh
# tests/run_tests.sh — Run the OS kernel in QEMU (test mode) and report results.
#
# Usage: ./tests/run_tests.sh [kernel.bin] [disk.img]
# Defaults: build_test/kernel.bin  build/disk.img
#
# Exit codes:
#   0  ALL TESTS PASSED
#   1  test failures or suite did not complete
#   2  prerequisites missing

KERNEL="${1:-build_test/kernel.bin}"
DISK="${2:-build/disk.img}"
TIMEOUT="${TIMEOUT:-120}"

err() { echo "ERROR: $*" >&2; exit 2; }

# ── Prerequisites ─────────────────────────────────────────────────────────────
command -v qemu-system-x86_64 >/dev/null 2>&1 || \
    err "qemu-system-x86_64 not found. Install QEMU: brew install qemu"
[ -f "$KERNEL" ] || err "kernel not found at '$KERNEL'. Run: make test-kernel"
[ -f "$DISK"   ] || err "disk image not found at '$DISK'. Run: make"

# ── Run QEMU ──────────────────────────────────────────────────────────────────
TMP=$(mktemp /tmp/os_test_XXXXXX.txt)
trap 'rm -f "$TMP"' EXIT

echo "==> Booting $KERNEL in QEMU (timeout ${TIMEOUT}s)..."

# Use -serial file:... so output goes straight to TMP without TTY/printf issues.
# -no-reboot: QEMU exits when the kernel halts (after acpi_shutdown).
qemu-system-x86_64 \
    -kernel "$KERNEL" \
    -m 256M \
    -serial "file:$TMP" \
    -display none \
    -vga none \
    -drive "file=$DISK,format=raw,if=ide" \
    -netdev user,id=net0 \
    -device e1000,netdev=net0 \
    -no-reboot \
    2>/dev/null &
QEMU_PID=$!

# Wait for QEMU to exit or timeout (macOS-compatible poll loop, no 'timeout' cmd)
elapsed=0
while kill -0 "$QEMU_PID" 2>/dev/null; do
    sleep 1
    elapsed=$((elapsed + 1))
    if [ "$elapsed" -ge "$TIMEOUT" ]; then
        echo "TIMEOUT: QEMU still running after ${TIMEOUT}s, killing..." >&2
        kill "$QEMU_PID" 2>/dev/null || true
        break
    fi
done
wait "$QEMU_PID" 2>/dev/null || true

# ── Display serial output ─────────────────────────────────────────────────────
echo "--- Serial output ---"
cat "$TMP"
echo "--- End serial output ---"
echo ""

# ── Parse results ─────────────────────────────────────────────────────────────
PASS=$(grep -c "^\[PASS\]" "$TMP" 2>/dev/null || echo 0)
FAIL=$(grep -c "^\[FAIL\]" "$TMP" 2>/dev/null || echo 0)
echo "Results: $PASS passed, $FAIL failed"

if [ "$FAIL" -gt 0 ] 2>/dev/null; then
    echo ""
    echo "Failed tests:"
    grep "^\[FAIL\]" "$TMP" || true
fi

echo ""
if grep -q "ALL TESTS PASSED" "$TMP" 2>/dev/null; then
    echo "SUCCESS: All $PASS tests passed!"
    exit 0
elif grep -q "SOME TESTS FAILED" "$TMP" 2>/dev/null; then
    echo "FAILURE: $FAIL test(s) failed."
    exit 1
else
    echo "FAILURE: Test suite did not complete."
    echo "(Check for kernel boot errors above)"
    exit 1
fi
