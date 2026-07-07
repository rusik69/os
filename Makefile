# ── Compiler / Toolchain ──────────────────────────────────────────────

ifeq ($(origin CC), undefined)
CC = x86_64-elf-gcc
else ifeq ($(origin CC), default)
CC = x86_64-elf-gcc
endif

# Auto-detect ccache and wrap CC if available — massively speeds up rebuilds
ifneq ($(shell which ccache 2>/dev/null),)
  CC := ccache $(CC)
endif

# Auto-detect distcc for distributed builds (Item 257)
# If DISTCC_HOSTS is set (e.g., DISTCC_HOSTS="localhost 10.0.0.1"), prepend distcc.
# distcc and ccache compose naturally: CCACHE_PREFIX=distcc make
# but we also support a bare distcc cc for non-ccache setups.
ifneq ($(DISTCC_HOSTS),)
  ifeq ($(origin CC), undefined)
    CC = distcc x86_64-elf-gcc
  else ifeq ($(origin CC), default)
    CC = distcc x86_64-elf-gcc
  else
    # If CC was overridden (e.g. by ccache), wrap in distcc via CCACHE_PREFIX
    CCACHE_PREFIX := distcc
  endif
endif

AS = nasm
ifeq ($(origin LD), undefined)
LD = x86_64-elf-ld
else ifeq ($(origin LD), default)
LD = x86_64-elf-ld
endif
ifeq ($(origin OBJCOPY), undefined)
OBJCOPY = x86_64-elf-objcopy
else ifeq ($(origin OBJCOPY), default)
OBJCOPY = x86_64-elf-objcopy
endif

# Number of parallel jobs (all available CPU cores)
NPROCS := $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)

# Kernel version string — used in module vermagic checking
KVERSION ?= 6.1.0-osdev

# Determine preemption model for vermagic
ifdef CONFIG_PREEMPT
VERMAGIC_FLAGS := -DCONFIG_PREEMPT
else ifdef CONFIG_PREEMPT_VOLUNTARY
VERMAGIC_FLAGS := -DCONFIG_PREEMPT_VOLUNTARY
else
VERMAGIC_FLAGS :=
endif

# SMP enabled? (auto-detected from target)
VERMAGIC_FLAGS += -DCONFIG_SMP

# NOTE: -Wno-unused-* are intentionally suppressed in this freestanding kernel
# build.  -Wunused-* produces noise because kernel code deliberately omits
# parameter names in function declarations (forward-compatibility stubs) and
# has variables that are read by inline assembly or used only in certain
# configs.  These suppressions keep the build clean while -Werror is active.
# Note: format checking suppressed — the kernel defines size_t as uint64_t
# (= unsigned long long) which conflicts with GCC's built-in size_t for %zu.
# __printf annotations are still present for documentation and static analysis.
CFLAGS = -std=c17 -ffreestanding -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -fstack-protector-strong -fstack-clash-protection -mstack-protector-guard=global -fno-omit-frame-pointer -nostdlib -nostdinc -fno-builtin \
         -Wall -Wextra -Werror -Wno-format -Wno-sign-conversion -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable -Isrc/include -Iuserspace/kmods/gui -Iuserspace/kmods/doom -mcmodel=large -g \
         -Wa,--noexecstack -O2 -MMD -MP \
         -include kernel_pch.h \
         -DKVERSION=\"$(KVERSION)\" \
         -DBUILD_TIME=\"$(shell date -u '+%Y-%m-%d_%H:%M:%S_UTC')\" \
         $(VERMAGIC_FLAGS) \
         $(CFLAGS_EXTRA)
ASFLAGS = -f elf64 -g
LDFLAGS = -T linker.ld -nostdlib -z max-page-size=0x1000 -z noexecstack -z relro -z now --allow-multiple-definition

# Auto-detect libgcc path (supports both x86_64-elf-gcc and x86_64-linux-gnu-gcc)
LIBGCC := $(shell $(CC) -print-libgcc-file-name 2>/dev/null || echo "/usr/lib/gcc/x86_64-linux-gnu/13/libgcc.a")

BUILDDIR = build

