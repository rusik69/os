# kernel-config.mk — Kernel configuration defaults for Makefile inclusion
#
# This file mirrors the CONFIG_* flags from build_config.txt in Makefile
# variable form, so that conditional compilation in the Makefile and build
# rules can test them directly.
#
# Usage:
#   include kernel-config.mk
#   ifdef CONFIG_MODULE_SIG
#     CFLAGS += -DCONFIG_MODULE_SIG
#   endif
#
# Auto-generated from build_config.txt — keep in sync.
#

# ── System ──────────────────────────────────────────────────────────
CONFIG_ARCH_X86_64 := y
CONFIG_64BIT        := y

# ── Kernel features ─────────────────────────────────────────────────
CONFIG_PREEMPT      := y
CONFIG_SMP          := y
CONFIG_NUMA         := y
CONFIG_CGROUPS      := y
CONFIG_NAMESPACES   := y
CONFIG_PID_NS       := y
CONFIG_NET_NS       := y
CONFIG_USER_NS      := y
CONFIG_COREDUMP     := y
CONFIG_KALLSYMS     := y
CONFIG_KALLSYMS_ALL := y
CONFIG_PRINTK       := y
CONFIG_BOOT_PRINTK  := y
CONFIG_DYNAMIC_DEBUG := y
CONFIG_FTRACE       := y
CONFIG_KPROBES      := y
CONFIG_PERF_EVENTS  := y

# ── Security ────────────────────────────────────────────────────────
CONFIG_SECURITY       := y
CONFIG_SECCOMP        := y
CONFIG_SECCOMP_FILTER := y
CONFIG_SECCOMP_BPF    := y
CONFIG_SECURITY_YAMA  := y
CONFIG_SECURITY_LANDLOCK := y
CONFIG_SECURITY_APPARMOR := n
CONFIG_INTEGRITY      := y
CONFIG_IMA            := y
CONFIG_SECURITY_SELINUX := n
CONFIG_NX_ENFORCE     := y
CONFIG_ASLR           := y
CONFIG_STACKPROTECTOR := y
CONFIG_SMAP           := y
CONFIG_SMEP           := y
CONFIG_KPTR_RESTRICT  := y
CONFIG_DMESG_RESTRICT := y
CONFIG_SIGNAL_VALIDATE := y
CONFIG_PERF_PARANOID  := 2
CONFIG_MODULE_SIG     := y
CONFIG_MODULE_COMPRESS := y
CONFIG_FW_LOADER      := y
CONFIG_LOCKDOWN       := y

# ── Memory Management ───────────────────────────────────────────────
CONFIG_PAGE_POISONING        := y
CONFIG_SLAB                  := y
CONFIG_SLAB_FREELIST_RANDOM  := y
CONFIG_SLAB_FREELIST_HARDENED := y
CONFIG_SLAB_REDZONE          := y
CONFIG_KASAN                 := y
CONFIG_KASAN_STACK           := y
CONFIG_TRANSPARENT_HUGEPAGE  := y
CONFIG_THP                   := y
CONFIG_KSM                   := y
CONFIG_ZRAM                  := y
CONFIG_ZRAM_LZ4HC            := y
CONFIG_ZRAM_ZSTD             := y
CONFIG_HUGETLB               := y
CONFIG_COMPACTION            := y
CONFIG_MEMORY_HOTPLUG        := y
CONFIG_MEMFD                 := y
CONFIG_MSEAL                 := y
CONFIG_USERFAULTFD           := y
CONFIG_DMA_API               := y

# ── Process / Scheduler ─────────────────────────────────────────────
CONFIG_CFS_BANDWIDTH    := y
CONFIG_FAIR_GROUP_SCHED := y
CONFIG_RT_GROUP_SCHED   := y
CONFIG_SCHED_DEADLINE   := y
CONFIG_SCHED_FIFO       := y
CONFIG_SCHED_RR         := y
CONFIG_SCHED_IDLE       := y
CONFIG_PELT             := y
CONFIG_NUMA_BALANCING   := y
CONFIG_CGROUP_CPU       := y
CONFIG_CORE_SCHED       := y
CONFIG_NOHZ             := y

# ── Cluster / Orchestration ─────────────────────────────────────────
CONFIG_CLUSTER_AUTOSCALER   := y
CONFIG_CLUSTER_DESCHEDULER  := y
CONFIG_CLUSTER_INGRESS      := y
CONFIG_CONTAINER_EXEC       := y

# ── Filesystems ─────────────────────────────────────────────────────
CONFIG_TMPFS       := y
CONFIG_EXT2_FS     := y
CONFIG_FAT32_FS    := y
CONFIG_ISO9660_FS  := y
CONFIG_PROCFS      := y
CONFIG_SYSFS       := y
CONFIG_DEVFS       := y
CONFIG_DEBUGFS     := y
CONFIG_TARFS       := y
CONFIG_OVERLAY_FS  := y
CONFIG_FUSE        := n
CONFIG_FS_QUOTA    := y
CONFIG_FS_ENCRYPTION := y
CONFIG_FS_VERITY   := n

# ── Networking ──────────────────────────────────────────────────────
CONFIG_INET             := y
CONFIG_IPV6             := n
CONFIG_TCP_CUBIC        := y
CONFIG_TCP_BBR          := n
CONFIG_NETFILTER        := y
CONFIG_NETFILTER_CONNTRACK := y
CONFIG_NETFILTER_NAT    := y
CONFIG_BRIDGE           := y
CONFIG_VLAN             := y
CONFIG_IPIP             := y
CONFIG_VXLAN            := n
CONFIG_WIREGUARD        := y
CONFIG_IPVS             := y
CONFIG_SYN_COOKIES      := y
CONFIG_DNS_CACHE        := n

# ── Drivers ─────────────────────────────────────────────────────────
CONFIG_ATA         := y
CONFIG_AHCI        := y
CONFIG_NVME        := y
CONFIG_E1000       := y
CONFIG_VIRTIO_BLK  := y
CONFIG_VIRTIO_NET  := y
CONFIG_USB_EHCI    := y
CONFIG_USB_XHCI    := y
CONFIG_ACPI        := y
CONFIG_ACPI_EC     := y
CONFIG_ACPI_THERMAL := y
CONFIG_PCI_MSI     := y
CONFIG_PCI_AER     := y
CONFIG_RTC         := y
CONFIG_WATCHDOG    := y
CONFIG_HPET        := y
CONFIG_I2C         := y
CONFIG_GPIO        := y
CONFIG_TPM         := n

# ── Power Management ────────────────────────────────────────────────
CONFIG_CPU_IDLE    := y
CONFIG_CPU_FREQ    := y
CONFIG_ACPI_PSTATE := y
CONFIG_SUSPEND     := n

# ── Debugging ───────────────────────────────────────────────────────
CONFIG_LOCKDEP            := y
CONFIG_LOCK_STAT          := y
CONFIG_RCU_STALL          := y
CONFIG_NMI_WATCHDOG       := y
CONFIG_SOFTLOCKUP_DETECTOR := y
CONFIG_HARDLOCKUP_DETECTOR := y
CONFIG_STACKTRACE         := y
CONFIG_PSTORE             := y
CONFIG_KDUMP              := y
