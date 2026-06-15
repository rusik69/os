#!/bin/bash
#
# initramfs.sh — Minimal initramfs CPIO archive generator
#
# Constructs a minimal initramfs cpio archive containing a busybox-skeleton
# with essential device nodes (/dev/null, /dev/console, /dev/ttyS0),
# an init script that mounts rootfs, pivot_root, and execs the real init.
#
# Item S161: Initramfs generator
#
# Usage:
#   scripts/initramfs.sh [-o output.cpio] [-g] [-d dir]
#
# Options:
#   -o FILE   Output archive path (default: build/initramfs.cpio)
#   -g        Compress with gzip (produces .cpio.gz)
#   -d DIR    Working directory for staging (default: /tmp/initramfs_staging)
#
# Examples:
#   scripts/initramfs.sh -o build/initramfs.cpio.gz -g
#   scripts/initramfs.sh -o build/initramfs.cpio

set -euo pipefail

# ── Defaults ──────────────────────────────────────────────────────────────
OUTPUT="build/initramfs.cpio"
STAGING_DIR="/tmp/initramfs_staging"
DO_COMPRESS=0

# ── Parse arguments ───────────────────────────────────────────────────────
while getopts "o:gd:" opt; do
    case "$opt" in
        o) OUTPUT="$OPTARG" ;;
        g) DO_COMPRESS=1 ;;
        d) STAGING_DIR="$OPTARG" ;;
        *) echo "Usage: $0 [-o output.cpio] [-g] [-d staging_dir]"
           exit 1 ;;
    esac
done

echo "[initramfs] Creating initramfs at ${OUTPUT}..."

# ── Clean and recreate staging directory ──────────────────────────────────
rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR"

# ── Create essential directory structure ──────────────────────────────────
mkdir -p "$STAGING_DIR/bin"
mkdir -p "$STAGING_DIR/sbin"
mkdir -p "$STAGING_DIR/etc"
mkdir -p "$STAGING_DIR/dev"
mkdir -p "$STAGING_DIR/proc"
mkdir -p "$STAGING_DIR/sys"
mkdir -p "$STAGING_DIR/tmp"
mkdir -p "$STAGING_DIR/var/log"
mkdir -p "$STAGING_DIR/var/run"
mkdir -p "$STAGING_DIR/mnt/root"

# ── Create essential device nodes ─────────────────────────────────────────
# /dev/null — null device (major 1, minor 3)
mknod -m 666 "$STAGING_DIR/dev/null" c 1 3 2>/dev/null || {
    echo "[initramfs] Warning: could not create /dev/null (cpio will include node anyway)"
}

# /dev/console — system console (major 5, minor 1)
mknod -m 600 "$STAGING_DIR/dev/console" c 5 1 2>/dev/null || true

# /dev/ttyS0 — first serial port (major 4, minor 64)
mknod -m 600 "$STAGING_DIR/dev/ttyS0" c 4 64 2>/dev/null || true

# /dev/tty0 — current virtual console (major 4, minor 0)
mknod -m 600 "$STAGING_DIR/dev/tty0" c 4 0 2>/dev/null || true

# /dev/random, /dev/urandom — random number devices
mknod -m 644 "$STAGING_DIR/dev/random" c 1 8 2>/dev/null || true
mknod -m 644 "$STAGING_DIR/dev/urandom" c 1 9 2>/dev/null || true

# /dev/zero — zero device (major 1, minor 5)
mknod -m 666 "$STAGING_DIR/dev/zero" c 1 5 2>/dev/null || true

echo "[initramfs] Device nodes created"

# ── Create busybox-skeleton (symlinks to /bin/busybox) ────────────────────
# In a real initramfs, busybox would be placed at /bin/busybox.
# Here we create a placeholder script that provides essential applets.
cat > "$STAGING_DIR/bin/busybox" << 'BUSYBOX_PLACEHOLDER'
#!/bin/sh
# Busybox multi-call binary stub for initramfs
# In a real build, replace this with an actual static busybox binary.
cmd=$(basename "$0")
case "$cmd" in
    sh|ash)
        exec /bin/sh "$@"
        ;;
    mount)
        /bin/mount "$@"
        ;;
    umount)
        /bin/umount "$@"
        ;;
    reboot)
        echo "Rebooting..."
        echo b > /proc/sysrq-trigger 2>/dev/null || reboot -f
        ;;
    poweroff)
        echo "Powering off..."
        echo o > /proc/sysrq-trigger 2>/dev/null || poweroff -f
        ;;
    cat|echo|ls|mkdir|cp|mv|rm)
        # These are provided by the kernel shell builtins
        exec "/bin/$cmd" "$@"
        ;;
    *)
        echo "busybox: $cmd: applet not found"
        exit 1
        ;;