C_SRCS = src/kernel/kernel.c \
         src/kernel/gdt.c \
         src/kernel/idt.c \
         src/kernel/fault.c \
         src/kernel/syscall.c \
         src/kernel/syscall_cleanup.c \
         src/kernel/syscall_new.c \
         src/kernel/syscall_linux.c \
         src/kernel/posix_timer.c \
         src/kernel/sys_process.c \
         src/kernel/sys_mmap.c \
         src/kernel/sys_nl.c \
         src/kernel/sys_ioctl.c \
         src/kernel/sys_credentials.c \
         src/kernel/sys_caps.c \
         src/kernel/vfs.c \
         src/kernel/elf.c \
         src/kernel/spawn_kernel.c \
         src/kernel/apic.c \
         src/kernel/cmdline.c \
         src/kernel/smp.c \
         src/kernel/cpu.c \
         src/kernel/cet.c \
         src/kernel/cpuidle.c \
         src/kernel/oom.c \
         src/kernel/rcu.c \
         src/kernel/aslr.c \
         src/kernel/splash.c \
         src/kernel/seccomp.c \
         src/kernel/sysrq.c \
         src/kernel/shell_hooks.c \
         src/kernel/panic.c \
         src/kernel/nmi_watchdog.c \
         src/kernel/lockdep.c \
         src/kernel/timers.c \
         src/kernel/workqueue.c \
         src/kernel/idr.c \
         src/kernel/rng.c \
         src/kernel/fsnotify.c \
         src/kernel/crypto.c \
         src/kernel/module.c \
         src/kernel/module_elf.c \
         src/kernel/module_signature.c \
         src/kernel/module_autoload.c \
         src/kernel/module_compress.c \
         src/kernel/module_deps.c \
         src/kernel/module_alias.c \
         src/kernel/module_async.c \
         src/kernel/sys_module.c \
         src/kernel/sysctl.c \
         src/kernel/ksym.c \
         src/kernel/caps.c \
         src/kernel/signal_validate.c \
         src/kernel/chroot.c \
         src/kernel/pid_namespace.c \
         src/kernel/cgroup_namespace.c \
         src/kernel/mnt_namespace.c \
         src/kernel/user_namespace.c \
         src/kernel/smack.c \
         src/kernel/tpm_attest.c \
         src/kernel/efi_secureboot.c \
         src/kernel/ipc_namespace.c \
         src/kernel/uid16.c \
         src/kernel/swap.c \
         src/drivers/vga.c \
         src/drivers/pic.c \
         src/drivers/timer.c \
         src/drivers/keyboard.c \
         src/drivers/serial.c \
         src/drivers/ata.c \
         src/drivers/ata_pio.c \
         src/drivers/ata_identify.c \
         src/drivers/ata_opal.c \
         src/drivers/ata_power.c \
         src/drivers/pci.c \
         src/drivers/blockdev.c \
         src/drivers/genhd.c \
         src/drivers/e1000.c \
         src/drivers/rtl8139.c \
         src/drivers/vmxnet3.c \
         src/drivers/igb.c \
         src/drivers/i40e.c \
         src/drivers/mlx4.c \
         src/drivers/intel_gpu.c \
         src/drivers/rtc.c \
         src/drivers/mouse.c \
         src/drivers/speaker.c \
         src/drivers/acpi.c \
         src/drivers/acpi_aml.c \
         src/drivers/acpi_thermal.c \
         src/drivers/acpi_ec.c \
         src/drivers/acpi_cpufreq.c \
         src/drivers/acpi_power_button.c \
         src/drivers/acpi_button.c \
         src/drivers/acpi_ac_adapter.c \
         src/power/suspend.c \
         src/power/pm_qos.c \
         src/power/wakeup.c \
         src/power/cpufreq.c \
         src/power/cpufreq_ondemand.c \
         src/power/cpufreq_conservative.c \
         src/power/cpufreq_userspace.c \
         src/power/cpufreq_schedutil.c \
         src/power/cpuidle_ladder.c \
         src/power/cpuidle_teo.c \
         src/power/devfreq.c \
         src/power/energy_model.c \
         src/drivers/ahci.c \
         src/drivers/nvme.c \
         src/drivers/nvme_pmr.c \
         src/drivers/usb_ehci.c \
         src/drivers/usb_msc.c \
         src/drivers/usb_hid_joy.c \
         src/drivers/usb_hid_consumer.c \
         src/drivers/usb_hid_sysctrl.c \
         src/drivers/usb_printer.c \
         src/drivers/usb_typec.c \
         src/drivers/usb_debug.c \
         src/drivers/usb_audio.c \
         src/drivers/usb_video.c \
         src/drivers/ps2.c \
         src/drivers/fbcon.c \
         src/drivers/partitions.c \
         src/drivers/mdadm.c \
         src/drivers/mdadm_ext.c \
         src/drivers/nbd.c \
         src/drivers/iscsi.c \
         src/drivers/nvmf.c \
         src/drivers/fcoe.c \
         src/drivers/drbd.c \
         src/drivers/rbd.c \
         src/drivers/cmos.c \
         src/drivers/floppy.c \
         src/drivers/battery.c \
         src/drivers/i2c.c \
         src/drivers/edid.c \
         src/drivers/smbus.c \
         src/memory/pmm.c \
         src/memory/vmm.c \
         src/memory/heap.c \
         src/memory/slab.c \
         src/memory/compaction.c \
         src/memory/hugetlb.c \
         src/process/process.c \
         src/process/scheduler.c \
         src/process/sched_deadline.c \
         src/process/pelt.c \
         src/process/signal.c \
         src/process/users.c \
         src/fs/fs.c \
         src/fs/procfs.c \
         src/fs/procfs_cpuinfo.c \
         src/fs/procfs_meminfo.c \
         src/fs/procfs_stat.c \
         src/fs/devfs.c \
         src/fs/tmpfs.c \
         src/fs/tmpfs_huge.c \
         src/ipc/shm.c \
         src/fs/fat32.c \
         src/fs/vfat_shortname.c \
         src/fs/fat32_lfn.c \
         src/fs/iso9660.c \
         src/fs/iso9660_rr.c \
         src/fs/iso9660_joliet.c \
         src/fs/bufcache.c \
         src/fs/page_cache.c \
         src/fs/fstab.c \
         src/fs/fsck.c \
         src/fs/xattr.c \
         src/fs/posix_acl.c \
         src/fs/iosched.c \
         src/fs/luks.c \
         src/fs/ext2.c \
         src/fs/ext2_ea.c \
         src/fs/ext2_acl.c \
         src/fs/crypto.c \
         src/fs/ext4.c \
         src/fs/ext4_extents.c \
         src/fs/jbd2.c \
         src/fs/cifs.c \
         src/fs/nfsd.c \
         src/fs/reiserfs.c \
         src/fs/cpio.c \
         src/fs/initramfs.c \
         src/fs/romfs.c \
         src/fs/tarfs.c \
         src/fs/vfs_enhance.c \
         src/fs/squashfs.c \
         src/fs/fuse.c \
         src/fs/fuse_dev.c \
         src/fs/fuse_notify.c \
         src/fs/nfs_proc.c \
         src/net/net.c \
         src/net/net_tcp.c \
         src/net/net_udp.c \
         src/net/ipv6.c \
         src/net/ipv6_core.c \
         src/net/ipv6_ndisc.c \
         src/net/ipv6_pmtu.c \
         src/net/udp_ipv6.c \
         src/net/tcp_ipv6.c \
         src/net/ipv6_mld.c \
         src/net/telnetd.c \
         src/net/httpd.c \
         src/net/socket.c \
         src/net/socket_ext.c \
         src/net/af_unix.c \
         src/net/af_packet.c \
         src/net/can.c \
         src/net/netlink.c \
         src/net/netfilter.c \
         src/net/netfilter_hooks.c \
         src/net/nf_tables.c \
         src/net/conntrack.c \
         src/net/conntrack_helpers.c \
         src/net/dns_cache.c \
         src/net/dns_resolver.c \
         src/net/pkt_sched.c \
         src/net/bridge.c \
         src/net/stp.c \
         src/net/net_ext.c \
         src/net/vlan.c \
         src/net/tun.c \
         src/net/net_ns.c \
         src/net/ipip.c \
         src/net/gre.c \
         src/net/vxlan.c \
         src/net/wireguard.c \
         src/net/wg_netlink.c \
         src/net/ipvs.c \
         src/net/veth.c \
         src/net/netdevice.c \
         src/net/rps.c \
         src/net/sshd.c \
         src/net/tcp_bbr.c \
         src/net/tcp_cubic.c \
         src/net/tcp_newreno.c \
         src/net/sctp.c \
         src/net/sctp_sm.c \
         src/net/sctp_tsn.c \
         src/net/dccp.c \
         src/net/mptcp.c \
         src/net/mptcp_sched.c \
         src/net/macsec.c \
         src/net/6lowpan.c \
         src/net/ipoib.c \
         src/net/ipsec.c \
         src/net/pfkey.c \
         src/net/igmp.c \
         src/net/garp.c \
         src/net/lacp.c \
         src/net/mrp.c \
         src/net/lldp.c \
         src/net/tcp_bic.c \
         src/net/sch_tbf.c \
         src/net/sch_fq.c \
         src/net/sch_red.c \
         src/net/tcp_vegas.c \
         src/net/tcp_westwood.c \
         src/net/tcp_illinois.c \
         src/net/tcp_hybla.c \
         src/net/tcp_cc.c \
         src/net/tls.c \
         src/net/tls_handshake.c \
         src/net/tls_aead.c \
         src/net/tls_session.c \
         src/net/tls_x509.c \
         src/net/ktls.c \
         src/kernel/service.c \
         src/kernel/ssh_crypto.c \
         src/kernel/ssh_client.c \
         src/ipc/pipe.c \
         src/ipc/eventfd.c \
         src/ipc/inotify.c \
         src/drivers/virtio_net.c \
         src/drivers/virtio_blk.c \
         src/drivers/virtio_pci_modern.c \
         src/drivers/ac97.c \
         src/drivers/sound_core.c \
         src/drivers/sound_midi.c \
         src/drivers/sound_mixer_sw.c \
         src/drivers/sound_oss.c \
         src/drivers/sound_pcm.c \
         src/drivers/sound_src.c \
         src/drivers/fm_synth.c \
         src/drivers/fm_gm.c \
         src/drivers/ramdisk.c \
         src/drivers/watchdog.c \
         src/drivers/netconsole.c \
         src/ipc/mutex.c \
         src/ipc/fifo.c \
         src/ipc/mqueue.c \
         src/fs/sysfs.c \
         src/fs/sysfs_devices.c \
         src/fs/sysfs_bus.c \
         src/fs/sysfs_class.c \
         src/fs/sysfs_firmware.c \
         src/fs/sysfs_numa.c \
         src/fs/debugfs.c \
         src/fs/tracefs.c \
         src/ipc/semaphore.c \
         src/kernel/audit.c \
         src/kernel/yama.c \
         src/kernel/kptr_restrict.c \
         src/kernel/dmesg.c \
         src/kernel/coredump_core.c \
         src/ipc/waitqueue.c \
         src/lib/string.c \
         src/lib/printf.c \
         src/lib/stdlib.c \
         src/lib/libc.c \
         src/lib/stdio.c \
         src/lib/unistd.c \
         src/lib/signal_libc.c \
         src/lib/pthread.c \
         src/lib/dlfcn.c \
         src/drivers/file_lock.c \
         src/drivers/kaps.c \
         src/drivers/pgrp.c \
         src/drivers/coredump.c \
         src/drivers/aio.c \
         src/drivers/signalfd.c \
         src/drivers/timerfd.c \
         src/drivers/hpet.c \
         src/drivers/dmi.c \
         src/drivers/bochs.c \
         src/drivers/loop.c \
         src/drivers/spi.c \
         src/drivers/dm.c \
         src/drivers/dm-linear.c \
         src/drivers/dm-zero.c \
         src/drivers/dm-error.c \
         src/drivers/dm-crypt.c \
         src/drivers/dm-verity.c \
         src/drivers/pmem.c \
         src/drivers/ipmi_kcs.c \
         src/drivers/tpm_tis.c \
         src/drivers/dyndbg.c \
         src/drivers/uio.c \
         src/drivers/pagecache.c \
         src/drivers/firmware_class.c \
         src/fs/freeze.c \
         src/fs/quota.c \
         src/net/dhcp.c \
         src/net/dhcp6.c \
         src/kernel/irq_affinity.c \
         src/kernel/tpm_rng.c \
         src/kernel/trace.c \
         src/kernel/trace_events.c \
         src/kernel/smap_smep_umip.c \
         src/kernel/uaccess.c \
         src/kernel/notifier.c \
         src/kernel/notifier_ext.c \
         src/kernel/softirq.c \
         src/kernel/tasklet.c \
         src/kernel/stacktrace.c \
         src/kernel/ratelimit.c \
         src/kernel/logbuf.c \
         src/lib/crc32.c \
         src/lib/uuid.c \
         src/lib/hexdump.c \
         src/lib/radix_tree.c \
         src/lib/bitmap.c \
         src/lib/find_bit.c \
         src/lib/mempool.c \
         src/lib/crc16.c \
         src/lib/string_ext.c \
         src/lib/time.c \
         src/lib/stdlib_ext.c \
         src/lib/search.c \
         src/lib/errno_ext.c \
         src/lib/assert.c \
         src/lib/base64.c \
         src/lib/aes.c \
         src/lib/aes_xts.c \
         src/lib/sha256.c \
         src/lib/sha512.c \
         src/lib/md5.c \
         src/lib/hmac.c \
         src/lib/crc64.c \
         src/lib/adler32.c \
         src/lib/stdlib_user.c \
         src/kernel/rwsem.c \
         src/kernel/x2apic.c \
         src/kernel/tsc_deadline.c \
         src/kernel/invpcid.c \
         src/kernel/fsgsbase.c \
         src/kernel/rdpid.c \
         src/kernel/nx_enforce.c \
         src/kernel/vsyscall.c \
         src/memory/memhotplug.c \
         src/memory/page_poison.c \
         src/memory/page_owner.c \
         src/memory/cma.c \
         src/memory/zram.c \
         src/memory/zram_writeback.c \
         src/memory/ksm.c \
         src/memory/thp.c \
         src/memory/hugepage_migration.c \
         src/memory/zcomp.c \
         src/memory/zcomp_fast.c \
         src/memory/zswap.c \
         src/memory/mglru.c \
         src/kernel/perf_events.c \
         src/kernel/jump_label.c \
         src/kernel/pstore.c \
         src/kernel/kdump.c \
         src/kernel/kexec.c \
         src/kernel/stack_guard.c \
         src/kernel/rseq.c \
         src/kernel/kcov.c \
         src/kernel/kprobes.c \
         src/kernel/ftrace.c \
         src/kernel/mce.c \
         src/kernel/mce_inject.c \
         src/kernel/kasan_light.c \
         src/kernel/kcsan.c \
         src/kernel/kfence.c \
         src/kernel/kmemleak.c \
         src/kernel/ima.c \
         src/kernel/ima_policy.c \
         src/kernel/ima_appraise.c \
         src/kernel/evm.c \
         src/kernel/ipe.c \
         src/kernel/keyring.c \
         src/kernel/compress.c \
         src/kernel/psi.c \
         src/kernel/firmware.c \
         src/kernel/memfd.c \
         src/kernel/mseal.c \
         src/kernel/stackleak.c \
         src/kernel/userfaultfd.c \
         src/kernel/madvise_ext.c \
         src/kernel/mem_policy.c \
         src/kernel/page_idle.c \
         src/kernel/page_allocator_ext.c \
         src/kernel/sched_attr.c \
         src/kernel/cpuset.c \
         src/kernel/pidfd.c \
         src/kernel/poll.c \
         src/kernel/epoll.c \
         src/kernel/landlock.c \
         src/kernel/seccomp_bpf.c \
         src/kernel/process_rlimit.c \
         src/kernel/devtmpfs.c \
         src/kernel/overlay.c \
         src/kernel/fanotify.c \
         src/kernel/fs_mount_prop.c \
         src/kernel/aio_enhanced.c \
         src/kernel/aio.c \
         src/kernel/range.c \
         src/kernel/hashtable.c \
         src/kernel/interval_tree.c \
         src/kernel/prio_tree.c \
         src/kernel/bitfield.c \
         src/kernel/hweight.c \
         src/kernel/sort_ext.c \
         src/kernel/timer_source.c \
         src/kernel/klist.c \
         src/kernel/cpu_bitmask.c \
         src/kernel/cpu_topology.c \
         src/kernel/llist.c \
         src/kernel/percpu_counter.c \
         src/kernel/ratelimit_ext.c \
         src/kernel/refcount_ext.c \
         src/kernel/dynamic_debug.c \
         src/kernel/irq_work.c \
         src/kernel/taskstats.c \
         src/kernel/irq_regs.c \
         src/kernel/timeconst.c \
         src/kernel/div64.c \
         src/kernel/gpiolib.c \
         src/kernel/io_map.c \
         src/kernel/io_uring.c \
         src/kernel/config_gz.c \
         src/kernel/fault_inject.c \
         src/kernel/kpti.c \
         src/test/kunit.c \
         src/test/kunit_memory.c \
         src/test/kunit_tests.c \
         src/test/kunit_pmm.c \
         src/test/kunit_slab.c \
         src/test/kunit_sched.c \
         src/test/kunit_vmm.c \
         src/test/kunit_security.c \
         src/test/kunit_security_new.c \
         src/test/kunit_power.c \
         src/test/kunit_ext.c \
         src/test/kunit_vfs.c \
         src/test/kunit_container_ext.c \
         src/test/kunit_net.c \
         src/test/kunit_errno.c \
         src/test/kunit_tls.c \
         src/test/kunit_coverage.c \
         src/test/kunit_regression.c \
         src/test/kunit_usb.c \
         src/container/runtime.c \
         src/container/config.c \
         src/container/state.c \
         src/container/ext.c \
         src/container/container_exec_enhanced.c \
         src/container/storage.c \
         src/container/network.c \
         src/container/image.c \
         src/container/orch.c \
         src/container/service_proxy.c \
         src/container/scheduler_policy.c \
         src/container/seccomp_notify.c \
         src/container/checkpoint.c \
         src/container/security_scan.c \
         src/orch/metrics.c \
         src/orch/log_shipper.c \
         src/orch/log_aggregator.c \
         src/orch/events.c \
         src/orch/tracing.c \
         src/orch/dashboard.c \
         src/orch/alerting.c \
         src/orch/manifest.c \
         src/orch/compose.c \
         src/orch/namespace.c \
         src/orch/rbac.c \
         src/orch/auth.c \
         src/orch/pod_security.c \
         src/orch/secrets.c \
         src/orch/pod_health.c \
         src/orch/hooks.c \
         src/drivers/xhci.c \
         src/drivers/xhci_streams.c \
         src/drivers/gpio_irq.c \
         src/drivers/iommu.c \
         src/kernel/sched_idle.c \
         src/kernel/hrtimer.c \
         src/kernel/idle_inject.c \
         src/kernel/core_sched.c \
         src/kernel/nohz.c \
         src/kernel/lockdown.c \
         src/kernel/pkey.c \
         src/kernel/pm_runtime.c \
         src/power/rapl.c \
         src/drivers/sndstat.c \
         src/drivers/acpi_platform_profile.c \
         src/drivers/acpi_fan.c \
         src/fs/overlay_enhance.c \
         src/net/ntp.c \
         src/net/smtp.c \
         src/net/dns_server.c \
         src/drivers/usb_hid.c \
         src/drivers/usb_hid_mt.c \
         src/drivers/usb_cdc_acm.c \
         src/drivers/usb_hub.c \
         src/boot/uefi_gop.c \
         src/boot/uefi_runtime.c \
         src/drivers/simplefb.c \
         src/drivers/usb_core.c \
         src/drivers/usb_transfer.c \
         src/drivers/usb_eth.c \
         src/drivers/gadget/udc_core.c \
         src/drivers/gadget/f_mass_storage.c \
         src/kernel/tpm_key.c \
         src/drivers/drm/drm_core.c \
         src/drivers/drm/drm_gem.c \
         src/drivers/drm/drm_dumb.c \
         src/drivers/drm/drm_atomic.c \
         src/drivers/drm/drm_fb.c \
         src/drivers/drm/drm_display.c \
         src/drivers/drm/bochs_drm.c \
         src/drivers/drm/simplefb_drm.c \
         src/drivers/drm/drm_prime.c \
         src/drivers/drm/drm_damage.c \
         src/drivers/drm/drm_fence.c \
         src/drivers/drm/drm_multi.c \
         src/drivers/drm/drm_irq.c \
         src/kernel/live_patch.c \
         src/kernel/cgroup.c \
         src/kernel/kaslr.c \
         src/kernel/mprotect.c \
         src/kernel/wx_enforce.c \
         src/kernel/dma_api.c \
         src/drivers/dm-raid.c \
         src/drivers/mpath.c \
         src/drivers/edac.c \
         src/drivers/ghes.c \
         src/drivers/i3c.c \
         src/fs/verity.c \
         src/fs/readdir.c \
         src/fs/hfs.c \
         src/fs/cramfs.c \
         src/fs/minix.c \
         src/fs/ufs.c \
         src/fs/sysv.c \
         src/fs/adfs.c \
         src/fs/bfs.c \
         src/fs/btrfs.c \
         src/fs/btrfs_csum.c \
         src/fs/ntfs.c \
         src/fs/exfat.c \
         src/fs/hfsplus.c \
         src/drivers/virtio_gpu.c \
         src/drivers/virtio_input.c \
         src/drivers/virtio_rng.c \
         src/drivers/virtio_balloon.c \
         src/drivers/virtio_scsi.c \
         src/drivers/virtio_console.c \
         src/drivers/pvpanic.c \
         src/drivers/ivshmem.c \
         src/drivers/9pnet_virtio.c \
         src/drivers/vmw_balloon.c \
         src/drivers/vmw_pvscsi.c \
         src/lib/chacha20.c \
         src/lib/poly1305.c \
         src/lib/chacha20poly1305.c \
         src/lib/aes_gcm.c \
         src/lib/ecc.c \
         src/lib/rsa.c \
         src/kernel/execshield.c \
         src/kernel/usercopy.c \
         src/kernel/randstruct.c \
         src/kernel/cfi.c \
         src/kernel/scs.c \
         src/kernel/blk_mq.c \
         src/kernel/ftrace_stack.c \
         src/kernel/hwlat_detector.c \
         src/kernel/perf_branch.c \
         src/kernel/kgdb_stub.c \
         src/memory/damon.c \
         src/memory/zbud.c \
         src/memory/zsmalloc.c \
         src/net/rds.c \
         src/net/vsock.c \
         src/net/openvswitch.c \
         src/net/fq_codel.c \
         src/net/cake.c \
         src/net/tcp_bbr2.c \
         src/net/tcp_bbr3.c \
         src/drivers/pcie_aer.c \
         src/drivers/pcie_dpc.c \
         src/drivers/pcie_ptm.c \
         src/drivers/sriov.c \
         src/drivers/bcache.c \
         src/drivers/dm_snapshot.c \
         src/drivers/dma-api.c \
         src/drivers/usb_uas.c \
         src/drivers/usb_serial.c \
         src/drivers/usb_cdc_ether.c \
         src/drivers/usb_wifi.c \
         src/drivers/acpi_cppc.c \
         src/drivers/powercap.c \
         src/drivers/i6300esb.c \
         src/power/suspend_s2idle.c \
         src/fs/nfs.c \
         src/fs/erofs.c \
         src/fs/f2fs.c \
         src/fs/jffs2.c \
         src/fs/nilfs2.c \
         src/kernel/numa_balancing.c \
         src/kernel/uprobes.c \
         src/kernel/early_serial.c \
         src/kernel/efi_runtime.c \
         src/memory/page_pool.c \
         src/memory/numa_mem.c \
         src/drivers/dm-era.c \
         src/drivers/dma_buf.c \
         src/drivers/bonding.c \
         src/kernel/ras_netlink.c \
         src/net/xdp.c \
         src/kernel/kvm.c \
         src/drivers/vhost_scsi.c \
         src/drivers/vhost_blk.c \
         src/drivers/vfio.c \
         src/drivers/virtio_fs.c \
         src/drivers/virtio_iommu.c \
         src/drivers/virtio_packed.c \
         src/drivers/vdpa.c \
         src/drivers/balloon.c

