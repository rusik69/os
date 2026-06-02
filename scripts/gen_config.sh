#!/bin/bash
# ── Generate kernel build configuration for /proc/config.gz ──────────
# Produces a text file with all relevant build-time configuration options
# that can be compressed and embedded in the kernel binary.

set -eu

OUTPUT="${1:-build_config.txt}"

# Collect build info
KERNEL_NAME="HermesOS"
KERNEL_VERSION="$(date +%Y%m%d)"
BUILD_USER="${USER:-unknown}"
BUILD_HOST="${HOSTNAME:-unknown}"
BUILD_DATE="$(date -u '+%Y-%m-%d %H:%M:%S UTC')"
ARCH="x86_64"
C_COMPILER="${CC:-x86_64-elf-gcc}"
COMPILER_VERSION="$(${C_COMPILER} --version 2>/dev/null | head -1 || echo 'unknown')"

cat > "$OUTPUT" <<EOF
#
# Automatically generated kernel build configuration
# ${BUILD_DATE}
#

# ── System ──────────────────────────────────────────────────────────
CONFIG_ARCH_X86_64=y
CONFIG_64BIT=y

# ── Kernel features ─────────────────────────────────────────────────
CONFIG_PREEMPT=y
CONFIG_SMP=y
CONFIG_NUMA=y
CONFIG_CGROUPS=y
CONFIG_NAMESPACES=y
CONFIG_PID_NS=y
CONFIG_NET_NS=y
CONFIG_USER_NS=y
CONFIG_COREDUMP=y
CONFIG_KALLSYMS=y
CONFIG_KALLSYMS_ALL=y
CONFIG_PRINTK=y
CONFIG_BOOT_PRINTK=y
CONFIG_DYNAMIC_DEBUG=y
CONFIG_FTRACE=y
CONFIG_KPROBES=y
CONFIG_PERF_EVENTS=y

# ── Security ────────────────────────────────────────────────────────
CONFIG_SECURITY=y
CONFIG_SECCOMP=y
CONFIG_SECCOMP_FILTER=y
CONFIG_SECURITY_YAMA=y
CONFIG_SECURITY_LANDLOCK=y
CONFIG_SECURITY_APPARMOR=n
CONFIG_INTEGRITY=y
CONFIG_IMA=y
CONFIG_SECURITY_SELINUX=n
CONFIG_NX_ENFORCE=y
CONFIG_ASLR=y
CONFIG_STACKPROTECTOR=y
CONFIG_SMAP=y
CONFIG_SMEP=y
CONFIG_KPTR_RESTRICT=y
CONFIG_DMESG_RESTRICT=y
CONFIG_MODULE_SIG=y

# ── Memory Management ───────────────────────────────────────────────
CONFIG_PAGE_POISONING=y
CONFIG_SLAB=y
CONFIG_SLAB_FREELIST_RANDOM=y
CONFIG_SLAB_FREELIST_HARDENED=y
CONFIG_SLAB_REDZONE=y
CONFIG_KASAN=y
CONFIG_KASAN_STACK=y
CONFIG_TRANSPARENT_HUGEPAGE=y
CONFIG_THP=y
CONFIG_KSM=y
CONFIG_ZRAM=y
CONFIG_ZRAM_LZ4HC=y
CONFIG_ZRAM_ZSTD=y
CONFIG_HUGETLB=y
CONFIG_COMPACTION=y
CONFIG_MEMORY_HOTPLUG=y
CONFIG_MEMFD=y

# ── Process / Scheduler ─────────────────────────────────────────────
CONFIG_CFS_BANDWIDTH=y
CONFIG_FAIR_GROUP_SCHED=y
CONFIG_RT_GROUP_SCHED=y
CONFIG_SCHED_DEADLINE=y
CONFIG_SCHED_FIFO=y
CONFIG_SCHED_RR=y
CONFIG_SCHED_IDLE=y
CONFIG_PELT=y
CONFIG_NUMA_BALANCING=y
CONFIG_CGROUP_CPU=y

# ── Filesystems ─────────────────────────────────────────────────────
CONFIG_TMPFS=y
CONFIG_EXT2_FS=y
CONFIG_FAT32_FS=y
CONFIG_ISO9660_FS=y
CONFIG_PROCFS=y
CONFIG_SYSFS=y
CONFIG_DEVFS=y
CONFIG_DEBUGFS=y
CONFIG_TARFS=y
CONFIG_OVERLAY_FS=y
CONFIG_FUSE=n
CONFIG_FS_QUOTA=y
CONFIG_FS_ENCRYPTION=y
CONFIG_FS_VERITY=n

