#!/bin/sh
# tests/e2e.sh — Boot OS kernel in QEMU (user-mode networking) and run e2e
#                shell tests via telnet.  Invoked by: make e2e
#
# Usage:
#   ./tests/e2e.sh [kernel.bin] [disk.img]
#   Defaults: build/kernel.bin  build/disk.img
#
# Environment:
#   E2E_HOST         telnet host    (default 127.0.0.1)
#   E2E_PORT         host telnet port (auto-selected if not set)
#   E2E_TIMEOUT      per-command timeout seconds  (default 30)
#   E2E_BOOT_TIMEOUT boot + DHCP timeout seconds  (default 90)
#
# Exit codes:
#   0  all e2e tests passed
#   1  one or more tests failed
#   2  prerequisites missing or QEMU failed to start

KERNEL="${1:-build/kernel.bin}"
DISK="${2:-build/disk.img}"
E2E_HOST="${E2E_HOST:-127.0.0.1}"

# Pick a free port in a stable range (not too high, not conflicting with common services)
if [ -z "$E2E_PORT" ]; then
    # Try ports in 12300-12399 range; pick first free one
    for _p in 12323 12324 12325 12326 12327 12328; do
        if command -v lsof >/dev/null 2>&1; then
            if ! lsof -i ":${_p}" >/dev/null 2>&1; then
                E2E_PORT=$_p
                break
            fi
        elif command -v ss >/dev/null 2>&1; then
            if ! ss -tlnp | grep -q ":${_p} " 2>/dev/null; then
                E2E_PORT=$_p
                break
            fi
        else
            E2E_PORT=$_p
            break
        fi
    done
    [ -z "$E2E_PORT" ] && E2E_PORT=12323
fi

err() { echo "ERROR: $*" >&2; exit 2; }

# ── Prerequisites ─────────────────────────────────────────────────────────────
command -v qemu-system-x86_64 >/dev/null 2>&1 || \
    err "qemu-system-x86_64 not found. Install QEMU: brew install qemu / apt install qemu-system-x86"
command -v python3 >/dev/null 2>&1 || \
    err "python3 not found. Install: brew install python / apt install python3"
[ -f "$KERNEL" ] || err "kernel not found at '$KERNEL'. Run: make"
[ -f "$DISK"   ] || err "disk image not found at '$DISK'. Run: make"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── Serial log ────────────────────────────────────────────────────────────────
SERIAL_LOG=$(mktemp /tmp/os_e2e_serial_XXXXXX)
trap 'rm -f "$SERIAL_LOG"; kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null' EXIT

# ── Launch QEMU ───────────────────────────────────────────────────────────────
echo "==> Starting QEMU kernel=$KERNEL port=${E2E_PORT}->23 ..."

qemu-system-x86_64 \
    -kernel "$KERNEL" \
    -m 256M \
    -serial "file:$SERIAL_LOG" \
    -display none \
    -vga none \
    -drive "file=$DISK,format=raw,if=ide" \
    -netdev "user,id=net0,hostfwd=tcp::${E2E_PORT}-:23" \
    -device e1000,netdev=net0 \
    -no-reboot \
    2>/dev/null &
QEMU_PID=$!

# ── Wait for kernel to reach the telnet server ────────────────────────────────
BOOT_WAIT="${E2E_BOOT_TIMEOUT:-90}"
echo "==> Waiting up to ${BOOT_WAIT}s for telnet server..."
elapsed=0
while [ "$elapsed" -lt "$BOOT_WAIT" ]; do
    # Check QEMU is still alive
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "ERROR: QEMU exited unexpectedly" >&2
        echo "--- Serial log ---"
        cat "$SERIAL_LOG"
        exit 2
    fi
    if grep -q "Telnet server on port 23" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done

if ! grep -q "Telnet server on port 23" "$SERIAL_LOG" 2>/dev/null; then
    echo "ERROR: Kernel did not reach telnet server in ${BOOT_WAIT}s" >&2
    echo "--- Serial log ---"
    cat "$SERIAL_LOG"
    exit 2
fi

echo "==> Kernel booted. Waiting 3s for scheduler to start..."
sleep 3
echo "==> Running e2e tests..."
echo ""

# ── Run e2e Python test suite ─────────────────────────────────────────────────
export E2E_HOST E2E_PORT
python3 -u "$SCRIPT_DIR/e2e.py"
RESULT=$?

echo ""
echo "--- Serial log (last 40 lines) ---"
tail -40 "$SERIAL_LOG"

exit $RESULT
