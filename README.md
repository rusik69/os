# OS — x86-64 Hobby Kernel

[![CI](https://github.com/rusik69/os/actions/workflows/ci.yml/badge.svg)](https://github.com/rusik69/os/actions/workflows/ci.yml)

A production-oriented x86-64 hobby operating system kernel written in C17 and NASM assembly. ~83K lines of C across 572 source files, 60+ subsystems — boot, memory management, process/scheduler, VFS + multiple filesystems, full TCP/IP networking, SMP, drivers (PCI, ACPI, NVMe, AHCI, USB, virtio, e1000), in-kernel C compiler, DOS emulator, GUI, DOOM port, and 200+ shell commands.

**Status:** Active development. Boots in QEMU (and real hardware with UEFI/BIOS). Self-hosted CI via GitHub Actions runner.

## Quick Start

```bash
# Build
make -j$(nproc)

# Run in QEMU
make run

# Run tests
./tests/run_tests.sh kernel.elf disk.img
```

Dependencies: `x86_64-elf-gcc`, `nasm`, `qemu-system-x86_64`, `make`.

## Project Structure

| Directory     | Purpose |
|---------------|---------|
| `src/boot/`   | Multiboot1 entry, 32→64-bit transition, page table setup |
| `src/kernel/` | Core kernel: GDT/IDT, syscalls, interrupts, SMP, RCU, lockdep, ASLR, security |
| `src/memory/` | PMM (bitmap allocator), VMM (4-level paging), slab, heap, KSM, THP, compaction |
| `src/process/`| Scheduler, signals, process lifecycle, users/groups, PELT |
| `src/fs/`     | VFS layer + ext2, FAT32, tmpfs, procfs, sysfs, devfs, ISO9660, tarfs, cpio |
| `src/net/`    | TCP/IP stack, UDP, DHCP, DNS, HTTPd, SSHd, Telnetd, WireGuard, bridge, VLAN, netfilter |
| `src/drivers/`| PCI, ACPI, AHCI, NVMe, ATA, e1000, virtio-blk/net, USB EHCI/xHCI, keyboard, mouse, VGA, framebuffer, AC97 audio |
| `src/ipc/`    | Pipes, FIFOs, shared memory, mutexes, semaphores, eventfd, signalfd, timerfd, mqueue |
| `src/shell/`  | Built-in shell with 200+ commands, command table, scripting |
| `src/lib/`    | Kernel libc: printf, string, bitmap, CRC, AES, SHA256, MD5, radix tree, UUID |
| `src/compiler/`| In-kernel C compiler (cc, as, ld) — compiles C to ELF at runtime |
| `src/doom/`   | DOOM game engine port (raycaster, renderer, textures) |
| `src/dos/`    | DOS API emulation (INT 21h, program loading) |
| `src/gui/`    | Graphical window system with widgets and mouse support |
| `src/include/`| Master headers for all subsystems |
| `src/test/`   | Kernel-resident test suite (200+ tests) |
| `tests/`      | Host-side libc tests, E2E QEMU smoke tests |
| `docker/`     | CI container definitions |
| `docs/`       | Architecture diagram and documentation |

## Features

### Kernel
- 64-bit long mode, 4-level paging with 2MB/1GB huge pages
- SMP: multi-CPU bringup via ACPI MADT, x2APIC, TSC deadline timer
- Preemptible kernel with lockdep deadlock detection
- RCU (Read-Copy-Update) with expedited grace periods
- KASAN-light (memory corruption detection)
- Kernel ASLR, NX enforcement, SMAP/SMEP/UMIP
- Stack guard pages, kernel stack canaries
- Seccomp-BPF, Landlock LSM, YAMA ptrace scope
- Audit subsystem, dmesg with kptr_restrict
- OOM killer with process scoring

### Memory Management
- Physical Memory Manager: bitmap-based, refcounted (COW), per-CPU hot cache
- Virtual Memory Manager: 4-level paging, copy-on-write fork, per-process PML4
- Slab allocator with per-CPU caches and object poisoning
- Kernel heap (kmalloc/kfree, 12MB limit)
- KSM (Kernel Same-page Merging), THP (Transparent Hugepages)
- ZRAM compressed RAM block device
- CMA (Contiguous Memory Allocator), memory compaction, hotplug
- Page poisoning, page owner tracking

### Process Management
- Round-robin + priority scheduler with aging
- CFS-compatible vruntime scheduling
- 256 process slots, fork/exec/waitpid/exit
- Signals (32 POSIX signals), signal masking, sigaction
- POSIX thread support (clone, TLS via FSGSBASE)
- Process groups, sessions, job control
- User/group permissions, capabilities
- RLIMIT enforcement (process_rlimit)
- chroot, namespaces (PID, mount, net, user)

### Filesystem
- VFS layer: mount table, path resolution, file operations, inode/dentry caches
- ext2: read/write, directories, symlinks, HTree indexes
- FAT32: read/write, long filenames, mkdir, rmdir
- ISO9660: with Rock Ridge and Joliet extensions
- tmpfs: RAM-based, directories, symlinks, device nodes
- procfs: /proc/{uptime,meminfo,cpuinfo,stat,self,interrupts}
- sysfs: kobject-based device model, kernel parameter export
- devfs: dynamic device node creation
- tarfs/cpio/romfs: initramfs-style embedded filesystems
- OverlayFS: union mount stacking
- Page cache, buffer cache, file advisories
- Extended attributes (xattr), fanotify/inotify directory monitoring
- Filesystem freeze/thaw

### Networking
- TCP: full state machine, sliding window, congestion control (Reno/CUBIC/B BR)
- UDP: connected sockets, multicast, broadcast
- IP: routing table, fragmentation, ICMP, ARP
- DHCP client with lease management
- DNS stub resolver with caching
- HTTPd, Telnetd, SSHd (SSH v2), FTP client
- WireGuard VPN, IPIP/GRE tunnels, VLAN 802.1Q
- Bridge with STP, VXLAN overlay
- Netfilter: packet filtering, NAT, conntrack, nf_tables
- Traffic control: HTB, fq_codel, pfifo_fast
- IPVS load balancing, IGMP snooping
- Socket API: TCP/UDP/raw, bind/connect/listen/accept/send/recv
- Network namespaces, TUN/TAP interfaces
- RPS/RFS: receive packet steering and flow steering

### Drivers
- PCI: full enumeration, MSI/MSI-X, PCIe capabilities, AER
- ACPI: tables (MADT, DSDT, SSDT, FADT, HPET, LPIT, NFIT), EC, thermal, power button
- NVMe: multi-queue, admin/submission/completion queues, sanitize
- AHCI: NCQ, port multiplier, TRIM
- ATA PIO: legacy IDE, ATAPI
- USB: EHCI and xHCI, mass storage, HID, isochronous
- virtio: virtio-blk (multi-queue) and virtio-net (LRO)
- e1000: multi-queue RSS, interrupt moderation
- Framebuffer: VGA text mode, Bochs VBE, Intel GPU, EDID parsing
- AC97 audio: PCM playback, recording
- PS/2 keyboard, mouse, serial console
- HPET, PIT, RTC, CMOS, watchdog
- GPIO, I2C, SMBus
- Software RAID (MD): RAID0, RAID1, RAID10
- Loop device, ramdisk, partition table (MBR + GPT)
- Core dump device (pstore/ramoops)
- Battery, ACPI thermal zones

### User Space
- Built-in shell: 200+ commands (coreutils, networking, admin)
- Scripting: variables, pipelines, redirection, job control
- In-kernel C compiler: lex, parse, codegen, ELF link
- DOS emulator: INT 21h, .COM and .EXE loading
- DOOM: full game engine, raycast renderer, framebuffer output
- GUI: window manager, widgets, mouse cursor, compositing

### Security
- Seccomp-BPF: syscall filtering with SECCOMP_RET_KILL/TRAP/ALLOW/LOG
- Landlock: path-based mandatory access control
- YAMA: ptrace scope restriction (levels 0-4)
- Kernel ASLR, KASLR for modules
- NX (non-executable) enforcement, SMAP/SMEP, UMIP
- Kernel stack guard pages and canaries
- SLAB object poisoning and random freelist order
- dmesg_restrict, kptr_restrict, kernel address display control
- Audit subsystem, process capability bounding sets
- Audit logging, IMA (integrity measurement)
- CET shadow stack (Intel Control-flow Enforcement)

### Performance & Scaling
- SMP: multi-CPU scheduling, per-CPU runqueues
- Per-CPU page and slab caches (lockless fast paths)
- RCU: expedited grace periods, nocb mode, stall detection
- PELT: Per-Entity Load Tracking for scheduler decisions
- MCS lock: optimistic spinning for mutexes
- TLB shootdown avoidance for single-thread processes
- Workqueue: bound and unbound worker pools
- Softirq/kSoftirqd with fair scheduling
- Receive/Transmit Packet Steering (RPS/RFS/XPS)
- TCP: CUBIC, BBR, RACK loss detection, TFO, SYN cookies
- I/O schedulers: deadline, CFQ, noop

## Building

```bash
# Debug build (default)
make

# Release build (optimized)
make build/arch/release/kernel.elf

# Strict build (all warnings as errors)
make build-strict

# Build and run in QEMU
make run

# Build and debug in QEMU + GDB
make debug
```

Uses `ccache` automatically if installed. Cross-compiler toolchain: `x86_64-elf-gcc`.

## Testing

```bash
# Boot kernel in QEMU and run 200+ built-in tests
./tests/run_tests.sh kernel.elf disk.img

# Host-side libc unit tests
cd tests/host_libc && make && ./test_libc

# E2E QEMU smoke test (boot to shell + commands)
./tests/e2e.sh
```

The kernel has ~200 built-in tests covering: scheduler, VM, PMM, slab, IPC, VFS, TCP, UDP, socket API, and device drivers. Tests run in QEMU and report PASS/FAIL via serial. CI enforces all tests pass on every commit.

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for a detailed walkthrough of the kernel design, boot sequence, memory layout, subsystem relationships, and data flow.

Also see [docs/architecture-diagram.html](docs/architecture-diagram.html) for an interactive visual diagram.

## License

MIT — see LICENSE file.
