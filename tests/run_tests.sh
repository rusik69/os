#!/bin/bash
# run_tests.sh — Boot test kernel in QEMU, capture output, validate results
#
# Usage:  ./run_tests.sh <kernel.bin> <disk.img>
#
# Launches QEMU with the test kernel, captures serial output,
# and checks for "ALL TESTS PASSED" or "SOME TESTS FAILED".
# Exits 0 on pass, 1 on failure, 124 on timeout.

set -euo pipefail

KERNEL="${1:?missing kernel.bin}"
DISK="${2:?missing disk.img}"
TIMEOUT=${TIMEOUT:-180}

SERIAL_LOG=$(mktemp /tmp/os-test-XXXXXX.txt)
trap 'rm -f "$SERIAL_LOG"' EXIT

echo "==> Booting test kernel (timeout=${TIMEOUT}s)..."

# Launch QEMU with -serial file: for clean capture
# Use -no-reboot so QEMU exits on triple-fault or ACPI shutdown
if ! timeout "$TIMEOUT" qemu-system-x86_64 \
    -kernel "$KERNEL" \
    -m 256M \
    -serial file:"$SERIAL_LOG" \
    -vga none \
    -display none \
    -drive file="$DISK",format=raw,if=ide \
    -netdev user,id=net0 -device e1000,netdev=net0 \
    -no-reboot 2>/dev/null; then
    # timeout or QEMU exit
    :
fi

echo "==> Checking results..."
if grep -q "ALL TESTS PASSED" "$SERIAL_LOG"; then
    PASS=$(grep -c "PASS" "$SERIAL_LOG" || true)
    FAIL=$(grep -c "FAIL" "$SERIAL_LOG" || true)
    echo "========================================"
    echo "  ALL TESTS PASSED  ($PASS passed)"
    echo "========================================"
    exit 0
elif grep -q "SOME TESTS FAILED" "$SERIAL_LOG"; then
    PASS=$(grep -c "PASS" "$SERIAL_LOG" || true)
    FAIL=$(grep -c "FAIL" "$SERIAL_LOG" || true)
    echo "========================================"
    echo "  SOME TESTS FAILED  ($PASS passed, $FAIL failed)"
    echo "========================================"
    grep "FAIL" "$SERIAL_LOG" || true
    exit 1
elif grep -q "\[\[TEST_DONE\]\]" "$SERIAL_LOG"; then
    echo "================================================"
    echo "  TESTS COMPLETED but no PASS/FAIL marker found"
    echo "================================================"
    PASS=$(grep -c "PASS" "$SERIAL_LOG" || true)
    FAIL=$(grep -c "FAIL" "$SERIAL_LOG" || true)
    echo "  $PASS passed, $FAIL failed"
    tail -20 "$SERIAL_LOG"
    exit 1
else
    echo "========================================"
    echo "  TESTS INCOMPLETE (timeout or crash)"
    echo "========================================"
    PASS=$(grep -c "PASS" "$SERIAL_LOG" || true)
    FAIL=$(grep -c "FAIL" "$SERIAL_LOG" || true)
    echo "  $PASS passed, $FAIL failed before timeout"
    tail -20 "$SERIAL_LOG"
    exit 124
fi