esac
BUSYBOX_PLACEHOLDER
chmod +x "$STAGING_DIR/bin/busybox"

# Create symlinks to busybox for common applets
for applet in sh ash mount umount reboot poweroff cat echo ls mkdir cp mv rm; do
    ln -sf /bin/busybox "$STAGING_DIR/bin/$applet" 2>/dev/null || true
done

# Provide /bin/sh if not already present
if [ ! -f "$STAGING_DIR/bin/sh" ]; then
    ln -sf /bin/busybox "$STAGING_DIR/bin/sh" 2>/dev/null || true
fi

echo "[initramfs] Busybox skeleton created"

# ── Copy kernel modules into initramfs ────────────────────────────────────
if [ -d /tmp/modules_staging ]; then
    KO_COUNT=$(find /tmp/modules_staging -maxdepth 1 -name '*.ko' 2>/dev/null | wc -l)
    if [ "$KO_COUNT" -gt 0 ]; then
        echo "[initramfs] Copying $KO_COUNT kernel modules into initramfs..."
        mkdir -p "$STAGING_DIR/modules"
        for ko in /tmp/modules_staging/*.ko; do
            name=$(basename "$ko")
            cp "$ko" "$STAGING_DIR/modules/$name"
            echo "  COPY  modules/$name"
        done
    else
        echo "[initramfs] No .ko files found in /tmp/modules_staging — skipping modules"
    fi

    # Copy etc/modules (auto-load list) if present
    if [ -f /tmp/modules_staging/etc/modules ]; then
        echo "[initramfs] Copying /etc/modules (auto-load list)..."
        mkdir -p "$STAGING_DIR/etc"
        cp /tmp/modules_staging/etc/modules "$STAGING_DIR/etc/modules"
    fi

    # Copy modules.dep (dependency metadata) if present
    if [ -f /tmp/modules_staging/modules.dep ]; then
        echo "[initramfs] Copying modules.dep..."
        cp /tmp/modules_staging/modules.dep "$STAGING_DIR/modules/modules.dep"
    fi
else
    echo "[initramfs] /tmp/modules_staging not found — skipping kernel modules"
fi

# ── Create /etc/fstab stub ────────────────────────────────────────────────
cat > "$STAGING_DIR/etc/fstab" << 'FSTAB'
# /etc/fstab — static filesystem table for initramfs
# <device>    <mountpoint>    <type>    <opts>
proc          /proc           proc      defaults
sysfs         /sys            sysfs     defaults
devtmpfs      /dev            devtmpfs  defaults
FSTAB

# ── Create the init script ────────────────────────────────────────────────
# This is the key component: it mounts the real root filesystem,
# moves mount points, pivot_roots, and execs the real init.
cat > "$STAGING_DIR/init" << 'INIT_SCRIPT'
#!/bin/sh
#
# initramfs init script
#
# This is invoked by the kernel as PID 1 after unpacking the initramfs.
# It handles:
#   1. Mounting essential virtual filesystems (proc, sysfs, devtmpfs)
#   2. Parsing the root= kernel command-line parameter
#   3. Mounting the real root filesystem
#   4. Moving mounts and pivot_root to the real root
#   5. Exec'ing the real init (/sbin/init)
#
# Supported root= formats:
#   root=/dev/sda1         — block device path
#   root=UUID=xxxx-xxxx    — UUID-based root (requires blkid)
#   root=LABEL=xxx         — label-based root (requires fsck/blkid)
#   root=/dev/hda1         — legacy IDE device
#

# Mount essential virtual filesystems
echo "[initramfs] Mounting proc..."
mount -t proc proc /proc

echo "[initramfs] Mounting sysfs..."
mount -t sysfs sysfs /sys

echo "[initramfs] Mounting devtmpfs..."
mount -t devtmpfs dev /dev

# Create /dev nodes if devtmpfs didn't cover them
[ -e /dev/null ]    || mknod /dev/null    c 1 3
[ -e /dev/console ] || mknod /dev/console c 5 1
[ -e /dev/ttyS0 ]   || mknod /dev/ttyS0   c 4 64

# Load essential kernel modules
echo "[initramfs] Loading kernel modules..."
if [ -d /modules ]; then
    for m in /modules/*.ko; do
        [ -f "$m" ] || continue
        name=$(basename "$m" .ko)
        echo "[initramfs] Loading module: $name"
        insmod "$m" 2>/dev/null || echo "[initramfs] Warning: failed to load $name"
    done
fi

# Load modules listed in /etc/modules (auto-load list)
if [ -f /etc/modules ]; then
    while read -r modname; do
        [ -z "$modname" ] || [ "${modname#\(comment}" != "$modname" ] && continue
        echo "[initramfs] Loading module: $modname"
        modprobe "$modname" 2>/dev/null || echo "[initramfs] Warning: failed to load $modname"
    done < /etc/modules
fi

# Parse kernel command line from /proc/cmdline
CMDLINE=$(cat /proc/cmdline 2>/dev/null || echo "")

# Extract root= parameter
ROOT_DEVICE=""
for param in $CMDLINE; do
    case "$param" in
        root=*)
            ROOT_DEVICE="${param#root=}"
            ;;
        ro)
            # Read-only root — handled at mount time
            ;;
        rw)
            # Read-write root — handled at mount time
            ;;
    esac
done

# If no root= specified, try common defaults
if [ -z "$ROOT_DEVICE" ]; then
    echo "[initramfs] No root= parameter found, trying defaults..."
    for dev in /dev/sda1 /dev/vda1 /dev/hda1 /dev/nvme0n1p1; do
        if [ -b "$dev" ]; then
            ROOT_DEVICE="$dev"
            echo "[initramfs] Found root device: $ROOT_DEVICE"
            break
        fi
    done
fi

# Resolve UUID= and LABEL=
case "$ROOT_DEVICE" in
    UUID=*)
        UUID="${ROOT_DEVICE#UUID=}"
        echo "[initramfs] Resolving UUID=$UUID..."
        # Scan /sys/block for matching device
        for blk in /sys/block/*; do
            [ -d "$blk" ] || continue
            for part in "$blk"/*/uevent; do
                [ -f "$part" ] || continue
                # Would use blkid to resolve UUID → device
                :
            done
        done
        # Fallback: try /dev/disk/by-uuid/
        if [ -e "/dev/disk/by-uuid/$UUID" ]; then
            ROOT_DEVICE="/dev/disk/by-uuid/$UUID"
        else
            echo "[initramfs] WARNING: Cannot resolve UUID=$UUID, trying as-is"
        fi
        ;;
    LABEL=*)
        LABEL="${ROOT_DEVICE#LABEL=}"
        echo "[initramfs] Resolving LABEL=$LABEL..."
        if [ -e "/dev/disk/by-label/$LABEL" ]; then
            ROOT_DEVICE="/dev/disk/by-label/$LABEL"
        else
            echo "[initramfs] WARNING: Cannot resolve LABEL=$LABEL, trying as-is"
        fi
        ;;