ASM_SRCS = src/boot/boot.asm \
           src/kernel/gdt_asm.asm \
           src/kernel/idt_asm.asm \
           src/kernel/syscall_asm.asm \
           src/process/switch.asm \
           src/kernel/ap_trampoline.asm \
           src/kernel/kretprobe_trampoline.asm

C_OBJS = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(C_SRCS))
ASM_OBJS = $(patsubst src/%.asm,$(BUILDDIR)/%.o,$(ASM_SRCS))
APP_SRCS = $(wildcard src/apps/*.c)
OBJS = $(ASM_OBJS) $(C_OBJS)
# Header dependency tracking: include .d files when they exist
DEPS = $(C_OBJS:.o=.d)

# ── Kernel module build rules (M39) ──────────────────────────────────
# Loadable kernel modules (.ko) are compiled with -DMODULE and partially
# linked via `ld -r` to produce relocatable files for the module loader.
#
# Module source files live alongside the regular kernel sources in src/.
# obj-m lists the module .ko names to build (without path or extension).
# Each module .ko is built from one or more .c source files compiled with
# -DMODULE, then partially linked together.
#
# Example usage:
#   make modules          # build all modules listed in obj-m
#   make modules_install  # copy .ko files into the disk image
#
# Module compilation flags: same as CFLAGS but with -DMODULE and without
# -nostdinc (modules include kernel headers via the same include paths).
MODULE_CFLAGS  = $(filter-out -nostdinc -fno-builtin -mno-mmx -mno-sse -mno-sse2 -fstack-protector-strong -mstack-protector-guard=global -mcmodel=large, $(CFLAGS)) \
                 -DMODULE -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -mcmodel=small -Isrc/include \
                 -Iuserspace/kmods/shell -Iuserspace/kmods/doom -Iuserspace/kmods/dos -Iuserspace/kmods/gui
MODULE_LDFLAGS = -r -z max-page-size=0x1000

# Build directory for module objects and .ko files
MODULE_BUILDDIR = $(BUILDDIR)/modules

# Module category sub-makefiles — moved from inline obj-m to category includes.
# See src/modules/Makefile.modules for the full module list organized by category.
# Each category sub-makefile (drivers.mk, net.mk, fs.mk, memory.mk, security.mk,
# kernel.mk, power.mk, container.mk, orch.mk, ipc.mk, shell.mk) defines its
# obj-m and <basename>-objs entries.
include src/modules/Makefile.modules

# Derive module .ko paths from obj-m list
MODULE_KOS = $(addprefix $(MODULE_BUILDDIR)/, $(obj-m))

# Multi-file module support: <basename>-objs lists .o files to link into <basename>.ko
# If <basename>-objs is not defined, the module is built from a single <basename>.o
# Examples:
#   doom-objs := doom/doom_task.o doom/doom_map.o doom/doom_raycast.o ...
#   (relative to MODULE_BUILDDIR, .o suffix)
#
# Function to get the object list for a given module .ko name ($1 = full .ko path)
# Single-file modules: return "$(basename $1).o" (e.g., build/modules/drivers/e1000.o)
# Multi-file modules:  return "$(MODULE_BUILDDIR)/$(component1).o $(MODULE_BUILDDIR)/$(component2).o ..."
module_objs_for = $(if $($(basename $(notdir $(1)))-objs), \
    $(addprefix $(MODULE_BUILDDIR)/, \
        $(addsuffix .o, \
            $(basename $($(basename $(notdir $(1)))-objs)))), \
    $(basename $(1)).o)

# Module .o files: expand each module to its component .o files
MODULE_OBJS = $(foreach ko,$(obj-m), \
    $(call module_objs_for,$(ko)))

# Rule to compile a module source file into a module .o file
# Source file is located under src/ relative to the .o path minus MODULE_BUILDDIR/
# e.g., build/modules/drivers/e1000.o ← src/drivers/e1000.c
# e.g., build/modules/doom/doom_task.o ← src/doom/doom_task.c
$(MODULE_BUILDDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MODULE_CFLAGS) -c $< -o $@

# Module sources can also live under userspace/kmods/ for programs that are
# separate from the kernel source tree but run as kernel modules.
# e.g., build/modules/shell/shell.o ← userspace/kmods/shell/shell.c
$(MODULE_BUILDDIR)/%.o: userspace/kmods/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MODULE_CFLAGS) -c $< -o $@

# Rule to partially-link module .o(s) into a .ko relocatable file.
# Supports multi-source modules via <basename>-objs variable.
# Uses .SECONDEXPANSION so $$* expands to the module stem (e.g. "doom" from "doom.ko")
.SECONDEXPANSION:
$(MODULE_KOS): $(MODULE_BUILDDIR)/%.ko: $$(call module_objs_for,$(MODULE_BUILDDIR)/$$*.ko)
	@mkdir -p $(dir $@)
	$(LD) $(MODULE_LDFLAGS) -o $@ $^

# Build all kernel modules listed in obj-m
modules: $(MODULE_KOS)

# Install built modules into the filesystem disk image
# Copies .ko files to /modules/ on the FAT32 disk image (where the
# kernel's module loader expects to find them at runtime).
modules_install: modules
	@echo "=== Installing modules ==="
	@mkdir -p /tmp/modules_staging
	@for ko in $(MODULE_KOS); do \
	    if [ -f "$$ko" ]; then \
	        name=$$(basename $$ko); \
	        echo "  INSTALL $$name"; \
	        cp "$$ko" /tmp/modules_staging/$$name; \
	    fi; \
	done
	@echo "Modules staged in /tmp/modules_staging/"
	@echo "Run: scripts/install_modules.sh <disk_image> to add to disk image"

# ── /proc/config.gz — embedded compressed kernel build config ──────
#
# Pipeline: gen_config.sh → build_config.txt → gzip → build_config.gz
#           → xxd -i → build/build_config_gz.h (included by config_gz.c)
#
BUILD_CONFIG_TXT  = $(BUILDDIR)/build_config.txt
BUILD_CONFIG_GZ   = $(BUILDDIR)/build_config.gz
BUILD_CONFIG_GZ_H = $(BUILDDIR)/build_config_gz.h

# The header must be generated before config_gz.c is compiled.
# We add the build directory as an include path so #include "build_config_gz.h" resolves.
$(BUILDDIR)/kernel/config_gz.o: CFLAGS += -I$(BUILDDIR)

# Generate the header: config text → gzip → xxd -i → header
# Write to a temp file and atomically rename to avoid parallel-build races
$(BUILD_CONFIG_GZ_H): $(BUILD_CONFIG_GZ)
	@mkdir -p $(dir $@)
	@{ \
	    echo '/* Auto-generated — do not edit. */'; \
	    echo '#ifndef BUILD_CONFIG_GZ_H'; \
	    echo '#define BUILD_CONFIG_GZ_H'; \
	    xxd -i -n build_config_gz < $<; \
	    echo '#endif /* BUILD_CONFIG_GZ_H */'; \
	} > $@.tmp && mv $@.tmp $@

$(BUILD_CONFIG_GZ): $(BUILD_CONFIG_TXT)
	gzip -c < $< > $@

$(BUILD_CONFIG_TXT): scripts/gen_config.sh
	@mkdir -p $(dir $@)
	scripts/gen_config.sh $@

# ── KPTI trampoline — flat binary embedded in kernel ─────────────
# Pipeline: kpti_trampoline.asm → nasm -f bin → xxd -i → header
#
KPTI_TRAMP_SRC = src/kernel/kpti_trampoline.asm
KPTI_TRAMP_BIN = $(BUILDDIR)/kpti_trampoline.bin
KPTI_TRAMP_H   = $(BUILDDIR)/kpti_trampoline_bin.h

$(KPTI_TRAMP_BIN): $(KPTI_TRAMP_SRC) | $(BUILDDIR)
	nasm -f bin -Wno-number-overflow -o $@ $<

$(KPTI_TRAMP_H): $(KPTI_TRAMP_BIN)
	@mkdir -p $(dir $@)
	@{ \
	    echo '/* Auto-generated — do not edit. */'; \
	    echo '#ifndef KPTI_TRAMPOLINE_BIN_H'; \
	    echo '#define KPTI_TRAMPOLINE_BIN_H'; \
	    xxd -i -n kpti_trampoline_bin < $<; \
	    echo '#endif /* KPTI_TRAMPOLINE_BIN_H */'; \
	} > $@.tmp && mv $@.tmp $@

# kpti.c needs the generated header
$(BUILDDIR)/kernel/kpti.o: CFLAGS += -I$(BUILDDIR)
$(BUILDDIR)/kernel/kpti.o: $(KPTI_TRAMP_H)

# ── Default target: build kernel in parallel ──────────────────────────
# NOTE: -include must stay BELOW the default target so that dependency
# files never accidentally steal .DEFAULT_GOAL.

.DEFAULT_GOAL := all

all: $(BUILDDIR)/disk.img
	$(MAKE) -j$(NPROCS) $(BUILDDIR)/kernel.bin

# Build kernel.elf only (skip disk image)
kernel: $(BUILDDIR)/kernel.elf
	@echo "Kernel built: $<"

-include $(wildcard $(DEPS))

# ── Phony targets ─────────────────────────────────────────────────────

.PHONY: all kernel \
        run run-smp run-gdb run-uefi run-virtio qemu qemu-gdb \
        help debug clean deps \
        test test-kernel test-serial test-cli test-clean test-coverage clean-all \
        nic-test \
        check check-full check-clean check-app-boundary check-debug fsck-test doom-test \
        format format-check check-whitespace lint lint-full cppcheck-check \
        ccache-stats count build-info count-lines count-funcs count-headers \
        run-test unit-test junit-test bench \
        e2e e2e-smoke e2e-test e2e-list \
        stress stress-help \
        modules modules_install build-strict analyze cppcheck cppcheck-all \
        clang-tidy-check ctags etags doccheck sparse todo \
        release dist \
        install install-clean \
        clean-kernel clean-test verify

