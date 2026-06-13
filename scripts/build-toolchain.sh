#!/bin/bash
#
# scripts/build-toolchain.sh — Cross-compiler bootstrap
#
# Downloads and builds an x86_64-elf cross-compiler toolchain (GCC + binutils)
# targeting the Hermes OS kernel.  The resulting toolchain is installed
# to a local directory and does NOT affect the system's native toolchain.
#
# This script builds:
#   - binutils-2.42 (assembler, linker, objcopy, etc.)
#   - gcc-13.2.0  (C cross-compiler for x86_64-elf)
#
# Prerequisites:
#   build-essential, flex, bison, libgmp-dev, libmpfr-dev, libmpc-dev,
#   texinfo, wget, xz-utils
#
# Usage:
#   ./scripts/build-toolchain.sh [--prefix=PATH] [--jobs=N]
#
# Defaults:
#   --prefix  /usr/local/x86_64-elf
#   --jobs    (number of CPU cores)
#
# Item S200: Cross-compiler bootstrap
#

set -euo pipefail

# ── Configuration ────────────────────────────────────────────────────

PREFIX="${PREFIX:-/usr/local/x86_64-elf}"
TARGET="x86_64-elf"
NPROCS="${NPROCS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)}"

BINUTILS_VERSION="2.42"
GCC_VERSION="13.2.0"

BINUTILS_URL="https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VERSION}.tar.xz"
GCC_URL="https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz"

BUILD_DIR="/tmp/cross-toolchain-build"
SOURCES_DIR="${BUILD_DIR}/sources"
INSTALL_DIR="${PREFIX}"

# ── Colors ────────────────────────────────────────────────────────────

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

info()  { echo -e "${BLUE}[INFO]${NC} $*"; }
ok()    { echo -e "${GREEN}[OK]${NC} $*"; }
err()   { echo -e "${RED}[ERR]${NC} $*" >&2; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }

# ── Parse arguments ──────────────────────────────────────────────────

for arg in "$@"; do
    case "$arg" in
        --prefix=*)
            PREFIX="${arg#*=}"
            INSTALL_DIR="$PREFIX"
            ;;
        --jobs=*)
            NPROCS="${arg#*=}"
            ;;
        --help|-h)
            echo "Usage: $0 [--prefix=PATH] [--jobs=N]"
            echo ""
            echo "  --prefix=PATH   Install prefix (default: /usr/local/x86_64-elf)"
            echo "  --jobs=N        Parallel build jobs (default: # of CPUs)"
            echo ""
            echo "This script builds an x86_64-elf cross-compiler toolchain."
            exit 0
            ;;
        *)
            err "Unknown option: $arg"
            exit 1
            ;;
    esac
done

# ── Prerequisites check ──────────────────────────────────────────────

check_prereqs() {
    info "Checking prerequisites..."

    local missing=0
    for cmd in gcc g++ make flex bison wget xz; do
        if ! command -v "$cmd" &>/dev/null; then
            err "Missing: $cmd"
            missing=1
        fi
    done

    # Check for development libraries
    for lib in gmp mpfr mpc; do
        if ! ldconfig -p 2>/dev/null | grep -q "lib${lib}"; then
            warn "Library lib${lib} not found via ldconfig — may be missing"
        fi
    done

    if [ "$missing" -eq 1 ]; then
        err "Install prerequisites:"
        err "  sudo apt install build-essential flex bison libgmp-dev \\"
        err "       libmpfr-dev libmpc-dev texinfo wget xz-utils"
        exit 1
    fi

    ok "Prerequisites found"
}

# ── Download sources ─────────────────────────────────────────────────

download_sources() {
    info "Downloading sources..."

    mkdir -p "$SOURCES_DIR"
    cd "$SOURCES_DIR"

    if [ ! -f "binutils-${BINUTILS_VERSION}.tar.xz" ]; then
        info "Downloading binutils-${BINUTILS_VERSION}..."
        wget -q --show-progress "$BINUTILS_URL"
    else
        ok "binutils-${BINUTILS_VERSION}.tar.xz already downloaded"
    fi

    if [ ! -f "gcc-${GCC_VERSION}.tar.xz" ]; then
        info "Downloading gcc-${GCC_VERSION}..."
        wget -q --show-progress "$GCC_URL"
    else
        ok "gcc-${GCC_VERSION}.tar.xz already downloaded"
    fi
}

# ── Extract sources ──────────────────────────────────────────────────

extract_sources() {
    info "Extracting sources..."

    cd "$SOURCES_DIR"

    if [ ! -d "binutils-${BINUTILS_VERSION}" ]; then
        info "Extracting binutils..."
        tar -xf "binutils-${BINUTILS_VERSION}.tar.xz"
        ok "binutils extracted"
    else
        ok "binutils already extracted"
    fi

    if [ ! -d "gcc-${GCC_VERSION}" ]; then
        info "Extracting gcc..."
        tar -xf "gcc-${GCC_VERSION}.tar.xz"
        ok "gcc extracted"
    else
        ok "gcc already extracted"
    fi
}

# ── Build binutils ──────────────────────────────────────────────────

