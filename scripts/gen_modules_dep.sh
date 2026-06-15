#!/bin/bash
#
# gen_modules_dep.sh — Generate modules.dep for kernel modules
#
# Scans /tmp/modules_staging/ for all .ko files, extracts dependency
# information from each module, and builds a modules.dep file in the
# format: module_name.ko: dep1.ko dep2.ko
#
# If dependency info cannot be extracted (e.g., modinfo not available),
# falls back to a flat modules.dep with no dependencies.
#
# Output: /tmp/modules_staging/modules.dep

set -euo pipefail

MODULES_DIR="${1:-/tmp/modules_staging}"
OUTPUT_FILE="${MODULES_DIR}/modules.dep"

if [ ! -d "$MODULES_DIR" ]; then
    echo "Error: modules directory '$MODULES_DIR' not found"
    exit 1
fi

echo "[gen_modules_dep] Scanning $MODULES_DIR for .ko files..."

# Collect all .ko files
KO_FILES=("$MODULES_DIR"/*.ko)
KO_COUNT=${#KO_FILES[@]}

if [ "$KO_COUNT" -eq 0 ] || [ "${KO_FILES[0]}" = "$MODULES_DIR/*.ko" ]; then
    echo "[gen_modules_dep] No .ko files found — creating empty modules.dep"
    : > "$OUTPUT_FILE"
    echo "[gen_modules_dep] Created: $OUTPUT_FILE (empty)"
    exit 0
fi

echo "[gen_modules_dep] Found $KO_COUNT module(s)"

# Try to determine if modinfo is available
USE_MODINFO=0
if command -v modinfo &>/dev/null; then
    USE_MODINFO=1
    echo "[gen_modules_dep] Using modinfo for dependency extraction"
else
    echo "[gen_modules_dep] modinfo not available — creating flat modules.dep"
fi

# Build modules.dep
{
    for ko in "${KO_FILES[@]}"; do
        modname=$(basename "$ko")
        
        if [ "$USE_MODINFO" -eq 1 ]; then
            # Extract dependencies using modinfo
            deps=$(modinfo -F depends "$ko" 2>/dev/null || echo "")
            if [ -n "$deps" ]; then
                # Convert comma-separated deps to space-separated .ko filenames
                dep_list=""
                IFS=',' read -ra dep_array <<< "$deps"
                for dep in "${dep_array[@]}"; do
                    dep_trimmed=$(echo "$dep" | xargs)
                    if [ -n "$dep_trimmed" ]; then
                        dep_list="$dep_list ${dep_trimmed}.ko"
                    fi
                done
                echo "${modname}:${dep_list}"
            else
                echo "${modname}:"
            fi
        else
            # Fallback: flat entry with no dependencies
            echo "${modname}:"
        fi
    done
} > "$OUTPUT_FILE"

echo "[gen_modules_dep] Created: $OUTPUT_FILE"
echo "[gen_modules_dep] Contents:"
cat "$OUTPUT_FILE"