# ── Boundary check on app sources ─────────────────────────────────────

check-app-boundary:
	@if [ -z "$(APP_SRCS)" ]; then exit 0; fi; \
	bad=$$(rg --pcre2 -n '^#include "(?!libc\.h|shell_cmds\.h|shell_cmd_table\.h|shell\.h|printf\.h|string\.h|stdlib\.h|types\.h|keyboard\.h|blockdev\.h|fat32\.h|ata\.h|ahci\.h|service\.h|fault\.h|syscall\.h|vfs\.h|module\.h|module_elf\.h|heap\.h|ssh\.h|ssh_client\.h|vfs\.h|sysctl\.h|users\.h|net\.h|fstab\.h|devtmpfs\.h|nvme\.h|vga\.h|errno\.h|fsck\.h|dm\.h|container\.h|spinlock\.h|process\.h|timer\.h|scheduler\.h|elf\.h|orch_api\.h|oci_spec\.h|seccomp\.h|crypto\.h|json\.h|signal\.h|ext2\.h|socket\
.h|pmm\.h|ac97\.h|loop\.h|ftrace\.h|kprobes\.h|trace\.h|perf_events\.h|firmware\.h|watchdog\.h|timers\.h|lockdown\.h|ioprio\.h|netdevice\.h|freeze\.h|fbcon\.h|string_ext\.h|dhcp\.h|caps\.h)' $(APP_SRCS) 2>/dev/null || true); \
	if [ -n "$$bad" ]; then \
	    echo "ERROR: App source includes an unexpected header."; \
	    echo "Allowed headers: libc.h, shell_cmds.h, shell_cmd_table.h, shell.h, printf.h,"; \
	    echo "  string.h, stdlib.h, types.h, keyboard.h, blockdev.h, fat32.h, ata.h,"; \
	    echo "  ahci.h, service.h, fault.h, heap.h, devtmpfs.h, nvme.h, dm.h"; \
	    echo "Offending files:"; \
	    echo "$$bad"; \
	    exit 1; \
	fi

# ── check-debug: Build with all CONFIG_DEBUG options enabled ────────
#
# Builds the kernel with all debugging and hardening options forced on,
# treating compiler warnings as errors.  This is the 'max debug' build
# used to catch latent bugs before release.
#
# If the 'analyze' target is available (requires cppcheck/Clang SA),
# static analysis is also performed.
check-debug:
	@echo "=== check-debug: Building with all debug options ==="
	$(MAKE) CFLAGS_EXTRA="-DCONFIG_DEBUG_STACK_USAGE -DCONFIG_DEBUG_PAGEALLOC -DCONFIG_DEBUG_SPINLOCK -DCONFIG_DEBUG_ATOMIC_SLEEP -DCONFIG_DEBUG_KMEMLEAK -DCONFIG_DEBUG_FAULT_INJECT -Werror" \
	       -j$(NPROCS) $(BUILDDIR)/kernel.bin 2>&1
	@echo "=== check-debug: Build successful ==="
	@# If static analysis tools are available, run them
	@if which cppcheck >/dev/null 2>&1; then \
	    echo "=== check-debug: Running cppcheck static analysis ==="; \
	    cppcheck --enable=all --inconclusive --suppress=missingIncludeSystem \
	        -I src/include -I src/gui -I src/doom \
	        --std=c17 --platform=unix64 \
	        src/kernel/*.c src/memory/*.c src/process/*.c \
	        2>&1 || true; \
	fi
	@if which clang-tidy >/dev/null 2>&1; then \
	    echo "=== check-debug: Running clang-tidy (limited scope) ==="; \
	    for f in src/kernel/fault.c src/kernel/lockdep.c src/kernel/kmemleak.c src/memory/pmm.c; do \
	        clang-tidy --quiet --extra-arg=-ffreestanding \
	            --extra-arg=-Isrc/include \
	            "$$f" 2>/dev/null || true; \
	    done; \
	fi
	@echo "=== check-debug: All checks passed ==="

# ── ext2 fsck validation test ─────────────────────────────────────────
# Generates ext2 images with all feature combinations using
# scripts/mkext2img.py and validates them structurally.
fsck-test:
	@echo "=== Running ext2 fsck validation tests ==="
	@python3 scripts/test_ext2_fsck.py
	@echo "=== fsck-test completed ===\n"

# ── ext4 fsck validation test ─────────────────────────────────────────
# Generates ext4 images with all feature combinations using
# scripts/mkext4img.py and validates them structurally with fsck.ext4.
ext4-fsck-test:
	@echo "=== Running ext4 fsck validation tests ==="
	@python3 scripts/test_ext4_fsck.py
	@echo "=== ext4-fsck-test completed ==="

# ── Precompiled headers (PCH, Item 258) ──────────────────────────────
#
# Precompiling kernel_pch.h into a .gch file speeds up full builds by
# ~2-3x because GCC can load the parsed header tree from a single
# serialised image instead of parsing 20+ headers per translation unit.
#
# The .gch file is produced by compiling the header with the same flags
# used for .c files but with -x c-header.  GCC automatically uses the
# .gch when it encounters `#include "kernel_pch.h"` (or when the header
# is force-included via -include, as we do in CFLAGS above).
#
# Note: The PCH is shared across all kernel .o files, so it must be
# built *before* any .c is compiled.  It is listed as a prerequisite of
# kernel.elf (the top-level target) to ensure ordering.
PCH_HEADER = src/include/kernel_pch.h
PCH_FILE   = src/include/kernel_pch.h.gch

# PCH compilation flags: same as CFLAGS but WITHOUT -include kernel_pch.h
# (which would cause a self-include loop) and WITHOUT -MMD/-MP (GCC's
# -MMD generates wrong deps for -x c-header when combined with -include).
PCH_CFLAGS = $(filter-out -include kernel_pch.h -MMD -MP, $(CFLAGS))

# Rebuild the PCH when the header itself or any headers it includes change.
# We generate a .d file listing header dependencies.
$(PCH_FILE): $(PCH_HEADER)
	@mkdir -p $(BUILDDIR)
	$(CC) $(PCH_CFLAGS) -MM -MP -MF $(BUILDDIR)/kernel_pch.d -x c-header $<
	$(CC) $(PCH_CFLAGS) -x c-header $< -o $@

# Include the auto-generated dependency file for the PCH (if it exists)
-include $(BUILDDIR)/kernel_pch.d

# ── Compilation rules ─────────────────────────────────────────────────

$(BUILDDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: src/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILDDIR)/kernel.elf: check-app-boundary $(PCH_FILE) $(OBJS)
	@mkdir -p $(BUILDDIR)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)

# config_gz.o depends on the auto-generated header
$(BUILDDIR)/kernel/config_gz.o: $(BUILD_CONFIG_GZ_H)

$(BUILDDIR)/kernel.bin: $(BUILDDIR)/kernel.elf
	cp $< $@

# ── Userspace init binary (standalone ELF for kernel loader) ─────────

# Userspace init is now built from userspace/init/
USERSPACE_BINS = userspace/init.elf

# Root filesystem staging directory
ROOTFS_DIR = $(BUILDDIR)/rootfs

# ── Disk image — full root filesystem with commands ────────────────
# Uses mkfatimg.py to create a FAT32 image from a staged root directory
# containing /sbin/init, /bin/sh, /bin/* (all command ELFs), and /etc/inittab.

DISK_IMG_SIZE_MB ?= 64

# Build userspace commands
.PHONY: userspace-build
userspace-build:
	@$(MAKE) -C userspace all 2>&1 | grep -v "is up to date" || true

# Stage files into rootfs directory
ROOTFS_STAMP = $(BUILDDIR)/.rootfs_stamp

$(ROOTFS_STAMP): userspace-build
	@$(MAKE) -k modules 2>/dev/null; true
	# ^ module builds are best-effort; disk.img needs userspace only
	@rm -rf $(ROOTFS_DIR)
	@mkdir -p $(ROOTFS_DIR)/sbin $(ROOTFS_DIR)/bin $(ROOTFS_DIR)/etc $(ROOTFS_DIR)/tmp $(ROOTFS_DIR)/modules
	# Copy userspace-built init as /sbin/init
	cp userspace/init.elf $(ROOTFS_DIR)/sbin/init
	# Copy init2 and initramfs as /sbin/
	cp userspace/init2.elf $(ROOTFS_DIR)/sbin/init2
	cp userspace/initramfs.elf $(ROOTFS_DIR)/sbin/initramfs
	# Copy userspace shell as /bin/sh
	cp userspace/sh.elf $(ROOTFS_DIR)/bin/sh
	# Copy all userspace command ELFs to /bin/
	@for f in userspace/*.elf; do \
		name=$$(basename $$f .elf); \
		[ "$$name" = "init" ] && continue; \
		[ "$$name" = "sh" ] && continue; \
		[ "$$name" = "init2" ] && continue; \
		[ "$$name" = "initramfs" ] && continue; \
		cp $$f $(ROOTFS_DIR)/bin/$$name; \
	done
	# Copy kernel modules (.ko) to /modules/
	@for ko in shell.ko; do \
		if [ -f $(MODULE_BUILDDIR)/$$ko ]; then \
			cp $(MODULE_BUILDDIR)/$$ko $(ROOTFS_DIR)/modules/$$ko; \
			echo "[rootfs] Copied module $$ko"; \
		fi; \
	done
	# Strip ELFs to save space
	@if command -v x86_64-linux-gnu-strip >/dev/null 2>&1; then \
		echo "[rootfs] Stripping ELF binaries..."; \
		x86_64-linux-gnu-strip $(ROOTFS_DIR)/sbin/init $(ROOTFS_DIR)/sbin/init2 $(ROOTFS_DIR)/sbin/initramfs $(ROOTFS_DIR)/bin/* 2>/dev/null || true; \
	fi
	# Create /etc/inittab (kernel also creates a default at boot)
	@echo 'ttyS0::respawn:/bin/sh' > $(ROOTFS_DIR)/etc/inittab
	@echo 'console::askfirst:/bin/sh' >> $(ROOTFS_DIR)/etc/inittab
	@touch $@
	@echo "[rootfs] Staged $(shell find $(ROOTFS_DIR) -type f | wc -l) files in $(ROOTFS_DIR)"

$(BUILDDIR)/disk.img: $(ROOTFS_STAMP)
	@mkdir -p $(BUILDDIR)
	@python3 scripts/mkfatimg.py $@ $(DISK_IMG_SIZE_MB) $(ROOTFS_DIR)
	@echo "[disk] Created $@ ($(DISK_IMG_SIZE_MB) MB)"

# ── Run targets ───────────────────────────────────────────────────────

run: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	qemu-system-x86_64 -cpu max,-x2apic -smp 2 -kernel $(BUILDDIR)/kernel.bin -m 256M -serial stdio -vga std \
		-display cocoa -k en-us \
		-drive file=$(BUILDDIR)/disk.img,format=raw,if=ide \
		-netdev user,id=net0 -device e1000,netdev=net0 ; \
	stty sane

run-virtio: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	qemu-system-x86_64 -cpu max,-x2apic -smp 2 -kernel $(BUILDDIR)/kernel.bin -m 256M -serial stdio -vga std \
		-drive file=$(BUILDDIR)/disk.img,format=raw,if=ide \
		-netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
		-no-reboot

# ── Test build (separate output dir, compiled with -DTEST_MODE) ──────

BUILDDIR_TEST = build_test
TEST_CFLAGS   = $(CFLAGS) -DTEST_MODE

# Optional: skip ATA disk tests (faster in TCG, no disk needed)
ifneq ($(SKIP_DISK_TESTS),)
TEST_CFLAGS += -DSKIP_DISK_TESTS
endif

C_TEST_SRCS  = $(C_SRCS) $(CMD_SRCS) $(COMPILER_SRCS) $(GUI_SRCS) $(DOOM_SRCS) src/test/test.c
ASM_TEST_SRCS = $(ASM_SRCS)

C_TEST_OBJS  = $(patsubst src/%.c,$(BUILDDIR_TEST)/%.o,$(C_TEST_SRCS))
ASM_TEST_OBJS = $(patsubst src/%.asm,$(BUILDDIR_TEST)/%.o,$(ASM_TEST_SRCS))
TEST_OBJS    = $(ASM_TEST_OBJS) $(C_TEST_OBJS)

# Test build variant config
BUILD_CONFIG_GZ_H_TEST = $(BUILDDIR_TEST)/build_config_gz.h
$(BUILDDIR_TEST)/kernel/config_gz.o: CFLAGS += -I$(BUILDDIR_TEST)
$(BUILDDIR_TEST)/kernel/kpti.o: CFLAGS += -I$(BUILDDIR)
$(BUILDDIR_TEST)/kernel/kpti.o: $(KPTI_TRAMP_H)
$(BUILD_CONFIG_GZ_H_TEST): $(BUILD_CONFIG_GZ)
	@mkdir -p $(dir $@)
	@echo '/* Auto-generated — do not edit. */' > $@
	@echo '#ifndef BUILD_CONFIG_GZ_H' >> $@
	@echo '#define BUILD_CONFIG_GZ_H' >> $@
	xxd -i -n build_config_gz < $< >> $@
	@echo '#endif /* BUILD_CONFIG_GZ_H */' >> $@

