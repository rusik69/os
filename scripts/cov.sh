#!/bin/bash
#
# scripts/cov.sh — Kernel code coverage collection
#
# Uses GCC's gcov kernel integration to collect and report code
# coverage data from a kernel built with CONFIG_GCOV_KERNEL=y.
#
# Usage:
#   ./scripts/cov.sh [--build] [--run] [--report] [--clean]
#
# Options:
#   --build      Build kernel with coverage flags
#   --run        Boot in QEMU and collect coverage data
#   --report     Generate HTML coverage report with gcov/lcov
#   --clean      Remove coverage artifacts
#   --all        Do all steps (default)
#
# Environment:
#   KERNEL       Path to kernel binary (default: build/kernel.bin)
#   OUTDIR       Coverage output directory (default: build/cov)
#   QEMU         QEMU binary (default: qemu-system-x86_64)
#
# Item S180: Coverage collection
#

set -euo pipefail

# ── Configuration ────────────────────────────────────────────────────

KERNEL="${KERNEL:-build/kernel.bin}"
OUTDIR="${OUTDIR:-build/cov}"
QEMU="${QEMU:-qemu-system-x86_64}"
COV_DISK="build/cov_disk.img"
GCOV_DIR="/sys/kernel/debug/gcov"
MOUNT_POINT="/tmp/cov_mount"

# Coverage flags for GCC
COV_CFLAGS="-fprofile-arcs -ftest-coverage"

# ── Helper functions ─────────────────────────────────────────────────

info()  { echo -e "\033[1;34m[COV]\033[0m $*"; }
ok()    { echo -e "\033[1;32m[OK]\033[0m $*"; }
err()   { echo -e "\033[1;31m[ERR]\033[0m $*" >&2; }

cleanup() {
    info "Cleaning up..."
    rm -rf "$MOUNT_POINT" "$COV_DISK" 2>/dev/null || true
    rm -f /tmp/cov_*.txt 2>/dev/null || true
}

# ── Build with coverage ──────────────────────────────────────────────

do_build() {
    info "Building kernel with coverage flags..."
    info "Extra CFLAGS: $COV_CFLAGS"

    mkdir -p "$OUTDIR"

    # Build with coverage instrumentation
    make CFLAGS_EXTRA="$COV_CFLAGS" -j$(nproc) 2>&1 | tail -5

    if [ -f "$KERNEL" ]; then
        ok "Kernel built with coverage: $KERNEL"
    else
        err "Kernel build failed"
        exit 1
    fi
}

# ── Run in QEMU and collect coverage data ────────────────────────────

do_run() {
    info "Booting kernel in QEMU to collect coverage..."

    if [ ! -f "$KERNEL" ]; then
        err "Kernel not found. Run --build first."
        exit 1
    fi

    # Create a small disk image for coverage data output
    dd if=/dev/zero of="$COV_DISK" bs=1M count=4 2>/dev/null

    # Create mount point
    mkdir -p "$MOUNT_POINT"

    # Run QEMU with serial output captured
    info "Starting QEMU (timeout: 30s)..."
    local qemu_out
    qemu_out=$(mktemp /tmp/cov_qemu_XXXXXX.txt)

    timeout 30 \
        "$QEMU" \
        -kernel "$KERNEL" \
        -m 256M \
        -serial stdio \
        -vga none -display none \
        -drive file="$COV_DISK",format=raw,if=ide \
        -no-reboot \
        -append "console=serial quiet coverage=1" \
        2>&1 | tee "$qemu_out" | tail -5 || true

    info "QEMU finished. Checking output..."

    # Check for panic
    if grep -qi "panic\|oops\|BUG:" "$qemu_out"; then
        err "Kernel panic detected during coverage run"
        grep -i "panic\|oops\|BUG:" "$qemu_out"
        rm -f "$qemu_out"
        exit 1
    fi

    # Extract .gcda file list from kernel log (if gcov dumped them)
    grep "gcov:" "$qemu_out" 2>/dev/null | tee "$OUTDIR/gcov_files.txt" || true

    rm -f "$qemu_out"
    ok "Coverage data collected"
}

# ── Generate report ──────────────────────────────────────────────────

