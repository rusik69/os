#!/bin/bash
# install_modules.sh — Copy kernel modules (.ko) into a FAT32 disk image
#
# Usage: scripts/install_modules.sh <disk_image> [modules_dir]
#
# The disk image must be a FAT32 filesystem. Modules (.ko files) are
# copied to /modules/ inside the image, where the kernel module loader
# expects to find them at runtime (request_module() / insmod).
#
# If modules_dir is not specified, defaults to /tmp/modules_staging/.
#
# Requires mtools (mcopy) to inject files into the FAT32 image.
#   sudo apt install mtools

set -euo pipefail

DISK_IMAGE="${1:?Usage: $0 <disk_image> [modules_dir]}"
MODULES_DIR="${2:-/tmp/modules_staging}"

if [ ! -f "$DISK_IMAGE" ]; then
    echo "Error: disk image '$DISK_IMAGE' not found"
    exit 1
fi

if [ ! -d "$MODULES_DIR" ]; then
    echo "No modules found in '$MODULES_DIR' — skipping install"
    exit 0
fi

# Count .ko files
KO_COUNT=$(find "$MODULES_DIR" -name '*.ko' 2>/dev/null | wc -l)
if [ "$KO_COUNT" -eq 0 ]; then
    echo "No .ko files found in '$MODULES_DIR' — nothing to install"
    exit 0
fi

echo "=== Installing $KO_COUNT module(s) into $DISK_IMAGE ==="

# Use mcopy from mtools to inject files. We need to create /modules/
# directory first, then copy each .ko file.
# mtools uses drive letters: use mcopy with -i for image path directly.

# Check if mcopy is available
if command -v mcopy &>/dev/null; then
    # Create /modules directory (mmd -i <image>::/modules)
    mmd -i "$DISK_IMAGE" ::/modules 2>/dev/null || true

    for ko in "$MODULES_DIR"/*.ko; do
        name=$(basename "$ko")
        echo "  INSTALL  /modules/$name"
        mcopy -i "$DISK_IMAGE" "$ko" ::/modules/"$name"
    done
    echo "=== Done: $KO_COUNT modules installed ==="
else
    echo "Warning: mcopy (mtools) not found. Install it: sudo apt install mtools"
    echo "Modules remain staged in $MODULES_DIR"
    echo "To install manually:"
    echo "  sudo apt install mtools"
    echo "  scripts/install_modules.sh $DISK_IMAGE"
    exit 0
fi