$(BUILDDIR_TEST)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(BUILDDIR_TEST)/%.o: src/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILDDIR_TEST)/kernel.elf: check-app-boundary $(TEST_OBJS)
	@mkdir -p $(BUILDDIR_TEST)
	$(LD) $(LDFLAGS) -o $@ $(TEST_OBJS) $(LIBGCC)

# config_gz.o depends on the auto-generated header for test build too
$(BUILDDIR_TEST)/kernel/config_gz.o: $(BUILD_CONFIG_GZ_H_TEST)

$(BUILDDIR_TEST)/kernel.bin: $(BUILDDIR_TEST)/kernel.elf
	cp $< $@

# Build the test kernel binary (parallel via recursive make)
test-kernel:
	$(MAKE) -j$(NPROCS) $(BUILDDIR_TEST)/kernel.bin

# Run headless QEMU on serial TCP 4444 for manual inspection of test kernel
test-serial: $(BUILDDIR_TEST)/kernel.bin $(BUILDDIR)/disk.img
	qemu-system-x86_64 -kernel $(BUILDDIR_TEST)/kernel.bin -m 256M \
		-serial tcp::4444,server,nowait \
		-vga none -display none \
		-drive file=$(BUILDDIR)/disk.img,format=raw,if=ide \
		-netdev user,id=net0 -device e1000,netdev=net0 \
		-no-reboot

# Run tests: clean build + host-side unit tests + KUnit + QEMU boot test
test: $(BUILDDIR)/disk.img
	@echo "=== Building test kernel (clean build) ==="
	$(MAKE) -j$(NPROCS) test-kernel
	@echo ""
	@echo "=== Running host-side unit tests ==="
	$(MAKE) unit-test
	@echo ""
	@echo "=== Running QEMU boot test ==="
	python3 src/test/boot_test.py \
		--kernel $(BUILDDIR_TEST)/kernel.bin \
		--disk $(BUILDDIR)/disk.img \
		--timeout 30
	@echo ""
	@echo "=== Running KUnit test runner (if serial log available) ==="
	@if [ -f /tmp/qemu-test-*.txt ]; then \
		chmod +x scripts/run_kunit.sh; \
		scripts/run_kunit.sh --log /tmp/qemu-test-*.txt --quiet || true; \
	fi
	@echo ""
	@echo "============================================"
	@echo "  make test completed"
	@echo "============================================"

# ── Multi-NIC QEMU boot test ─────────────────────────────────
# Boots the kernel with each supported NIC model and verifies
# the kernel boots and the NIC driver initialises successfully.
nic-test: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	@echo "=== Running multi-NIC QEMU boot test ==="
	python3 src/test/multi_nic_test.py \
		--kernel $(BUILDDIR)/kernel.bin \
		--disk $(BUILDDIR)/disk.img \
		--timeout 45 \
		--verbose
	@echo ""

# ── Test CLI utilities ────────────────────────────────────────
# Builds everything, injects test script into rootfs, boots QEMU with
# SMP and runs every compiled command in the VM, reporting PASS/FAIL.
test-cli:
	@python3 src/test/test_cli.py --timeout 60 --smp 2

# ── Stress test target ─────────────────────────────────────────
# Builds the stress test ELFs, injects them into the disk image,
# replaces /etc/inittab to auto-run stress tests at boot,
# and boots QEMU to execute them.
# Override STRESS_DURATION to change per-test duration (default: 20s).
# Example: make stress STRESS_DURATION=60
STRESS_DURATION ?= 20

stress: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	@echo "=== Building stress test ELFs ==="
	$(MAKE) -C src/test/stress all 2>&1 | tail -5
	@echo ""
	@echo "=== Injecting stress test ELFs into disk image ==="
	mcopy -i $(BUILDDIR)/disk.img -o src/test/stress/stress_cpu.elf ::/bin/stress_cpu 2>/dev/null || \
	    echo "[stress] Warning: mcopy stress_cpu.elf failed"
	mcopy -i $(BUILDDIR)/disk.img -o src/test/stress/stress_memory.elf ::/bin/stress_memory 2>/dev/null || \
	    echo "[stress] Warning: mcopy stress_memory.elf failed"
	mcopy -i $(BUILDDIR)/disk.img -o src/test/stress/stress_disk.elf ::/bin/stress_disk 2>/dev/null || \
	    echo "[stress] Warning: mcopy stress_disk.elf failed"
	mcopy -i $(BUILDDIR)/disk.img -o src/test/stress/stress_runner.elf ::/stress_runner 2>/dev/null || \
	    echo "[stress] Warning: mcopy stress_runner.elf failed"
	@echo ""
	@echo "=== Creating stress boot inittab ==="
	@# Create inittab that runs stress tests via the runner, then spawns shell
	@echo '::sysinit:/stress_runner' > /tmp/stress_inittab
	@echo 'ttyS0::respawn:/bin/sh' >> /tmp/stress_inittab
	@echo 'console::askfirst:/bin/sh' >> /tmp/stress_inittab
	mcopy -i $(BUILDDIR)/disk.img -o /tmp/stress_inittab ::/etc/inittab 2>/dev/null || true
	@echo ""
	@echo "=== Running stress tests in QEMU ==="
	@echo "  Per-test duration: $(STRESS_DURATION)s"
	@echo "  Kernel: $(BUILDDIR)/kernel.bin"
	@echo "  Disk:   $(BUILDDIR)/disk.img"
	@echo ""
	@# Run QEMU headless.  The isa-debug-exit device enables clean shutdown.
	@# The kernel boots normally, init runs /etc/inittab which starts
	@# /stress_runner as a sysinit task.
	qemu-system-x86_64 -cpu max,-x2apic \
		-kernel $(BUILDDIR)/kernel.bin \
		-m 256M \
		-serial stdio \
		-vga none -display none \
		-drive file=$(BUILDDIR)/disk.img,format=raw,if=ide \
		-netdev user,id=net0 -device e1000,netdev=net0 \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-no-reboot 2>&1 || true
	@echo ""
	@echo "=== Stress tests completed ==="

# Show help for stress target
stress-help:
	@echo "Stress test targets:"
	@echo "  make stress             Run all stress tests in QEMU (default: 20s each)"
	@echo "  make stress STRESS_DURATION=60  Run with 60s per test"
	@echo ""
	@echo "The stress target builds stress ELFs, injects them into the disk image,"
	@echo "creates a boot inittab that runs the stress runner, and boots QEMU."

# ── E2E test: boot + interactive command tests ─────────────────────
e2e-test: $(BUILDDIR)/kernel.bin
	python3 src/test/e2e_test.py --kernel $(BUILDDIR)/kernel.bin

e2e-list:
	python3 src/test/e2e_test.py --list

.PHONY: e2e-test e2e-list

# ── Check target: full build with -Werror + run all tests ──────────────
CHECK_CFLAGS = $(CFLAGS) -Werror
BUILDDIR_CHECK = build_check

C_CHECK_SRCS  = $(C_SRCS) $(CMD_SRCS) $(COMPILER_SRCS) $(GUI_SRCS) $(DOOM_SRCS) src/test/test.c
ASM_CHECK_SRCS = $(ASM_SRCS)

C_CHECK_OBJS  = $(patsubst src/%.c,$(BUILDDIR_CHECK)/%.o,$(C_CHECK_SRCS))
ASM_CHECK_OBJS = $(patsubst src/%.asm,$(BUILDDIR_CHECK)/%.o,$(ASM_CHECK_SRCS))
CHECK_OBJS    = $(ASM_CHECK_OBJS) $(C_CHECK_OBJS)

$(BUILDDIR_CHECK)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CHECK_CFLAGS) -c $< -o $@

$(BUILDDIR_CHECK)/%.o: src/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILDDIR_CHECK)/kernel.elf: check-app-boundary $(CHECK_OBJS)
	@mkdir -p $(BUILDDIR_CHECK)
	$(LD) $(LDFLAGS) -o $@ $(CHECK_OBJS) $(LIBGCC)

# config_gz.o depends on the auto-generated header for check build too
$(BUILDDIR_CHECK)/kernel/config_gz.o: $(BUILD_CONFIG_GZ_H_TEST)
$(BUILDDIR_CHECK)/kernel/config_gz.o: CHECK_CFLAGS += -I$(BUILDDIR_CHECK) -I$(BUILDDIR_TEST)

$(BUILDDIR_CHECK)/kernel.bin: $(BUILDDIR_CHECK)/kernel.elf
	cp $< $@

check: $(BUILDDIR)/disk.img unit-test
	$(MAKE) -j$(NPROCS) $(BUILDDIR_CHECK)/kernel.bin
	@chmod +x tests/run_tests.sh
	@./tests/run_tests.sh $(BUILDDIR_CHECK)/kernel.bin $(BUILDDIR)/disk.img
	@echo ""
	@echo "=== Build-time tests passed, running E2E smoke test ==="
	$(MAKE) e2e-smoke

# ── check-full: build with ALL strict warning flags ───────────────
CHECK_FULL_CFLAGS = $(CFLAGS) -Werror -Wpedantic -Wconversion -Wshadow \
                    -Wformat=2 -Wundef -Wcast-align -Wstrict-prototypes \
                    -Wold-style-definition
BUILDDIR_CHECK_FULL = build_check_full

C_CHECK_FULL_SRCS  = $(C_SRCS) $(CMD_SRCS) $(COMPILER_SRCS) $(GUI_SRCS) $(DOOM_SRCS) src/test/test.c
ASM_CHECK_FULL_SRCS = $(ASM_SRCS)

C_CHECK_FULL_OBJS  = $(patsubst src/%.c,$(BUILDDIR_CHECK_FULL)/%.o,$(C_CHECK_FULL_SRCS))
ASM_CHECK_FULL_OBJS = $(patsubst src/%.asm,$(BUILDDIR_CHECK_FULL)/%.o,$(ASM_CHECK_FULL_SRCS))
CHECK_FULL_OBJS    = $(ASM_CHECK_FULL_OBJS) $(C_CHECK_FULL_OBJS)

$(BUILDDIR_CHECK_FULL)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CHECK_FULL_CFLAGS) -c $< -o $@

$(BUILDDIR_CHECK_FULL)/%.o: src/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILDDIR_CHECK_FULL)/kernel.elf: check-app-boundary $(CHECK_FULL_OBJS)
	@mkdir -p $(BUILDDIR_CHECK_FULL)
	$(LD) $(LDFLAGS) -o $@ $(CHECK_FULL_OBJS) $(LIBGCC)

$(BUILDDIR_CHECK_FULL)/kernel/config_gz.o: $(BUILD_CONFIG_GZ_H_TEST)
$(BUILDDIR_CHECK_FULL)/kernel/config_gz.o: CHECK_FULL_CFLAGS += -I$(BUILDDIR_CHECK_FULL) -I$(BUILDDIR_TEST)

$(BUILDDIR_CHECK_FULL)/kernel.bin: $(BUILDDIR_CHECK_FULL)/kernel.elf
	cp $< $@

check-full: $(BUILDDIR)/disk.img
	@echo "=== check-full: building with ALL strict warning flags ==="
	$(MAKE) -j$(NPROCS) $(BUILDDIR_CHECK_FULL)/kernel.bin
	@echo "=== check-full build complete (kernel.bin at $(BUILDDIR_CHECK_FULL)/kernel.bin) ==="
	@rm -rf $(BUILDDIR_CHECK_FULL)

# Clean check and check-full build artifacts
check-clean:
	rm -rf $(BUILDDIR_CHECK) $(BUILDDIR_CHECK_FULL)

# ── Host-side unit tests (compiled with host gcc, no kernel deps) ───
unit-test:
	@echo "=== Host-side unit tests ==="
	$(MAKE) -C tests/host_libc all
	$(MAKE) -C tests/unit all

# JUnit XML test reporting (for CI)
# Usage: make junit-test JUNIT_DIR=build/test-reports
junit-test:
	@echo "=== Host-side unit tests (JUnit XML output) ==="
	mkdir -p $(JUNIT_DIR)
	$(MAKE) -C tests/host_libc all JUNIT_DIR=$(JUNIT_DIR)
	$(MAKE) -C tests/unit all JUNIT_DIR=$(JUNIT_DIR)
	@echo "JUnit XML reports in $(JUNIT_DIR)/"

