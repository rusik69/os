#ifndef KERNEL_H
#define KERNEL_H
#include "types.h"

/*
 * ── Hermes OS Kernel Architecture Overview ────────────────────
 *
 * Hermes OS is a from-scratch x86_64 (IA-32e) kernel providing a Unix-like
 * environment with preemptive multitasking, virtual memory management, a
 * POSIX-compatible syscall ABI, and rich built-in networking and storage.
 *
 * ── Boot Flow ────────────────────────────────────────────────
 * (1) The bootloader (GRUB or QEMU -kernel) loads the kernel at entry.asm
 *     via the Multiboot spec.  The kernel starts in 32-bit protected mode.
 * (2) boot.asm switches to long mode (IA-32e) by setting up a minimal GDT
 *     and page table, then jumps to kernel_main() in kernel.c.
 * (3) kernel_main() initialises subsystems in a strict dependency order:
 *       Early serial → VGA console → CPU features → GDT/IDT → APIC →
 *       PMM (page allocator) → VMM (virtual memory) → Slab allocator →
 *       Heap → Scheduler → Syscall dispatch → Initcalls (drivers, FS,
 *       networking, IPC) → Mount root FS → Spawn /init → Idle loop.
 * (4) The init process (userspace/init/) launches the shell and services.
 *
 * ── Major Subsystems ─────────────────────────────────────────
 *
 *   Memory Management
 *     PMM (physical memory manager) — bitmap/buddy allocator for physical
 *     page frames.  VMM (virtual memory manager) — per-process page tables,
 *     demand paging, COW, mmap/munmap, mprotect, ASLR.  Slab allocator for
 *     kernel objects; general-purpose heap backed by slab; KASan-light for
 *     out-of-bounds detection; OOM killer; swap/zram/zswap.
 *
 *   Process & Scheduling
 *     Process structure (task_struct) with PID/TID, credentials, signal
 *     disposition, file descriptor table, namespace membership.  O(1)
 *     priority-based scheduler with per-CPU run-queues, load balancing,
 *     cgroups (CPU/memory/IO), CPUset partitioning, nohz_full, PELT
 *     load tracking, and real-time (SCHED_FIFO/RR) support.
 *
 *   Inter-Process Communication (IPC)
 *     Pipes (anonymous & named), eventfd, signalfd, inotify, epoll,
 *     POSIX message queues, futexes, shared memory (shm), and signal
 *     delivery with full POSIX real-time semantics.
 *
 *   Networking Stack
 *     Full TCP/IP stack: IPv4/IPv6, TCP, UDP, ICMP, ARP, DHCP client.
 *     Bridging, VLAN (802.1Q), MACsec, WireGuard, IPsec, IPVS (L4 load
 *     balancer), nftables firewall, traffic scheduling (HTB/fq_codel),
 *     packet sockets, TUN/TAP, VETH, network namespaces, IGMP snooping,
 *     LLDP, RFS/RPS, and Netfilter hook framework.
 *
 *   Filesystems & Storage
 *     VFS layer with mount propagation.  ext2, FAT32, tmpfs, devtmpfs,
 *     sysfs, debugfs, tracefs, procfs, overlayfs.  Block device layer
 *     with partitioning (MBR/GPT), MD RAID (0/1/5/6/10), DM-crypt,
 *     DM-verity, DRBD (distributed replicated block device), NBD
 *     (network block device), iSCSI, NVMe-oF (NVMf), FCoE, and LVM-
 *     style multipath.  Page cache and read-ahead for block-backed FS.
 *
 *   Drivers & Hardware
 *     ACPI (AML interpreter, EC, thermal, cpupstate), HPET, PIT, RTC,
 *     CMOS, I2C/SMBus, SPI, Watchdog (i6300ESB), IPMI (KCS), TPM 2.0,
 *     DRM (simplefb, bochs), USB (xHCI hub, MSC, CDC-ACM), AHCI/SATA,
 *     NVMe, VirtIO (net, blk), e1000, VMXNET3, AC97 audio, PS/2
 *     keyboard & mouse, VMWare balloon, pvpanic, EDAC/GHES.
 *
 *   Security & Observability
 *     Seccomp-BPF for syscall filtering, Landlock LSM for filesystem
 *     sandboxing, IMA/EVM for integrity measurement, audit subsystem,
 *     ftrace/kprobes for dynamic tracing, perf_events, NMI watchdog,
 *     stack canary (/GS), NX/W^X enforcement, kernel ASLR/KASLR,
 *     module signing, kmemleak, fault injection.
 *
 * ── Concurrency Model ────────────────────────────────────────
 * Preemptive kernel — spinlocks, mutexes (with priority inheritance),
 * RCU for read-mostly data, lockdep for deadlock detection, per-CPU
 * variables, softirq/tasklet/workqueue deferral, IRQ threading.
 *
 * ── Project Structure ────────────────────────────────────────
 *   src/boot/        — Entry point, long-mode transition
 *   src/kernel/      — Core kernel: init, syscalls, security, observability
 *   src/memory/      — PMM, VMM, slab, heap, OOM, swap
 *   src/process/     — Task/process management, scheduler, signals
 *   src/ipc/         — Inter-process communication primitives
 *   src/net/         — Networking stack (TCP/IP, bridge, netfilter, VPN)
 *   src/fs/          — Filesystem implementations (ext2, fat32, tmpfs, ...)
 *   src/drivers/     — Hardware drivers (disk, net, USB, GPU, audio, ...)
 *   src/drivers/drm/ — Direct Rendering Manager subsystem
 *   src/include/     — Public kernel headers (types, macros, interfaces)
 *   userspace/       — System init, shell, libc, utility programs
 *   tests/           — Unit and end-to-end tests
 *
 * The kernel is compiled with -Wall -Wextra -Werror (zero warnings policy).
 * All code follows the project convention defined in AGENTS.md and uses
 * conventional commit messages (feat:, fix:, ci:, docs:, refactor:, test:).
 */
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define container_of(ptr, type, member) ({ const __typeof__(((type *)0)->member) *__mptr = (ptr); (type *)((char *)__mptr - offsetof(type, member)); })
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define min(x, y) ({ __typeof__(x) _x = (x); __typeof__(y) _y = (y); _x < _y ? _x : _y; })
#define max(x, y) ({ __typeof__(x) _x = (x); __typeof__(y) _y = (y); _x > _y ? _x : _y; })
#define clamp(v, lo, hi) min(max(v, lo), hi)
#define abs(x) ({ __typeof__(x) _x = (x); _x < 0 ? -_x : _x; })
#define __maybe_unused __attribute__((unused))
#define barrier() __asm__ volatile("" : : : "memory")
#define mb()      __asm__ volatile("mfence" : : : "memory")
#define rmb()     __asm__ volatile("lfence" : : : "memory")
#define wmb()     __asm__ volatile("sfence" : : : "memory")

/*
 * READ_ONCE / WRITE_ONCE — prevent compiler from merging, tearing, or
 * caching accesses to shared variables.  On x86-64, aligned loads and
 * stores are naturally atomic; these macros add the volatile qualifier
 * so the compiler emits exactly one load/store and re-fetches on every
 * use.  Use for any global variable that is read or written in
 * interrupt context without holding a spinlock that includes a
 * compiler barrier.
 */
#define READ_ONCE(x)                    (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, val)             ((*(volatile __typeof__(x) *)&(x)) = (val))
#endif