build_binutils() {
    info "Building binutils-${BINUTILS_VERSION}..."
    info "  Target:   ${TARGET}"
    info "  Prefix:   ${INSTALL_DIR}"
    info "  Jobs:     ${NPROCS}"

    local BUILD_DIR_BINUTILS="${BUILD_DIR}/build-binutils"
    mkdir -p "$BUILD_DIR_BINUTILS"
    cd "$BUILD_DIR_BINUTILS"

    if [ -f "${INSTALL_DIR}/bin/${TARGET}-ld" ]; then
        ok "binutils already installed, skipping"
        return
    fi

    "${SOURCES_DIR}/binutils-${BINUTILS_VERSION}/configure" \
        --target="${TARGET}" \
        --prefix="${INSTALL_DIR}" \
        --with-sysroot \
        --disable-nls \
        --disable-werror \
        --enable-gold=yes \
        --enable-ld=default \
        --enable-lto \
        --enable-plugins \
        2>&1 | tail -5

    make -j"${NPROCS}" 2>&1 | tail -5
    make install 2>&1 | tail -5

    ok "binutils installed to ${INSTALL_DIR}"
}

# ── Build GCC ────────────────────────────────────────────────────────

build_gcc() {
    info "Building gcc-${GCC_VERSION}..."
    info "  Target:   ${TARGET}"
    info "  Prefix:   ${INSTALL_DIR}"
    info "  Jobs:     ${NPROCS}"

    local BUILD_DIR_GCC="${BUILD_DIR}/build-gcc"
    mkdir -p "$BUILD_DIR_GCC"
    cd "$BUILD_DIR_GCC"

    if [ -f "${INSTALL_DIR}/bin/${TARGET}-gcc" ]; then
        ok "gcc already installed, skipping"
        return
    fi

    # Update PATH to include the new binutils
    export PATH="${INSTALL_DIR}/bin:${PATH}"

    "${SOURCES_DIR}/gcc-${GCC_VERSION}/configure" \
        --target="${TARGET}" \
        --prefix="${INSTALL_DIR}" \
        --disable-nls \
        --enable-languages=c \
        --without-headers \
        --disable-threads \
        --disable-shared \
        --disable-libatomic \
        --disable-libgomp \
        --disable-libquadmath \
        --disable-libssp \
        --disable-libstdcxx \
        --disable-libmudflap \
        --disable-libsanitizer \
        --disable-libvtv \
        --disable-libcilkrts \
        --enable-lto \
        --enable-plugin \
        --with-newlib \
        2>&1 | tail -5

    # Build only the C compiler (stage1)
    make -j"${NPROCS}" all-gcc 2>&1 | tail -10
    make install-gcc 2>&1 | tail -5

    ok "gcc installed to ${INSTALL_DIR}"
}

# ── Verify toolchain ─────────────────────────────────────────────────

verify_toolchain() {
    info "Verifying toolchain..."

    cd /tmp

    # Test the assembler
    echo 'int main(void) { return 42; }' > test_toolchain.c

    if "${INSTALL_DIR}/bin/${TARGET}-gcc" \
        -ffreestanding -nostdlib -nostdinc \
        -c test_toolchain.c -o test_toolchain.o 2>/dev/null; then
        ok "C compiler works: ${TARGET}-gcc"
    else
        err "C compiler test failed"
        exit 1
    fi

    if "${INSTALL_DIR}/bin/${TARGET}-ld" -o /dev/null test_toolchain.o 2>/dev/null; then
        ok "Linker works: ${TARGET}-ld"
    else
        err "Linker test failed (may need -nostdlib for empty binary)"
    fi

    if "${INSTALL_DIR}/bin/${TARGET}-objcopy" -O binary \
        test_toolchain.o /dev/null 2>/dev/null; then
        ok "Objcopy works: ${TARGET}-objcopy"
    fi

    rm -f test_toolchain.c test_toolchain.o

    echo ""
    info "Toolchain summary:"
    echo "  C compiler:  ${INSTALL_DIR}/bin/${TARGET}-gcc"
    echo "  Assembler:   ${INSTALL_DIR}/bin/${TARGET}-as"
    echo "  Linker:      ${INSTALL_DIR}/bin/${TARGET}-ld"
    echo "  Objcopy:     ${INSTALL_DIR}/bin/${TARGET}-objcopy"
    echo "  Objdump:     ${INSTALL_DIR}/bin/${TARGET}-objdump"
    echo ""
    info "Add to PATH:"
    echo "  export PATH=${INSTALL_DIR}/bin:\$PATH"
}

# ── Cleanup ──────────────────────────────────────────────────────────

do_clean() {
    info "Cleaning build artifacts..."

    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
        ok "Removed build directory: $BUILD_DIR"
    fi

    ok "Clean complete. Toolchain in ${INSTALL_DIR} is preserved."
}

# ── Main ─────────────────────────────────────────────────────────────

echo ""
echo "=========================================="
echo " x86_64-elf Cross-Compiler Toolchain"
echo "=========================================="
echo ""
echo "  Target:       ${TARGET}"
echo "  Prefix:       ${INSTALL_DIR}"
echo "  Binutils:     ${BINUTILS_VERSION}"
echo "  GCC:          ${GCC_VERSION}"
echo "  Parallel:     ${NPROCS} jobs"
echo ""

check_prereqs
download_sources
extract_sources
build_binutils
build_gcc
verify_toolchain

echo ""
echo "=========================================="
echo -e " ${GREEN}Toolchain build complete!${NC}"
echo "=========================================="
echo ""
echo "To use it in the kernel Makefile, set:"
echo "  export CC=${INSTALL_DIR}/bin/${TARGET}-gcc"
echo "  export LD=${INSTALL_DIR}/bin/${TARGET}-ld"
echo "  make"
echo ""