# Full clean rebuild + test
test-clean: clean
	$(MAKE) test

# ── Test with code coverage ─────────────────────────────────────────────
test-coverage: CFLAGS += -fprofile-arcs -ftest-coverage --coverage
test-coverage: LDFLAGS += --coverage
test-coverage: clean
	@echo "=== Building with code coverage (-fprofile-arcs -ftest-coverage) ==="
	$(MAKE) -j$(NPROCS) test-kernel
	@echo ""
	@echo "=== Running tests with coverage instrumentation ==="
	$(MAKE) unit-test
	@echo ""
	@echo "=== Coverage data written to build_test/ directory ==="
	@echo "Run: gcov -o build_test/ src/kernel/*.c  (per-file coverage)"
	@echo "Or:  lcov -c -d build_test/ -o coverage.info && genhtml coverage.info -o coverage/"

# E2E tests: boot normal kernel in QEMU with user-mode networking + telnet hostfwd
e2e: $(BUILDDIR)/disk.img
	$(MAKE) -j$(NPROCS) $(BUILDDIR)/kernel.bin
	@chmod +x tests/e2e.sh tests/e2e.py
	@./tests/e2e.sh $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img

# E2E smoke test: fast CI subset of e2e tests
e2e-smoke: $(BUILDDIR)/disk.img
	$(MAKE) -j$(NPROCS) $(BUILDDIR)/kernel.bin
	@chmod +x tests/e2e.sh tests/e2e.py
	@E2E_SMOKE=1 ./tests/e2e.sh $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img

# Fast run: build test-kernel and run tests in one invocation
run-test:
	$(MAKE) test

# Fast pre-merge verification: format check + static analysis + app boundary check
verify:
	$(MAKE) format-check
	$(MAKE) lint
	$(MAKE) check-app-boundary

# Verify doom framebuffer (PCI BAR0) renders non-black pixels in QEMU -vga std
doom-test: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	@chmod +x tests/doom_fb.sh
	@./tests/doom_fb.sh $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img

# E2E with explicit telnet port (override default 2323)
e2e-port-%: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	@chmod +x tests/e2e.sh tests/e2e.py
	@E2E_PORT=$* ./tests/e2e.sh $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img

# ── Debug target ──────────────────────────────────────────────────────

debug: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	qemu-system-x86_64 -kernel $(BUILDDIR)/kernel.bin -m 256M -serial stdio -vga std -s -S \
		-drive file=$(BUILDDIR)/disk.img,format=raw,if=ide \
		-netdev user,id=net0 -device e1000,netdev=net0

# ── Install target (Item 264) — build a bootable ISO with GRUB ────────
#
# Creates a bootable ISO image at $(BUILDDIR)/hermes.iso that can be
# written to USB or CD/DVD and booted on real hardware via GRUB.
# Prerequisites: grub-mkrescue, xorriso.
#
# Usage:
#   make install           # build kernel + disk.img + ISO
#   make install ISO=1     # same (ISO only)
#   make install USB=/dev/sdX   # build ISO then write to raw device
#
# The ISO uses the multiboot protocol (kernel already has a multiboot1
# header in src/boot/boot.asm) so any GRUB-compatible bootloader can
# load it.

# Where to install the ISO output
INSTALL_ISO ?= $(BUILDDIR)/hermes.iso

# GRUB configuration template
GRUB_CFG ?= scripts/grub.cfg

# Staging directory for ISO contents
ISO_STAGING ?= $(BUILDDIR)/iso_staging

# ── Build ISO image ───────────────────────────────────────────────────
# 1. Copy kernel + modules + disk image into a staging directory
# 2. Place GRUB config in boot/grub/
# 3. Run grub-mkrescue to produce a bootable ISO

$(INSTALL_ISO): $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img modules
	@echo "=== Building bootable ISO ==="
	@mkdir -p $(ISO_STAGING)/boot/grub
	@mkdir -p $(ISO_STAGING)/modules
	@cp $(BUILDDIR)/kernel.bin $(ISO_STAGING)/boot/kernel.elf
	@cp $(GRUB_CFG) $(ISO_STAGING)/boot/grub/grub.cfg
	@# Copy disk image (FAT32 with init, modules, etc.)
	@cp $(BUILDDIR)/disk.img $(ISO_STAGING)/boot/disk.img
	@# Copy kernel modules
	@for ko in $(MODULE_KOS); do \
		if [ -f "$$ko" ]; then \
			cp "$$ko" $(ISO_STAGING)/modules/; \
		fi \
	done
	@# Generate grub-mkrescue config snippet to embed the disk image
	@echo "*** Build ISO with grub-mkrescue ***"
	@if command -v grub-mkrescue >/dev/null 2>&1; then \
		grub-mkrescue -o $@ $(ISO_STAGING) 2>&1; \
		echo "=== ISO created: $@ ==="; \
		echo "    Write to USB: dd if=$@ of=/dev/sdX bs=4M status=progress"; \
	else \
		echo "*** grub-mkrescue not found — install xorriso + grub-common"; \
		exit 1; \
	fi
	@rm -rf $(ISO_STAGING)

# ── Install to USB device ────────────────────────────────────────────
# Usage: make install USB=/dev/sdX
# WARNING: This will OVERWRITE the target device!
install: $(INSTALL_ISO)
	@if [ -n "$(USB)" ]; then \
		echo "=== Writing ISO to $(USB) ==="; \
		echo "WARNING: This will overwrite $(USB)!"; \
		dd if=$(INSTALL_ISO) of=$(USB) bs=4M status=progress; \
		sync; \
		echo "=== Done: $(USB) is now bootable ==="; \
	else \
		echo "=== Bootable ISO ready: $(INSTALL_ISO) ==="; \
		echo "    Write to USB: make install USB=/dev/sdX"; \
	fi

# ── Clean ISO artifacts ──────────────────────────────────────────────
install-clean:
	rm -rf $(INSTALL_ISO) $(ISO_STAGING)

# ── Release target: build kernel + disk img + source tarball ─────────
#
# Creates a release-ready archive:
#   build/release-<version>/kernel.bin
#   build/release-<version>/disk.img
#   build/release-<version>/hermes-<version>-src.tar.gz
#
RELEASE_VERSION ?= $(shell git describe --tags --dirty --always 2>/dev/null || echo "snapshot-$(shell date +%Y%m%d)")
RELEASE_DIR = $(BUILDDIR)/release-$(RELEASE_VERSION)
RELEASE_TAR = $(BUILDDIR)/hermes-$(RELEASE_VERSION)-src.tar.gz

release: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	@echo "=== Creating release $(RELEASE_VERSION) ==="
	@mkdir -p $(RELEASE_DIR)
	@cp $(BUILDDIR)/kernel.bin $(RELEASE_DIR)/
	@cp $(BUILDDIR)/disk.img $(RELEASE_DIR)/
	@# Create source tarball
	@if command -v git >/dev/null 2>&1 && git rev-parse --git-dir >/dev/null 2>&1; then \
		git archive --format=tar.gz --prefix=hermes-$(RELEASE_VERSION)/ -o $(RELEASE_TAR) HEAD 2>/dev/null || \
		tar czf $(RELEASE_TAR) --exclude=.git --exclude='build/*' --exclude='build_test/*' \
			--exclude='build_check*' --exclude='build_analyze' \
			-C .. $(notdir $(CURDIR)) 2>/dev/null; \
	else \
		tar czf $(RELEASE_TAR) --exclude=.git --exclude='build/*' --exclude='build_test/*' \
			--exclude='build_check*' --exclude='build_analyze' \
			-C .. $(notdir $(CURDIR)); \
	fi
	@cp $(RELEASE_TAR) $(RELEASE_DIR)/
	@echo "=== Release $(RELEASE_VERSION) created in $(RELEASE_DIR)/ ==="
	@ls -lh $(RELEASE_DIR)/

# ── Dist target: create a distribution tarball with source + binaries ──
#
# Creates a single tarball containing source code and pre-built binaries
# suitable for distribution to end users.
#
DIST_DIR = $(BUILDDIR)/dist-$(RELEASE_VERSION)
DIST_TAR = $(BUILDDIR)/hermes-$(RELEASE_VERSION)-dist.tar.gz

dist: release
	@echo "=== Creating distribution tarball ==="
	@mkdir -p $(DIST_DIR)
	@cp $(RELEASE_DIR)/kernel.bin $(DIST_DIR)/
	@cp $(RELEASE_DIR)/disk.img $(DIST_DIR)/
	@cp $(RELEASE_TAR) $(DIST_DIR)/
	@# Add a README.dist
	@echo "Hermes OS $(RELEASE_VERSION)" > $(DIST_DIR)/VERSION
	@echo "---" >> $(DIST_DIR)/VERSION
	@echo "Built: $$(date)" >> $(DIST_DIR)/VERSION
	@echo "Kernel: $$(ls -lh $(DIST_DIR)/kernel.bin | awk '{print $$5}')" >> $(DIST_DIR)/VERSION
	@echo "Disk:   $$(ls -lh $(DIST_DIR)/disk.img | awk '{print $$5}')" >> $(DIST_DIR)/VERSION
	@cd $(BUILDDIR) && \
		tar czf $(DIST_TAR) -C $(BUILDDIR) dist-$(RELEASE_VERSION) && \
		echo "=== Distribution tarball: $(DIST_TAR) ===" && \
		ls -lh $(DIST_TAR)
	@rm -rf $(DIST_DIR)

# ── New run targets (SMP, GDB, UEFI) ──────────────────────────────────

run-smp: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	qemu-system-x86_64 -cpu max -smp 4 -kernel $(BUILDDIR)/kernel.bin -m 256M -serial stdio -vga std \
		-display cocoa -k en-us \
		-drive file=$(BUILDDIR)/disk.img,format=raw,if=ide \
		-netdev user,id=net0 -device e1000,netdev=net0 ; \
	stty sane

run-gdb: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	qemu-system-x86_64 -kernel $(BUILDDIR)/kernel.bin -m 256M -serial stdio -vga std -s -S \
		-drive file=$(BUILDDIR)/disk.img,format=raw,if=ide \
		-netdev user,id=net0 -device e1000,netdev=net0

run-uefi: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -kernel $(BUILDDIR)/kernel.bin -m 256M -serial stdio -vga std \
		-display cocoa -k en-us \
		-drive file=$(BUILDDIR)/disk.img,format=raw,if=ide \
		-netdev user,id=net0 -device e1000,netdev=net0 ; \
	stty sane

# Alias for run target
qemu: run

# Alias for run-gdb (GDB stub on port 1234)
qemu-gdb: run-gdb

# ── Help target: list all major targets ──────────────────────────────

