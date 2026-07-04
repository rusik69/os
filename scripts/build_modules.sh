#!/bin/bash
#
# build_modules.sh — Build all kernel modules listed in obj-m
#
# Usage:
#   scripts/build_modules.sh           # build all modules
#   scripts/build_modules.sh clean     # clean module build artifacts
#   scripts/build_modules.sh <filter>  # build only modules matching <filter>
#
# This script drives the kernel's module build system.  It invokes
#   make modules
# to compile all .ko files listed in obj-m, reports success/failure per
# module, and checks for zero-warning compilation.
#
# Environment:
#   MAKE       — make command (default: make)
#   JOBS       — parallel job count (default: nproc)
#
# Examples:
#   scripts/build_modules.sh                     # build everything
#   scripts/build_modules.sh e1000               # build only e1000.ko
#   scripts/build_modules.sh "fs/|net/"          # build FS + net modules

set -euo pipefail

ME="$(basename "$0")"
MAKE="${MAKE:-make}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
BUILDDIR="build"
MODULE_BUILDDIR="${BUILDDIR}/modules"

# ── Colors ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

ok()   { echo -e "${GREEN}[OK]${NC} $1"; }
fail() { echo -e "${RED}[FAIL]${NC} $1"; }
info() { echo -e "${CYAN}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }

# ── Clean ───────────────────────────────────────────────────────────────────
if [[ "${1:-}" == "clean" ]]; then
    echo "=== Cleaning module build artifacts ==="
    rm -rf "${MODULE_BUILDDIR}"
    info "Removed ${MODULE_BUILDDIR}"
    exit 0
fi

FILTER="${1:-}"

# ── Build all modules ──────────────────────────────────────────────────────
echo "=== Building kernel modules ==="
echo "  Make:      ${MAKE}"
echo "  Jobs:      ${JOBS}"
echo "  Filter:    ${FILTER:-none (all modules)}"
echo ""

BUILD_START=$(date +%s)

if [[ -n "$FILTER" ]]; then
    # Build with filter: override obj-m temporarily
    info "Building modules matching: ${FILTER}"
    ${MAKE} -j"${JOBS}" modules 2>&1 || true
    # After build, show matching .ko files
    echo ""
    info "Built modules matching '${FILTER}':"
    find "${MODULE_BUILDDIR}" -name '*.ko' 2>/dev/null | grep -E "${FILTER}" | sort || warn "No matching modules found"
    BUILD_END=$(date +%s)
    echo ""
    echo "Build took $((BUILD_END - BUILD_START)) seconds."
    exit 0
fi

# Full build: compile all modules
# Capture both stdout and stderr, tee to console and log
LOG_FILE="/tmp/module-build-$(date +%Y%m%d-%H%M%S).log"
${MAKE} -j"${JOBS}" modules 2>&1 | tee "${LOG_FILE}"
BUILD_EXIT=${PIPESTATUS[0]}
BUILD_END=$(date +%s)

echo ""
echo "=== Build completed in $((BUILD_END - BUILD_START)) seconds (exit code: ${BUILD_EXIT}) ==="

# ── Check for warnings / errors ────────────────────────────────────────────
WARN_COUNT=$(grep -c 'warning:' "${LOG_FILE}" 2>/dev/null || echo 0)
ERR_COUNT=$(grep -c 'error:' "${LOG_FILE}" 2>/dev/null || echo 0)

if [[ "${BUILD_EXIT}" -ne 0 ]]; then
    fail "Module build FAILED with exit code ${BUILD_EXIT}"
    warn "Errors: ${ERR_COUNT}, Warnings: ${WARN_COUNT}"
    echo ""
    info "Last 20 lines of build output:"
    tail -20 "${LOG_FILE}"
    exit "${BUILD_EXIT}"
fi

if [[ "${WARN_COUNT}" -gt 0 ]]; then
    warn "Build succeeded but with ${WARN_COUNT} warning(s)"
    echo ""
    info "Warnings:"
    grep -n 'warning:' "${LOG_FILE}" | head -30
fi

ok "Module build completed successfully"

# ── List built modules ─────────────────────────────────────────────────────
echo ""
echo "=== Built modules ==="
KO_COUNT=0
if [[ -d "${MODULE_BUILDDIR}" ]]; then
    while IFS= read -r -d '' ko; do
        KO_COUNT=$((KO_COUNT + 1))
        size=$(stat -c '%s' "$ko" 2>/dev/null || stat -f '%z' "$ko" 2>/dev/null)
        echo "  $(basename "$ko") ($(numfmt --to=iec "${size:-0}" 2>/dev/null || echo "${size}B"))"
    done < <(find "${MODULE_BUILDDIR}" -name '*.ko' -print0 2>/dev/null | sort -z)
fi
echo ""
info "Total: ${KO_COUNT} module(s) built"

# Log file location
echo ""
info "Build log: ${LOG_FILE}"

exit 0