esac

echo "[initramfs] Root device: ${ROOT_DEVICE:-none}"

# Determine root mount options (ro vs rw)
ROOT_OPTS="ro"
for param in $CMDLINE; do
    case "$param" in
        rw) ROOT_OPTS="rw" ;;
    esac
done

# Mount the real root filesystem
if [ -n "$ROOT_DEVICE" ] && [ -b "$ROOT_DEVICE" ]; then
    echo "[initramfs] Mounting root filesystem from $ROOT_DEVICE ($ROOT_OPTS)..."
    mount -o "$ROOT_OPTS" "$ROOT_DEVICE" /mnt/root 2>/dev/null || {
        echo "[initramfs] First mount failed, trying without filesystem type..."
        mount "$ROOT_DEVICE" /mnt/root 2>/dev/null || {
            echo "[initramfs] ERROR: Cannot mount root device!"
            echo "[initramfs] Dropping to emergency shell..."
            exec /bin/sh
        }
    }
elif [ -n "$ROOT_DEVICE" ]; then
    echo "[initramfs] ERROR: Root device '$ROOT_DEVICE' not found!"
    echo "[initramfs] Dropping to emergency shell..."
    exec /bin/sh
else
    echo "[initramfs] ERROR: No root device specified!"
    echo "[initramfs] Dropping to emergency shell..."
    exec /bin/sh