help:
	@echo "=== Hermes OS Build System ==="
	@echo ""
	@echo "=== Kernel Build Targets ==="
	@echo "  all (default)   - Build kernel.elf + kernel.bin + disk image"
	@echo "  kernel          - Build kernel.elf only"
	@echo "  clean           - Remove build/ and build_test/ artifacts"
	@echo "  clean-kernel    - Remove only kernel build artifacts (build/)"
	@echo "  clean-test      - Remove only test build artifacts (build_test/)"
	@echo "  clean-all       - clean + clear ccache statistics"
	@echo "  modules         - Build loadable kernel modules (.ko)"
	@echo "  modules_install - Stage .ko files for disk image installation"
	@echo "  userspace-build - Build userspace commands into ELF binaries"
	@echo "  release         - Build kernel.bin + disk.img + source tarball"
	@echo "  dist            - Create distribution tarball with source + binaries"
	@echo "  install         - Build bootable ISO (or write to USB via make install USB=/dev/sdX)"
	@echo "  install-clean   - Remove ISO and staging artifacts"
	@echo ""
	@echo "=== Run Targets ==="
	@echo "  qemu            - Boot kernel in QEMU (alias for run)"
	@echo "  qemu-gdb        - Boot kernel in QEMU with GDB stub (port 1234, alias for run-gdb)"
	@echo "  run             - Boot in QEMU (serial stdio, e1000 NIC)"
	@echo "  run-smp         - Boot QEMU with SMP (4 CPUs, -cpu max)"
	@echo "  run-gdb         - Boot QEMU with GDB stub (-s -S)"
	@echo "  run-uefi        - Boot QEMU with UEFI firmware (OVMF)"
	@echo "  run-virtio      - Boot QEMU with virtio-net"
	@echo "  debug           - Boot QEMU with GDB stub (alias for run-gdb)"
	@echo ""
	@echo "=== Testing Targets ==="
	@echo "  test            - Build test kernel + run all tests in QEMU"
	@echo "  test-kernel     - Build test kernel (separate build_test/ output dir)"
	@echo "  test-serial     - Run test kernel with serial TCP output"
	@echo "  test-clean      - Clean + rebuild + run tests"
	@echo "  test-coverage   - Build with -fprofile-arcs -ftest-coverage and run tests"
	@echo "  run-test        - Alias for 'make test'"
	@echo "  verify          - Fast pre-merge: format-check + lint + app boundary check"
	@echo "  stress          - Run all stress tests in QEMU"
	@echo "  stress-help     - Show help for stress target"
	@echo "  check           - Strict build (-Werror) + tests + E2E smoke"
	@echo "  check-full      - Ultra-strict build (-Werror + -Wpedantic + all warnings)"
	@echo "  check-debug     - Build with all debug options enabled"
	@echo "  check-clean     - Remove build_check/ and build_check_full/ artifacts"
	@echo "  check-app-boundary  - Verify app source includes only allowed headers"
	@echo "  unit-test       - Run host-side unit tests"
	@echo "  e2e             - Run E2E QEMU smoke tests"
	@echo "  e2e-smoke       - Fast CI E2E subset"
	@echo "  e2e-test        - Run E2E boot + interactive command tests"
	@echo "  e2e-list        - List E2E test case descriptions"
	@echo "  doom-test       - Verify DOOM framebuffer renders non-black pixels"
	@echo "  junit-test      - Run unit tests with JUnit XML output"
	@echo ""
	@echo "=== Static Analysis Targets ==="
	@echo "  cppcheck-all    - Run cppcheck with warning/performance/style checks"
	@echo "  cppcheck        - Run cppcheck with --enable=all on whole src/"
	@echo "  cppcheck-check  - Run cppcheck with suppressions (called by lint)"
	@echo "  lint            - Run cppcheck-check + clang-tidy-check"
	@echo "  clang-tidy-check - Run clang-tidy on first 20 C sources"
	@echo "  analyze         - GCC -fanalyzer static analysis"
	@echo "  build-strict    - Alias for cppcheck"
	@echo "  sparse          - Run sparse semantic parser on all C sources"
	@echo "  format          - Run clang-format on all .c and .h files"
	@echo "  format-check    - Check format compliance (via git-clang-format)"
	@echo "  check-whitespace - Check for trailing whitespace in source files"
	@echo ""
	@echo "=== Code Metrics Targets ==="
	@echo "  count-lines     - Count lines of code per subsystem"
	@echo "  count-funcs     - Count function definitions per subsystem"
	@echo "  count-headers   - Count .h files per subsystem"
	@echo "  count           - Show C/asm/header/test file and line totals"
	@echo "  build-info      - Show kernel size, object count, source file count, LOC"
	@echo "  ccache-stats    - Show ccache hit rate and statistics"
	@echo ""
	@echo "=== Developer Utility Targets ==="
	@echo "  ctags           - Generate ctags for src/"
	@echo "  etags           - Generate Emacs TAGS for src/"
	@echo "  todo            - Show TODO/FIXME/HACK/XXX/BUG markers in src/"
	@echo "  doccheck        - Verify documentation files exist and are valid"
	@echo "  deps            - Print build dependency install command (brew)"

# ── Clean targets ─────────────────────────────────────────────────────

clean:
	rm -rf $(BUILDDIR) $(BUILDDIR_TEST)
	rm -f $(PCH_FILE) $(BUILDDIR)/kernel_pch.d

# Clean everything including ccache statistics
clean-all: clean
	@if command -v ccache >/dev/null 2>&1; then \
		ccache --clear 2>/dev/null; \
		ccache --zero-stats 2>/dev/null; \
		echo "ccache stats cleared."; \
	fi

# Clean only kernel build artifacts under build/ (keep tests, userspace)
clean-kernel:
	rm -rf $(BUILDDIR)
	rm -f $(PCH_FILE) $(BUILDDIR)/kernel_pch.d

# Clean only test build artifacts under build_test/
clean-test:
	rm -rf $(BUILDDIR_TEST)

# ── Format: run clang-format on all .c and .h files ───────────────────

FORMAT_FILES := $(shell find src/ -type f \( -name '*.c' -o -name '*.h' \) | sort)

format:
	@if command -v clang-format >/dev/null 2>&1; then \
		clang-format -i $(FORMAT_FILES); \
		echo "Formatted $(words $(FORMAT_FILES)) files in src/"; \
	else \
		echo "clang-format not found — install with: sudo apt install clang-format"; \
	fi

# ── Format check: verify code matches clang-format style ────────────
# Uses git-clang-format to check only lines that differ from the base
# ref (default: HEAD~1). Exits with 1 if any formatting issues found.

format-check:
	@if command -v git-clang-format >/dev/null 2>&1; then \
		BASE_REF="$${FORMAT_BASE_REF:-HEAD~1}"; \
		echo "Checking code formatting against $${BASE_REF}..."; \
		git clang-format --diff "$${BASE_REF}" 2>/dev/null > /tmp/format_diff.$$$$; \
		if [ -s /tmp/format_diff.$$$$ ]; then \
			echo "❌ Code formatting issues found:"; \
			cat /tmp/format_diff.$$$$; \
			rm -f /tmp/format_diff.$$$$; \
			exit 1; \
		else \
			echo "✅ Code formatting is clean."; \
			rm -f /tmp/format_diff.$$$$; \
		fi \
	else \
		echo "git-clang-format not found. Install it (e.g., apt install clang-format) and try again."; \
		exit 1; \
	fi

# ── Check trailing whitespace in source files ─────────────────────────
check-whitespace:
	@echo "=== Checking for trailing whitespace ==="
	@errors=0; \
	for ext in c h asm py sh Makefile; do \
		find . -name "*.$$ext" -not -path "./.git/*" -not -path "./build/*" -not -path "./build_test/*" -not -path "./build_check*" -print0 2>/dev/null | \
		xargs -0 grep -l '[[:space:]]$$' 2>/dev/null | \
		while read f; do \
			echo "  TRAILING WHITESPACE: $$f"; \
			errors=$$((errors + 1)); \
		done; \
	done; \
	if [ "$$errors" -eq 0 ]; then \
		echo "  ✅ No trailing whitespace found."; \
	else \
		echo "  ❌ $$errors file(s) with trailing whitespace."; \
		exit 1; \
	fi

# ── Lint: run cppcheck + clang-tidy on all C sources ──────────────────

.PHONY: lint clang-tidy-check

lint:
	@$(MAKE) cppcheck-check
	@$(MAKE) clang-tidy-check

cppcheck-check:
	@if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --enable=all \
		  --suppress=unusedFunction \
		  --suppress=constVariablePointer \
		  --suppress=constParameterPointer \
		  --suppress=variableScope \
		  --suppress=unreadVariable \
		  --suppress=arrayIndexThenCheck \
		  --suppress=redundantInitialization \
		  --suppress=knownConditionTrueFalse \
		  --suppress=badBitmaskCheck \
		  --suppress=unusedStructMember \
		  --suppress=constVariable \
		  --suppress=shadowVariable \
		  --suppress=redundantAssignment \
		  --suppress=oppositeInnerCondition \
		  --suppress=zerodivcond \
		  --suppress=staticStringCompare \
		  --suppress=unsignedLessThanZero \
		  --suppress=arrayIndexOutOfBoundsCond \
		  --suppress=constParameterCallback \
		  --suppress=clarifyCondition \
		  --suppress=redundantCondition \
		  --suppress=duplicateValueTernary \
		  --suppress=useStandardLibrary \
		  --suppress=checkersReport \
		  --suppress=checkLevelNormal \
		  --std=c17 \
		  -Isrc/include -Iuserspace/kmods/gui -Iuserspace/kmods/doom \
		  --inline-suppr \
		  --error-exitcode=1 \
		  src/; \
	else \
		echo "cppcheck not found. Install it (e.g., apt install cppcheck) and try again."; \
		exit 1; \
	fi

clang-tidy-check:
	@if command -v clang-tidy >/dev/null 2>&1; then \
		echo "Running clang-tidy static analysis (first 20 C sources)..."; \
		SRCS=""; \
		count=0; \
		for f in $(C_SRCS); do \
			if [ $$count -ge 20 ]; then break; fi; \
			SRCS="$$SRCS $$f"; \
			count=$$((count + 1)); \
		done; \
		clang-tidy --quiet --warnings-as-errors="*" \
		  --extra-arg="-std=c17" \
		  --extra-arg="-ffreestanding" \
		  --extra-arg="-Isrc/include" \
		  --extra-arg="-Iuserspace/kmods/gui" \
		  --extra-arg="-Iuserspace/kmods/doom" \
		  $$SRCS 2>/dev/null | tail -20 || true; \
		echo "clang-tidy finished (checked $$count files)"; \
	else \
		echo "clang-tidy not found. Install it (e.g., apt install clang-tidy) and try again."; \
	fi

# ── Static analysis with GCC -fanalyzer (Item 261) ─────────────────────
#
# Builds the kernel with GCC's -fanalyzer for deep static analysis
# (control flow, data flow, resource leaks, null dereference, etc.).
# Since -fanalyzer can produce false positives in a freestanding kernel
# environment, the default mode warns without failing the build.
#
# Use:
#   make analyze           # build with -fanalyzer, report findings (non-fatal)
#   make analyze WERROR=1  # build with -fanalyzer -Werror (fatal on findings)
#
# NOTE: -fanalyzer only works with GCC >= 10.  Clang silently ignores it.
# PCH is disabled during analysis because GCC may produce spurious issues
# with precompiled headers under -fanalyzer.

ANALYZE_CFLAGS = $(filter-out -include kernel_pch.h, $(CFLAGS)) -fanalyzer $(if $(WERROR),-Werror,) \
                 -Wno-analyzer-malloc-leak \
                 -include src/include/errno.h -include src/include/string.h \
                 -include src/include/printf.h -include src/include/pmm.h \
                 -include src/include/vmm.h -include src/include/heap.h \
                 -include src/include/slab.h -include src/include/process.h \
                 -include src/include/scheduler.h -include src/include/spinlock.h \
                 -include src/include/mutex.h -include src/include/export.h
BUILDDIR_ANALYZE = build_analyze

C_ANALYZE_SRCS  = $(C_SRCS) $(CMD_SRCS) $(COMPILER_SRCS) $(GUI_SRCS) $(DOOM_SRCS) src/test/test.c
ASM_ANALYZE_SRCS = $(ASM_SRCS)

C_ANALYZE_OBJS  = $(patsubst src/%.c,$(BUILDDIR_ANALYZE)/%.o,$(C_ANALYZE_SRCS))
ASM_ANALYZE_OBJS = $(patsubst src/%.asm,$(BUILDDIR_ANALYZE)/%.o,$(ASM_ANALYZE_SRCS))
ANALYZE_OBJS    = $(ASM_ANALYZE_OBJS) $(C_ANALYZE_OBJS)

$(BUILDDIR_ANALYZE)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ANALYZE_CFLAGS) -c $< -o $@

$(BUILDDIR_ANALYZE)/%.o: src/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILDDIR_ANALYZE)/kernel.elf: check-app-boundary $(PCH_FILE) $(ANALYZE_OBJS)
	@mkdir -p $(BUILDDIR_ANALYZE)
	$(LD) $(LDFLAGS) -o $@ $(ANALYZE_OBJS) $(LIBGCC)

# config_gz.o depends on the auto-generated header for analyze build too
$(BUILDDIR_ANALYZE)/kernel/config_gz.o: $(BUILD_CONFIG_GZ_H)
$(BUILDDIR_ANALYZE)/kernel/config_gz.o: ANALYZE_CFLAGS += -I$(BUILDDIR)
$(BUILDDIR_ANALYZE)/kernel/config_gz.o: ANALYZE_CFLAGS += -I$(BUILDDIR_ANALYZE)

# kpti.o needs the trampoline header for analyze build
$(BUILDDIR_ANALYZE)/kernel/kpti.o: ANALYZE_CFLAGS += -I$(BUILDDIR)
$(BUILDDIR_ANALYZE)/kernel/kpti.o: $(KPTI_TRAMP_H)

$(BUILDDIR_ANALYZE)/kernel.bin: $(BUILDDIR_ANALYZE)/kernel.elf
	cp $< $@

analyze:
	@echo "=== GCC -fanalyzer static analysis ==="
	@echo "Note: -fanalyzer may report issues in freestanding kernel code."
	@echo "Review findings manually rather than relying solely on exit code.\n"
	@$(MAKE) -j$(NPROCS) $(BUILDDIR_ANALYZE)/kernel.bin || \
	  (echo "\nWARNING: -fanalyzer found issues (some may be false positives)"; exit 1)
	@echo "OK -fanalyzer analysis complete - no issues found"
	@rm -rf $(BUILDDIR_ANALYZE)

# ── Dependencies ──────────────────────────────────────────────────────

deps:
	brew install x86_64-elf-gcc nasm qemu xorriso