# ── Networking ──────────────────────────────────────────────────────
CONFIG_INET=y
CONFIG_IPV6=n
CONFIG_TCP_CUBIC=y
CONFIG_TCP_BBR=n
CONFIG_NETFILTER=y
CONFIG_NETFILTER_CONNTRACK=y
CONFIG_NETFILTER_NAT=y
CONFIG_BRIDGE=y
CONFIG_VLAN=y
CONFIG_IPIP=y
CONFIG_VXLAN=n
CONFIG_WIREGUARD=y
CONFIG_IPVS=y
CONFIG_SYN_COOKIES=y
CONFIG_DNS_CACHE=n

# ── Drivers ─────────────────────────────────────────────────────────
CONFIG_ATA=y
CONFIG_AHCI=y
CONFIG_NVME=y
CONFIG_E1000=y
CONFIG_VIRTIO_BLK=y
CONFIG_VIRTIO_NET=y
CONFIG_USB_EHCI=y
CONFIG_USB_XHCI=y
CONFIG_ACPI=y
CONFIG_ACPI_EC=y
CONFIG_ACPI_THERMAL=y
CONFIG_PCI_MSI=y
CONFIG_PCI_AER=y
CONFIG_RTC=y
CONFIG_WATCHDOG=y
CONFIG_HPET=y
CONFIG_I2C=y
CONFIG_GPIO=y
CONFIG_TPM=n

# ── Power Management ────────────────────────────────────────────────
CONFIG_CPU_IDLE=y
CONFIG_CPU_FREQ=y
CONFIG_ACPI_PSTATE=y
CONFIG_SUSPEND=n

# ── Debugging ───────────────────────────────────────────────────────
CONFIG_LOCKDEP=y
CONFIG_LOCK_STAT=y
CONFIG_RCU_STALL=y
CONFIG_NMI_WATCHDOG=y
CONFIG_SOFTLOCKUP_DETECTOR=y
CONFIG_HARDLOCKUP_DETECTOR=y
CONFIG_STACKTRACE=y
CONFIG_PSTORE=y
CONFIG_KDUMP=y
CONFIG_BOOT_PRINTK=y

# ── Build information ───────────────────────────────────────────────
CONFIG_KERNEL_NAME="${KERNEL_NAME}"
CONFIG_KERNEL_VERSION="${KERNEL_VERSION}"
CONFIG_BUILD_USER="${BUILD_USER}"
CONFIG_BUILD_HOST="${BUILD_HOST}"
CONFIG_BUILD_DATE="${BUILD_DATE}"
CONFIG_ARCH="${ARCH}"
CONFIG_COMPILER="${COMPILER_VERSION}"

# ── Source code statistics (approximate) ────────────────────────────
EOF

# Append source statistics if find is available
if command -v find >/dev/null 2>&1 && command -v wc >/dev/null 2>&1; then
    SRC_DIR="$(dirname "$0")/../src"
    C_FILES=$(find "$SRC_DIR" -name '*.c' 2>/dev/null | wc -l)
    ASM_FILES=$(find "$SRC_DIR" -name '*.asm' 2>/dev/null | wc -l)
    H_FILES=$(find "$SRC_DIR" -name '*.h' 2>/dev/null | wc -l)
    C_LOC=$(find "$SRC_DIR" -name '*.c' -exec cat {} + 2>/dev/null | wc -l)
    ASM_LOC=$(find "$SRC_DIR" -name '*.asm' -exec cat {} + 2>/dev/null | wc -l)
    H_LOC=$(find "$SRC_DIR" -name '*.h' -exec cat {} + 2>/dev/null | wc -l)
    TOTAL_LOC=$((C_LOC + ASM_LOC + H_LOC))

    cat >> "$OUTPUT" <<EOF
CONFIG_SRC_C_FILES=${C_FILES}
CONFIG_SRC_ASM_FILES=${ASM_FILES}
CONFIG_SRC_H_FILES=${H_FILES}
CONFIG_SRC_C_LOC=${C_LOC}
CONFIG_SRC_ASM_LOC=${ASM_LOC}
CONFIG_SRC_H_LOC=${H_LOC}
CONFIG_SRC_TOTAL_LOC=${TOTAL_LOC}
EOF
fi

echo "Generated kernel config: $OUTPUT ($(wc -l < "$OUTPUT") lines)"