fi

echo "[initramfs] Root filesystem mounted successfully"

# Move essential mount points into the real root
echo "[initramfs] Moving mounts to real root..."
mount --move /dev /mnt/root/dev 2>/dev/null || echo "[initramfs] Note: could not move /dev"
mount --move /proc /mnt/root/proc 2>/dev/null || echo "[initramfs] Note: could not move /proc"
mount --move /sys /mnt/root/sys 2>/dev/null || echo "[initramfs] Note: could not move /sys"

# Clean up /tmp mount
mount --move /tmp /mnt/root/tmp 2>/dev/null || true

# pivot_root to the real root
echo "[initramfs] Performing pivot_root..."
cd /mnt/root
# Move the old root (our initramfs) to /oldroot
pivot_root . /mnt/root/oldroot 2>/dev/null || {
    # If pivot_root is not available, try switch_root or exec chroot
    echo "[initramfs] pivot_root not available, trying switch_root..."
    exec switch_root /mnt/root /sbin/init 2>/dev/null || {
        echo "[initramfs] switch_root not available, using chroot fallback..."
        exec chroot /mnt/root /sbin/init
    }
}

# Unmount the old initramfs
umount /oldroot 2>/dev/null || true

# Execute the real init
echo "[initramfs] Exec'ing real init (/sbin/init)..."
exec /sbin/init "$@"
INIT_SCRIPT
chmod +x "$STAGING_DIR/init"

echo "[initramfs] Init script created"

# ── Create a minimal /sbin/init stub (fallback if real init not found) ────
cat > "$STAGING_DIR/sbin/init" << 'SBIN_INIT'
#!/bin/sh
# Stub /sbin/init — fallback if the real init doesn't exist on rootfs
echo "[initramfs] WARNING: /sbin/init not found on rootfs!"
echo "[initramfs] Starting fallback shell..."
exec /bin/sh
SBIN_INIT
chmod +x "$STAGING_DIR/sbin/init"

# ── Create /etc/inittab stub ──────────────────────────────────────────────
cat > "$STAGING_DIR/etc/inittab" << 'INITTAB'
# /etc/inittab — init configuration for initramfs
# Format: id:runlevels:action:process
::sysinit:/etc/init.d/rcS
::respawn:/bin/sh
INITTAB

# ── Create /etc/init.d/rcS ───────────────────────────────────────────────
mkdir -p "$STAGING_DIR/etc/init.d"
cat > "$STAGING_DIR/etc/init.d/rcS" << 'RCS'
#!/bin/sh
# System initialization script
echo "[rcS] Running system initialization..."
# Mount all filesystems
mount -a 2>/dev/null || true
RCS
chmod +x "$STAGING_DIR/etc/init.d/rcS"

echo "[initramfs] Directory structure:"
find "$STAGING_DIR" -type f -o -type l -o -type c | sort | sed 's|^/tmp/initramfs_staging|.|'

# ── Build the CPIO archive ────────────────────────────────────────────────
OUTPUT_DIR=$(dirname "$OUTPUT")
mkdir -p "$OUTPUT_DIR"

echo "[initramfs] Building CPIO archive..."
(
    cd "$STAGING_DIR"
    find . -print | sort | cpio -o -H newc --quiet
) > "${OUTPUT}.tmp"

mv "${OUTPUT}.tmp" "$OUTPUT"
echo "[initramfs] Created: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"

# ── Compress if requested ─────────────────────────────────────────────────
if [ "$DO_COMPRESS" -eq 1 ]; then
    echo "[initramfs] Compressing with gzip..."
    gzip -f -9 "$OUTPUT"
    echo "[initramfs] Created: ${OUTPUT}.gz ($(du -h "${OUTPUT}.gz" | cut -f1))"
fi

echo "[initramfs] Done!"