do_report() {
    info "Generating coverage report..."

    mkdir -p "$OUTDIR/html"

    # Use gcov to process coverage data
    if command -v gcov &>/dev/null; then
        info "Running gcov on source files..."

        # Find all .gcda files (coverage data)
        local gcda_files
        gcda_files=$(find "$OUTDIR" -name "*.gcda" 2>/dev/null || true)

        if [ -n "$gcda_files" ]; then
            for gcda in $gcda_files; do
                local src_dir
                src_dir=$(dirname "$gcda")
                pushd "$src_dir" >/dev/null
                gcov -o . "$(basename "$gcda" .gcda).c" 2>/dev/null || true
                popd >/dev/null
            done
            ok "gcov processing complete"
        else
            info "No .gcda files found (kernel may not have been run with coverage)"
        fi
    else
        info "gcov not found — install it (apt install gcov)"
    fi

    # Generate HTML report with lcov (if available)
    if command -v lcov &>/dev/null; then
        info "Generating HTML report with lcov..."

        local tracefile="$OUTDIR/coverage.info"

        # Capture coverage data
        lcov --capture --directory . \
            --output-file "$tracefile" \
            --rc lcov_branch_coverage=1 \
            2>/dev/null || true

        if [ -f "$tracefile" ]; then
            # Generate HTML
            genhtml "$tracefile" \
                --output-directory "$OUTDIR/html" \
                --rc lcov_branch_coverage=1 \
                --title "Hermes OS Kernel Coverage" \
                2>/dev/null || true

            if [ -f "$OUTDIR/html/index.html" ]; then
                ok "HTML report: file://$OUTDIR/html/index.html"
            fi
        fi
    else
        info "lcov not found — install it (apt install lcov)"
        info "Falling back to simple gcov text summary..."

        # Simple text summary using gcov
        find . -name "*.c.gcov" -o -name "*.h.gcov" 2>/dev/null | \
            while read -r f; do
                local total=0
                local executed=0
                total=$(grep -c '^        [0-9]' "$f" 2>/dev/null || echo 0)
                executed=$(grep -c '^    [1-9][0-9]*' "$f" 2>/dev/null || echo 0)
                if [ "$total" -gt 0 ]; then
                    local pct=$((executed * 100 / total))
                    echo "  $f: $pct% covered ($executed/$total lines)"
                fi
            done > "$OUTDIR/summary.txt"

        if [ -f "$OUTDIR/summary.txt" ]; then
            ok "Text summary: $OUTDIR/summary.txt"
            echo ""
            echo "=== Coverage Summary ==="
            cat "$OUTDIR/summary.txt"
        fi
    fi
}

# ── Clean coverage artifacts ─────────────────────────────────────────

do_clean() {
    info "Cleaning coverage artifacts..."

    # Remove coverage data files
    find . -name "*.gcda" -o -name "*.gcno" -o -name "*.gcov" | \
        while read -r f; do rm -f "$f"; done

    # Remove output directory
    rm -rf "$OUTDIR"
    rm -f "$COV_DISK"
    rm -f /tmp/cov_*.txt

    ok "Coverage artifacts cleaned"
}

# ── Main ─────────────────────────────────────────────────────────────

# Default: do all steps
if [ $# -eq 0 ]; then
    set -- --all
fi

for arg in "$@"; do
    case "$arg" in
        --build)
            do_build
            ;;
        --run)
            do_run
            ;;
        --report)
            do_report
            ;;
        --clean)
            do_clean
            ;;
        --all)
            do_build
            do_run
            do_report
            ;;
        --help|-h)
            echo "Usage: $0 [--build] [--run] [--report] [--clean] [--all]"
            echo ""
            echo "  --build    Build kernel with GCOV coverage flags"
            echo "  --run      Boot kernel in QEMU and collect coverage data"
            echo "  --report   Generate coverage report (HTML or text)"
            echo "  --clean    Remove coverage artifacts"
            echo "  --all      Do all steps (default)"
            exit 0
            ;;
        *)
            err "Unknown option: $arg"
            exit 1
            ;;
    esac
done

cleanup
ok "Coverage collection complete"