# ── Build info & stats ─────────────────────────────────────────────────

build-info:
	@echo "=== Build Info ==="
	@echo "Kernel size: $$(ls -lh $(BUILDDIR)/kernel.bin 2>/dev/null | awk '{print $$5}')"
	@echo "Object count: $$(find $(BUILDDIR) -name '*.o' 2>/dev/null | wc -l)"
	@echo "Source files: $$(find src -name '*.c' -o -name '*.asm' -o -name '*.h' | wc -l)"
	@echo "Total LOC: $$(find src -name '*.c' -o -name '*.asm' -o -name '*.h' | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}')"

# ── Source code line count ─────────────────────────────────────────────

count:
	@echo "=== Source Code Statistics ==="
	@echo "  C sources:  $$(find src -name '*.c' | wc -l) files, $$(find src -name '*.c' | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}') lines"
	@echo "  Assembly:   $$(find src -name '*.asm' | wc -l) files, $$(find src -name '*.asm' | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}') lines"
	@echo "  Headers:    $$(find src -name '*.h' | wc -l) files, $$(find src -name '*.h' | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}') lines"
	@echo "  Tests:      $$(find tests -name '*.c' -o -name '*.sh' -o -name '*.py' | wc -l) files"

# ── Line counts by subsystem ────────────────────────────────────────────

count-lines:
	@echo "=== Line counts by subsystem ==="; \
	for dir in kernel drivers fs net lib memory process ipc shell cluster; do \
		count=$$(find src/$$dir -name '*.c' -o -name '*.h' -o -name '*.asm' -o -name '*.S' 2>/dev/null | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}'); \
		echo "  $$dir: $$count"; \
	done

# ── Function definitions by subsystem ──────────────────────────────────────

count-funcs:
	@echo "=== Function count per subsystem ==="
	@for dir in kernel memory process fs net drivers ipc lib shell boot; do \
		count=$$(grep -rE '^(int|void|uint|bool|size_t|ssize_t|long|unsigned|uint8_t|uint16_t|uint32_t|uint64_t|uintptr_t|char|const|static|struct|enum)\b.*\(' src/$$dir/ --include='*.c' 2>/dev/null | grep -v '//' | wc -l); \
		printf "  %-12s %4d functions\n" "$$dir" $$count; \
	done

# ── Header files per subsystem ─────────────────────────────────────────────

count-headers:
	@echo "=== Header files per subsystem ==="; \
	for dir in kernel drivers fs net lib memory process ipc shell include; do \
		count=$$(find src/$$dir -name '*.h' 2>/dev/null | wc -l); \
		echo "  $$dir: $$count .h files"; \
	done; \
	total=$$(find src -name '*.h' 2>/dev/null | wc -l); \
	echo "  -------------------"; \
	echo "  total: $$total .h files"

# ── ccache statistics ──────────────────────────────────────────────────

ccache-stats:
	@if command -v ccache >/dev/null 2>&1; then \
		ccache --show-stats; \
	else \
		echo "ccache not installed."; \
	fi

# ── Static analysis (cppcheck) ──────────────────────────────────────────

build-strict: cppcheck

.PHONY: cppcheck
cppcheck:
	cppcheck --enable=all --suppress=missingIncludeSystem -Isrc/include --std=c17 --platform=unix64 src/ 2>&1 | tee build/cppcheck-report.txt

# ── Run cppcheck on kernel/memory/process/net/fs sources ─────────────────
cppcheck-all:
	@if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --enable=warning,performance,style --error-exitcode=1 \
			--suppress=missingIncludeSystem \
			-I src/include \
			src/ 2>&1; \
		echo "=== cppcheck-all complete ===\n"; \
	else \
		echo "cppcheck not found — install with: sudo apt install cppcheck"; \
	fi

# ── Sparse semantic parser ───────────────────────────────────────────────
#
# Sparse is a semantic parser for C that checks for common kernel coding
# errors: incorrect address-space annotations, endianness mismatches,
# missing __user/__iomem/__force casts, NULL pointer dereferences, and
# other static analysis warnings.
#
# Usage:
#   make sparse            # Run sparse on all C source files
#   make sparse C=1        # Check currently-unsolved problems
#   make sparse C=2        # Even more checks
#
# Install sparse from your distribution package manager:
#   apt install sparse          # Debian/Ubuntu
#   dnf install sparse          # Fedora
#   pacman -S sparse            # Arch
#

SPARSE_FLAGS ?= -Wsparse-all -Wno-bitwise-pointer -Wno-ptr-subtraction-blows \
                -DCONFIG_64BIT -D__x86_64__ -D__linux__ -D__CHECKER__ \
                -nostdinc -Isrc/include

.PHONY: sparse
sparse:
	@if ! command -v sparse >/dev/null 2>&1; then \
	    echo "⚠️  sparse is not installed."; \
	    echo "   Install it with: sudo apt install sparse"; \
	    echo "   (or equivalent for your package manager)"; \
	    exit 1; \
	fi
	@echo "=== Running sparse semantic parser on all C sources ==="
	@errors=0; \
	for src in $(C_SRCS) $(CMD_SRCS) $(COMPILER_SRCS) $(GUI_SRCS) $(DOOM_SRCS); do \
	    result=0; \
	    sparse $(SPARSE_FLAGS) $(CFLAGS_EXTRA) $$src 2>&1 || result=1; \
	    if [ $$result -ne 0 ]; then \
	        errors=$$((errors + 1)); \
	    fi; \
	done; \
	if [ $$errors -eq 0 ]; then \
	    echo "✅ sparse: no warnings."; \
	else \
	    echo "⚠️  sparse: $$errors file(s) produced warnings (see above)."; \
	fi

# ── ctags (source tags) ─────────────────────────────────────────────────

.PHONY: ctags etags
ctags:
	ctags -R src/
etags:
	ctags -R --output-format=etags src/

# ── TODO/FIXME/HACK/XXX/BUG finder ──────────────────────────────────────

.PHONY: todo
todo:
	@echo "=== TODO/FIXME/HACK/XXX/BUG Report for src/ ==="
	@echo ""
	@echo "--- BUG (critical) ---"
	@BUG_COUNT=0; \
	results=$$(grep -rn 'BUG' src/ --include='*.c' --include='*.h' 2>/dev/null | grep -v 'FIXME\|DEBUG\|BUG_ON\|TTRBUG\|KERN_DEBUG\|usb_debug\|debugfs\|pr_debug\|dbg_\|\.bug\|bug_table\|oops_\|spelling' || true); \
	if [ -n "$$results" ]; then \
		echo "$$results" | head -40; \
		BUG_COUNT=$$(echo "$$results" | wc -l); \
		echo "  [$$BUG_COUNT BUG markers found]"; \
	else \
		echo "  (none)"; \
	fi
	@echo ""
	@echo "--- FIXME (high) ---"
	@FIXME_COUNT=0; \
	results=$$(grep -rn 'FIXME' src/ --include='*.c' --include='*.h' 2>/dev/null | grep -v 'FIXME_INODE\|FIXME_MEMMAP\|spelling' || true); \
	if [ -n "$$results" ]; then \
		echo "$$results" | head -40; \
		FIXME_COUNT=$$(echo "$$results" | wc -l); \
		echo "  [$$FIXME_COUNT FIXME markers found]"; \
	else \
		echo "  (none)"; \
	fi
	@echo ""
	@echo "--- HACK (medium) ---"
	@HACK_COUNT=0; \
	results=$$(grep -rn 'HACK' src/ --include='*.c' --include='*.h' 2>/dev/null | grep -v 'HACK_\|SHACK\|HACKING\|spelling\|HACKERS' || true); \
	if [ -n "$$results" ]; then \
		echo "$$results" | head -40; \
		HACK_COUNT=$$(echo "$$results" | wc -l); \
		echo "  [$$HACK_COUNT HACK markers found]"; \
	else \
		echo "  (none)"; \
	fi
	@echo ""
	@echo "--- TODO (low) ---"
	@TODO_COUNT=0; \
	results=$$(grep -rn 'TODO' src/ --include='*.c' --include='*.h' 2>/dev/null | grep -v 'TODOLIST\|todolist\|TODO:.*context\|\.todo\|TODO_FILE\|spelling\|TODOs\|todolist\|SYSCALL_DEFINE.*TODO\|TODO!\|todo_kick\|todo_list\|TODOLIST' || true); \
	if [ -n "$$results" ]; then \
		echo "$$results" | head -40; \
		TODO_COUNT=$$(echo "$$results" | wc -l); \
		echo "  [$$TODO_COUNT TODO markers found]"; \
	else \
		echo "  (none)"; \
	fi
	@echo ""
	@echo "--- XXX (low) ---"
	@XXX_COUNT=0; \
	results=$$(grep -rn 'XXX' src/ --include='*.c' --include='*.h' 2>/dev/null | grep -v 'XXX_\|TXXX\|spelling' || true); \
	if [ -n "$$results" ]; then \
		echo "$$results" | head -40; \
		XXX_COUNT=$$(echo "$$results" | wc -l); \
		echo "  [$$XXX_COUNT XXX markers found]"; \
	else \
		echo "  (none)"; \
	fi
	@echo ""
	@echo "=== Summary ==="
	@grep -rn 'BUG\|FIXME\|HACK\|TODO\|XXX' src/ --include='*.c' --include='*.h' 2>/dev/null | \
		grep -v 'FIXME_INODE\|FIXME_MEMMAP\|BUG_ON\|DEBUG\|TTRBUG\|KERN_DEBUG\|SHACK\|HACK_\|HACKING\|HACKERS\|TODOLIST\|\.todo\|TODO_FILE\|TODOs\|todolist\|XXX_\|TXXX\|spelling\|todo_kick\|todo_list\|SYSCALL_DEFINE.*TODO\|dbg_\|debugfs\|pr_debug\|usb_debug\|oops_\|bug_table\|\.bug' | \
		awk '{ \
			if ($$0 ~ / BUG/) bugs++; \
			else if ($$0 ~ /FIXME/) fixmes++; \
			else if ($$0 ~ /HACK/) hacks++; \
			else if ($$0 ~ /TODO/) todos++; \
			else if ($$0 ~ /XXX/) xxxs++; \
		} END { \
			printf "  BUG:   %d  (critical)\\n", bugs; \
			printf "  FIXME: %d  (high)\\n", fixmes; \
			printf "  HACK:  %d  (medium)\\n", hacks; \
			printf "  TODO:  %d  (low)\\n", todos; \
			printf "  XXX:   %d  (low)\\n", xxxs; \
			printf "  Total: %d\\n", bugs+fixmes+hacks+todos+xxxs; \
		}'

# ── Documentation check ──────────────────────────────────────────────────

.PHONY: doccheck
doccheck:
	@echo "=== Documentation Check ==="
	@errors=0; \
	missing=; \
	for f in ARCHITECTURE.md README.md docs/DRIVER_API.md; do \
	    if [ ! -f "$$f" ]; then \
	        missing="$$missing $$f"; \
	        errors=$$((errors + 1)); \
	    fi; \
	done; \
	if [ -n "$$missing" ]; then \
	    echo "❌ Missing required documentation:$$missing"; \
	else \
	    echo "✅ All required documentation files present."; \
	fi; \
	md_files=$$(find . -maxdepth 2 -name '*.md' -not -path './.git/*' 2>/dev/null); \
	markdown_bad=0; \
	for f in $$md_files; do \
	    if ! grep -q '^#\|^##\|^###\|^####\|^#####\|^######' "$$f" 2>/dev/null; then \
	        echo "⚠️  $$f: no headers found (may not be valid markdown)"; \
	        markdown_bad=$$((markdown_bad + 1)); \
	    fi; \
	done; \
	if [ "$$markdown_bad" -gt 0 ]; then \
	    echo "⚠️  $$markdown_bad file(s) may have markdown issues"; \
	fi; \
	todo_docs=0; \
	for f in $$md_files; do \
	    if grep -n 'TODO\|FIXME\|HACK\|XXX\|BUG' "$$f" 2>/dev/null | grep -v -i 'syscall.*num\|signature\|\.todo' > /dev/null 2>&1; then \
	        count=$$(grep -c 'TODO\|FIXME\|HACK\|XXX\|BUG' "$$f" 2>/dev/null); \
	        echo "⚠️  $$f: $$count TODO/FIXME/HACK markers found"; \
	        todo_docs=$$((todo_docs + 1)); \
	    fi; \
	done; \
	if [ "$$todo_docs" -gt 0 ]; then \
	    echo "⚠️  $$todo_docs documentation file(s) contain TODO/FIXME markers (these should be resolved)"; \
	fi; \
	if [ "$$errors" -gt 0 ]; then \
	    echo "❌ doccheck FAILED — $$errors error(s)"; \
	    exit 1; \
	else \
	    echo "✅ doccheck PASSED"; \
	fi
