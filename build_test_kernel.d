build_test_kernel.o: src/kernel/kernel.c src/include/types.h \
 src/include/vga.h src/include/types.h src/include/gdt.h \
 src/include/idt.h src/include/pic.h src/include/fault.h \
 src/include/process.h src/include/signal.h src/include/timer.h \
 src/include/keyboard.h src/include/pmm.h src/include/vmm.h \
 src/include/heap.h src/include/process.h src/include/scheduler.h \
 src/include/shell.h src/include/serial.h src/include/printf.h \
 src/include/io.h src/include/ata.h src/include/fs.h src/include/pci.h \
 src/include/e1000.h src/include/net.h src/include/netfilter.h \
 src/include/pkt_sched.h src/include/bridge.h src/include/vlan.h \
 src/include/tun.h src/include/net_ns.h src/include/net_internal.h \
 src/include/net.h src/include/ipip.h src/include/wireguard.h \
 src/include/ipvs.h src/include/telnetd.h src/include/ssh.h \
 src/include/rtc.h src/include/mouse.h src/include/speaker.h \
 src/include/acpi.h src/include/syscall.h src/include/ahci.h \
 src/include/usb.h src/include/usb_msc.h src/include/service.h \
 src/include/httpd.h src/gui/gui.h src/include/intel_gpu.h \
 src/include/fat32.h src/include/users.h src/include/vfs.h \
 src/include/errno.h src/include/pipe.h src/include/waitqueue.h \
 src/include/spinlock.h src/include/io.h src/include/blockdev.h \
 src/include/shm.h src/include/virtio_net.h src/include/virtio_blk.h \
 src/include/ac97.h src/include/smp.h src/include/scheduler.h \
 src/include/gdt.h src/include/apic.h src/include/idt.h src/include/elf.h \
 src/include/cpu.h src/include/cpu_features.h src/include/x2apic.h \
 src/include/tsc_deadline.h src/include/vsyscall.h src/include/slab.h \
 src/include/oom.h src/include/rcu.h src/include/aslr.h \
 src/include/seccomp.h src/include/audit.h src/include/yama.h \
 src/include/kptr_restrict.h src/include/dmesg.h src/include/sysrq.h \
 src/include/panic.h src/include/nmi_watchdog.h src/include/lockdep.h \
 src/include/tmpfs.h src/include/vfs.h src/include/compaction.h \
 src/include/memhotplug.h src/include/page_poison.h src/include/cma.h \
 src/include/zram.h src/include/ksm.h src/include/thp.h \
 src/include/cmdline.h src/include/ramdisk.h src/include/timers.h \
 src/include/workqueue.h src/include/rng.h src/include/fsnotify.h \
 src/include/module.h src/include/list.h src/include/initcall.h \
 src/include/watchdog.h src/include/fbcon.h src/include/perf_events.h \
 src/include/jump_label.h src/include/atomic.h src/include/pstore.h \
 src/include/stack_guard.h src/include/rseq.h src/include/kasan_light.h \
 src/include/firmware.h src/include/memfd.h src/include/mseal.h \
 src/include/userfaultfd.h src/include/madvise_ext.h \
 src/include/mem_policy.h src/include/page_idle.h \
 src/include/page_allocator_ext.h src/include/sched_attr.h \
 src/include/cpuset.h src/include/pidfd.h src/include/landlock.h \
 src/include/seccomp_bpf.h src/include/process_rlimit.h \
 src/include/devtmpfs.h src/include/overlay.h src/include/fanotify.h \
 src/include/fs_mount_prop.h src/include/net_igmp.h \
 src/include/net_lldp.h src/include/aio_enhanced.h
src/include/types.h:
src/include/vga.h:
src/include/types.h:
src/include/gdt.h:
src/include/idt.h:
src/include/pic.h:
src/include/fault.h:
src/include/process.h:
src/include/signal.h:
src/include/timer.h:
src/include/keyboard.h:
src/include/pmm.h:
src/include/vmm.h:
src/include/heap.h:
src/include/process.h:
src/include/scheduler.h:
src/include/shell.h:
src/include/serial.h:
src/include/printf.h:
src/include/io.h:
src/include/ata.h:
src/include/fs.h:
src/include/pci.h:
src/include/e1000.h:
src/include/net.h:
src/include/netfilter.h:
src/include/pkt_sched.h:
src/include/bridge.h:
src/include/vlan.h:
src/include/tun.h:
src/include/net_ns.h:
src/include/net_internal.h:
src/include/net.h:
src/include/ipip.h:
src/include/wireguard.h:
src/include/ipvs.h:
src/include/telnetd.h:
src/include/ssh.h:
src/include/rtc.h:
src/include/mouse.h:
src/include/speaker.h:
src/include/acpi.h:
src/include/syscall.h:
src/include/ahci.h:
src/include/usb.h:
src/include/usb_msc.h:
src/include/service.h:
src/include/httpd.h:
src/gui/gui.h:
src/include/intel_gpu.h:
src/include/fat32.h:
src/include/users.h:
src/include/vfs.h:
src/include/errno.h:
src/include/pipe.h:
src/include/waitqueue.h:
src/include/spinlock.h:
src/include/io.h:
src/include/blockdev.h:
src/include/shm.h:
src/include/virtio_net.h:
src/include/virtio_blk.h:
src/include/ac97.h:
src/include/smp.h:
src/include/scheduler.h:
src/include/gdt.h:
src/include/apic.h:
src/include/idt.h:
src/include/elf.h:
src/include/cpu.h:
src/include/cpu_features.h:
src/include/x2apic.h:
src/include/tsc_deadline.h:
src/include/vsyscall.h:
src/include/slab.h:
src/include/oom.h:
src/include/rcu.h:
src/include/aslr.h:
src/include/seccomp.h:
src/include/audit.h:
src/include/yama.h:
src/include/kptr_restrict.h:
src/include/dmesg.h:
src/include/sysrq.h:
src/include/panic.h:
src/include/nmi_watchdog.h:
src/include/lockdep.h:
src/include/tmpfs.h:
src/include/vfs.h:
src/include/compaction.h:
src/include/memhotplug.h:
src/include/page_poison.h:
src/include/cma.h:
src/include/zram.h:
src/include/ksm.h:
src/include/thp.h:
src/include/cmdline.h:
src/include/ramdisk.h:
src/include/timers.h:
src/include/workqueue.h:
src/include/rng.h:
src/include/fsnotify.h:
src/include/module.h:
src/include/list.h:
src/include/initcall.h:
src/include/watchdog.h:
src/include/fbcon.h:
src/include/perf_events.h:
src/include/jump_label.h:
src/include/atomic.h:
src/include/pstore.h:
src/include/stack_guard.h:
src/include/rseq.h:
src/include/kasan_light.h:
src/include/firmware.h:
src/include/memfd.h:
src/include/mseal.h:
src/include/userfaultfd.h:
src/include/madvise_ext.h:
src/include/mem_policy.h:
src/include/page_idle.h:
src/include/page_allocator_ext.h:
src/include/sched_attr.h:
src/include/cpuset.h:
src/include/pidfd.h:
src/include/landlock.h:
src/include/seccomp_bpf.h:
src/include/process_rlimit.h:
src/include/devtmpfs.h:
src/include/overlay.h:
src/include/fanotify.h:
src/include/fs_mount_prop.h:
src/include/net_igmp.h:
src/include/net_lldp.h:
src/include/aio_enhanced.h:
