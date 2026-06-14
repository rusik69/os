# OS — x86-64 Hobby Kernel

[![CI](https://github.com/rusik69/os/actions/workflows/ci.yml/badge.svg)](https://github.com/rusik69/os/actions/workflows/ci.yml)

A production-oriented x86-64 hobby operating system kernel written in C17 and NASM assembly. ~150K+ lines of C across 872 source files, 80+ subsystems — boot, memory management, process/scheduler, VFS + multiple filesystems, full TCP/IP networking, SMP, drivers (PCI, ACPI, NVMe, AHCI, USB, virtio, e1000), in-kernel C compiler, DOS emulator, GUI, DOOM port, and 340+ shell commands.

**Status:** Active development. Boots in QEMU (and real hardware with UEFI/BIOS). Self-hosted CI via GitHub Actions runner. Production-grade hobby OS with container orchestration, clustering, security features, and performance infrastructure.

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
| `src/fs/`     | VFS layer + ext2, FAT32, tmpfs, procfs, sysfs, devfs, ISO9660, tarfs, cpio, and more |
| `src/net/`    | TCP/IP stack, UDP, DHCP, DNS, HTTPd, SSHd, Telnetd, WireGuard, bridge, VLAN, netfilter |
| `src/drivers/`| PCI, ACPI, AHCI, NVMe, ATA, e1000, virtio-blk/net/gpu/input/rng/scsi/console, USB EHCI/xHCI, keyboard, mouse, VGA, framebuffer, AC97 audio |
| `src/ipc/`    | Pipes, FIFOs, shared memory, mutexes, semaphores, eventfd, signalfd, timerfd, mqueue |
| `src/shell/`  | Built-in shell with 340+ commands, command table, scripting |
| `src/lib/`    | Kernel libc: printf, string, bitmap, CRC, AES, SHA256, MD5, radix tree, UUID |
| `src/compiler/`| In-kernel C compiler (cc, as, ld) — compiles C to ELF at runtime |
| `src/doom/`   | DOOM game engine port (raycaster, renderer, textures) |
| `src/dos/`    | DOS API emulation (INT 21h, program loading) |
| `src/gui/`    | Graphical window system with widgets and mouse support |
| `src/include/`| Master headers for all subsystems |
| `src/test/`   | Kernel-resident test suite (200+ tests + KUnit) |
| `tests/`      | Host-side libc tests, E2E QEMU smoke tests |
| `docker/`     | CI container definitions |
| `docs/`       | Architecture diagram and documentation |

## Features

### Kernel
- 64-bit long mode, 4-level paging with 2MB/1GB huge pages
- SMP: multi-CPU bringup via ACPI MADT, x2APIC, TSC deadline timer
- Preemptible kernel with lockdep deadlock detection
- RCU (Read-Copy-Update) with expedited grace periods
- KASAN-light (memory corruption detection), KFENCE, KCSAN, kmemleak
- Kernel ASLR, NX enforcement, SMAP/SMEP/UMIP, CET Shadow Stack
- KPTI (Kernel Page Table Isolation) with PCID/INVPCID
- Stack guard pages, kernel stack canaries
- Seccomp-BPF, Landlock LSM, YAMA ptrace scope
- Audit subsystem, IMA (Integrity Measurement), EVM, IPE
- Lockdown mode, module signing, dmesg with kptr_restrict
- OOM killer with process scoring

### Memory Management
- Physical Memory Manager: bitmap-based, refcounted (COW), per-CPU hot cache
- Virtual Memory Manager: 4-level paging, copy-on-write fork, per-process PML4
- Slab allocator with per-CPU caches and object poisoning
- Kernel heap (kmalloc/kfree, 12MB limit)
- KSM (Kernel Same-page Merging), THP (Transparent Hugepages)
- ZRAM compressed RAM block device, ZSWAP
- CMA (Contiguous Memory Allocator), memory compaction, hotplug
- Page poisoning, page owner tracking, memory policy
- IOMMU support, huge pages (2MB/1GB)

### Process Management
- Round-robin + priority scheduler with aging
- CFS-compatible vruntime scheduling
- 256 process slots, fork/exec/waitpid/exit
- Signals (32 POSIX signals), signal masking, sigaction
- POSIX thread support (clone, TLS via FSGSBASE)
- Process groups, sessions, job control
- User/group permissions, capabilities
- RLIMIT enforcement (process_rlimit)
- chroot, namespaces (PID, mount, net, user, cgroup)

### Filesystem Types
- **ext2** — read/write, directories, symlinks, HTree indexes
- **FAT32** — read/write, long filenames, mkdir, rmdir
- **tmpfs** — RAM-based, directories, symlinks, device nodes
- **ISO9660** — CDROM with Rock Ridge and Joliet extensions
- **squashfs** — compressed read-only filesystem
- **HFS** — Hierarchical File System (Apple)
- **cramfs** — compressed ROM filesystem
- **minix** — MINIX v1/v2 filesystem
- **UFS** — Unix File System (BSD)
- **SYSV** — System V filesystem
- **ADFS** — Acorn Disc Filing System
- **BFS** — BeOS filesystem
- **tarfs** — read-only tar archive filesystem
- **romfs** — simple read-only ROM filesystem
- **cpio** — initramfs cpio archive filesystem
- **devfs** — dynamic device node creation
- **procfs** — /proc/{uptime,meminfo,cpuinfo,stat,self,interrupts}
- **sysfs** — kobject-based device model, kernel parameter export
- **debugfs** — kernel debug data virtual filesystem
- **FUSE** — Filesystem in Userspace
- **OverlayFS** — union mount stacking with copy-up-on-write
- **fs-verity** — Merkle tree integrity verification
- Page cache, buffer cache, file advisories
- Extended attributes (xattr), POSIX ACL, fanotify/inotify directory monitoring
- Filesystem freeze/thaw, quota enforcement

### Networking
- **TCP** — full state machine, sliding window, congestion control (Reno, CUBIC, BBR, BIC, Vegas, Westwood, Illinois, Hybla), RACK loss detection, TFO, SYN cookies
- **UDP** — connected sockets, multicast, broadcast, UDP-Lite
- **IP** — routing table, fragmentation, ICMP, ICMPv6
- **IPv6** — full IPv6 stack, Neighbor Discovery, SLAAC
- **ARP** — Address Resolution Protocol
- **SCTP** — Stream Control Transmission Protocol
- **DCCP** — Datagram Congestion Control Protocol
- **MPTCP** — Multipath TCP
- **CAN** — Controller Area Network (AF_CAN)
- **AF_PACKET** — raw packet sockets (AF_PACKET, SOCK_RAW / SOCK_DGRAM)
- **AF_UNIX** — Unix domain sockets (stream + datagram)
- **netlink** — kernel-userspace communication
- **VLAN** — 802.1Q VLAN tagging
- **VXLAN** — Virtual eXtensible LAN (RFC 7348)
- **GRE** — Generic Routing Encapsulation (RFC 2784)
- **IPIP** — IP-in-IP tunneling (RFC 2003)
- **WireGuard** — secure VPN tunnel
- **MACsec** — IEEE 802.1AE media access control security
- **6LoWPAN** — IPv6 over Low-Power Wireless Personal Area Networks
- **IPoIB** — IP over InfiniBand
- **IPsec** — IP Security (AH/ESP, IKEv2 via pf_key)
- **Bridge** — learning bridge with STP, IGMP snooping, VLAN filtering
- **STP** — Spanning Tree Protocol (802.1D)
- **GARP** — Generic Attribute Registration Protocol
- **LACP** — Link Aggregation Control Protocol (802.3ad)
- **MRP** — Multiple Registration Protocol (802.1Q)
- **LLDP** — Link Layer Discovery Protocol
- DHCP client with lease management
- DNS stub resolver with caching
- HTTPd, Telnetd, SSHd (SSH v2), FTP client
- Netfilter: packet filtering, NAT, conntrack, nf_tables, ebtables, arptables
- Traffic control: HTB, fq_codel, pfifo_fast
- IPVS load balancing, IGMP snooping
- Network namespaces, TUN/TAP interfaces
- RPS/RFS/XPS: receive/transmit packet steering

### PCI / ACPI / Device Drivers
- **PCI** — full enumeration, MSI/MSI-X, PCIe capabilities, AER, SR-IOV
- **ACPI** — tables (MADT, DSDT, SSDT, FADT, HPET, LPIT, NFIT), EC, thermal zones, power button, platform profile
- **PNP** — Plug and Play device enumeration
- **Storage:**
  - **NVMe** — multi-queue, admin/submission/completion queues, sanitize, PMR
  - **AHCI** — NCQ, port multiplier, TRIM
  - **ATA PIO** — legacy IDE, ATAPI
  - **virtio-blk** — multi-queue virtio block device
  - **virtio-scsi** — virtio SCSI controller
  - **USB MSC** — USB mass storage class
  - **NBD** — Network Block Device
  - **Loop** — loopback block device
  - **Ramdisk** — ramdisk block device
  - **PMEM** — persistent memory
  - **MD (RAID)** — RAID0, RAID1, RAID10 (DM-RAID)
  - **Multipath** — device mapper multipath I/O
- **Network:**
  - **E1000** — Intel PRO/1000, multi-queue RSS, interrupt moderation
  - **virtio-net** — LRO, multi-queue
  - **USB Ethernet** — USB CDC ECM / RNDIS
  - **Veth** — virtual Ethernet interfaces
  - **Netconsole** — network console logging
- **USB:**
  - **EHCI** — Enhanced Host Controller Interface
  - **xHCI** — eXtensible Host Controller Interface (streams)
  - **USB Core** — hub, device enumeration
  - **HID** — Human Interface Devices (keyboard, mouse, joystick)
  - **CDC ACM** — serial/Modem over USB
  - **Printer** — USB printer class
  - **Type-C** — USB Type-C connector management
  - **Debug** — USB debug device
  - **Gadget** — USB gadget framework (UDC core, mass storage function)
- **Virtio:**
  - **virtio-blk** — multi-queue block
  - **virtio-net** — LRO
  - **virtio-gpu** — GPU/display
  - **virtio-input** — input devices
  - **virtio-rng** — entropy source
  - **virtio-scsi** — SCSI controller
  - **virtio-console** — console device
- **Display / Graphics:**
  - **Framebuffer** — VGA text mode, Bochs VBE, simplefb
  - **Intel GPU** — integrated graphics
  - **DRM** — Direct Rendering Manager skeleton (drm_core, drm_gem, drm_dumb, bochs_drm)
  - **EDID** — Extended Display Identification Data parsing
- **Audio:**
  - **AC97** — PCM playback, recording
  - **Sound Core** — OSS sound layer, MIDI
  - **Sndstat** — sound status
- **Miscellaneous:**
  - **PS/2** — keyboard, mouse
  - **Serial** — COM1/COM2 serial console (16550 UART)
  - **HPET** — High Precision Event Timer
  - **PIT** — Programmable Interval Timer
  - **RTC** — Real-Time Clock (CMOS + HPET)
  - **CMOS** — CMOS/NVRAM access
  - **Watchdog** — hardware watchdog timer
  - **SPI** — Serial Peripheral Interface bus
  - **I2C / SMBus** — I2C and SMBus controllers
  - **GPIO** — General Purpose I/O with interrupt support
  - **EDAC** — Error Detection And Correction (DRAM error reporting)
  - **GHES** — Generic Hardware Error Source (ACPI)
  - **I3C** — Improved Inter-Integrated Circuit bus
  - **TPM TIS** — Trusted Platform Module 2.0 TIS interface
  - **DMI** — Desktop Management Interface (SMBIOS)
  - **IPMI KCS** — Intelligent Platform Management Interface
  - **Firmware Class** — generic firmware loading API
  - **pvpanic** — QEMU panic device
  - **ivshmem** — QEMU inter-VM shared memory
  - **9pnet virtio** — 9P2000.L virtio transport
  - **VMware balloon** — memory balloon driver
  - **VMware PVSCSI** — paravirtual SCSI
- **Power Management:**
  - ACPI thermal zones, battery, cpufreq governors (ondemand, conservative, userspace, schedutil)
  - Cpuidle (ladder, teo, haltpoll), devfreq, energy model
  - Suspend/resume, PM QoS, PM runtime
  - RAPL — Running Average Power Limit

### Architecture Support
- **x86-64** — long mode, 4-level paging, canonical addresses
- **SMP** — multi-CPU via ACPI MADT, APIC, x2APIC, TSC deadline timer
- **IOMMU** — Intel VT-d DMA remapping
- **SMAP/SMEP/UMIP** — kernel protection features
- **CET** — Control-flow Enforcement (shadow stack, IBT)
- **KPTI** — Kernel Page Table Isolation with PCID/INVPCID
- **FSGSBASE** — userspace TLS via MSRs
- **RDPID** — fast TSC/processor ID
- **NX** — non-executable page enforcement
- **PKEYS** — Memory Protection Keys (pkey_alloc/free/mprotect)

### Security Features
- **Seccomp-BPF** — syscall filtering (SECCOMP_RET_KILL/TRAP/ALLOW/LOG/TRACE)
- **Landlock** — path-based mandatory access control
- **YAMA** — ptrace scope restriction (levels 0-4)
- **IMA** — Integrity Measurement Architecture (measure, appraise, policy)
- **EVM** — Extended Verification Module
- **IPE** — Integrity Policy Enforcement
- **Lockdown** — kernel lockdown mode (integrity, confidentiality)
- **PKEY** — Memory Protection Keys for userspace
- **Stackleak** — kernel stack erasure on syscall exit
- **KASLR** — kernel ASLR, module KASLR
- **KASAN-light** — kernel address sanitizer (out-of-bounds, use-after-free)
- **KFENCE** — low-overhead memory error detector
- **KCSAN** — Kernel Concurrency Sanitizer (data race detection)
- **kmemleak** — kernel memory leak detector
- **Module signing** — cryptographically signed modules (SHA-256/RSA)
- **Audit** — audit subsystem, syscall auditing, path auditing
- **dmesg_restrict** — kernel address display control
- **kptr_restrict** — pointer display restriction
- **Kernel stack canaries** — -fstack-protector-strong
- **Stack guard pages** — guard pages between kernel stacks
- **SLAB poisoning** — freed memory poisoning, freelist randomization

### Performance & Scaling
- **MGLRU** — Multi-Generational LRU page reclaim
- **Ftrace** — function tracer, trace events, tracepoints
- **Perf events** — hardware PMU counters, context-switch/page-fault tracking
- **Kprobes/kretprobes** — dynamic instrumentation
- **eBPF JIT** — eBPF verifier + JIT compiler
- **Jump labels** — static key patching
- **PSI** — Pressure Stall Information (CPU, memory, IO)
- **RCU** — expedited grace periods, nocb mode, stall detection, polled
- **PELT** — Per-Entity Load Tracking for scheduler
- **MCS lock** — optimistic spinning for mutexes
- **Workqueues** — bound and unbound worker pools
- **Softirq/kSoftirqd** — fair softirq scheduling
- **Tasklets** — bottom-half deferral mechanism
- **RPS/RFS/XPS** — receive/transmit packet steering
- **I/O schedulers** — deadline, CFQ, noop
- **TLB shootdown** — IPI-based TLB flushing, avoidance for single-thread processes
- **Per-CPU caches** — lockless page and slab caches
- **NOHZ** — tickless idle

### User Space
- Built-in shell: 340+ commands (coreutils, networking, admin, dev, debug)
- Scripting: variables, pipelines, redirection, job control, background tasks
- In-kernel C compiler: lex, parse, codegen, ELF link (cc, as, ld)
- DOS emulator: INT 21h, .COM and .EXE loading
- DOOM: full game engine, raycast renderer, framebuffer output
- GUI: window manager, widgets, mouse cursor, compositing, taskbar

### Container / Orchestration
- OCI-compatible container runtime: create, start, stop, delete
- Container exec, attach, logs, pause/unpause, wait, top, stats
- Container inspect: full JSON metadata dump
- OCI image management: pull, push, tag, list, remove, prune, save/load
- OverlayFS storage: layer management, whiteout, opaque directories
- Container networking: bridge, NAT, port mapping, network policies
- Orchestration API server on port 8375 (Docker-compatible REST)
- Pod abstraction: create, start, stop, add containers, health checks
- Service abstraction: stable endpoints with round-robin load balancing
- Container checkpoint/restore, security scanning, seccomp notify
- Scheduler policies: spread, binpack, random
- Resource limits, cgroup interfaces (CPU, memory, PID)

### Cluster Management
- Raft consensus: leader election, log replication, KV store
- Gossip protocol: membership, failure detection, state sync
- Node management: registration, health reporting, heartbeats
- Cluster networking: overlay, VXLAN, WireGuard mesh
- Network policies: ingress/egress rules, pod selectors
- Ingress controller: NodePort, LoadBalancer, HTTP routing
- Multi-tenant network isolation: per-namespace VXLAN/VLAN
- Horizontal Pod Autoscaler (HPA): CPU/memory-based scaling
- Vertical Pod Autoscaler (VPA): resource recommendation
- Cluster autoscaler: add/remove nodes on demand
- Descheduler: evict pods for better packing
- Custom Resource Definitions (CRDs)
- Cluster upgrades: cordon, drain, upgrade, uncordon, rollback
- Node problem detection and remediation
- Runtime security policies, RBAC, secrets management

## Building

```bash
# Debug build (default)
make

# Release build (optimized)
make build/arch/release/kernel.elf

# Strict build (all warnings as errors, then test)
make check

# Build and run in QEMU
make run

# Build and debug in QEMU + GDB
make debug

# SMP build with 4 CPUs
make run-smp

# Build with UEFI firmware
make run-uefi

# Build test kernel and run unit tests
make test

# Build and run E2E tests
make e2e

# Static analysis
make analyze

# Lint with cppcheck + clang-tidy
make lint

# Format all C sources
make format

# Show build info (kernel size, toolchain, object count)
make build-info

# Show source code statistics
make count

# Show ccache statistics
make ccache-stats

# Build kernel modules
make modules
```

Uses `ccache` automatically if installed. Cross-compiler toolchain: `x86_64-elf-gcc`.

## Running

```bash
# Standard QEMU run with IDE disk + e1000 NIC
make run

# SMP with 4 CPUs, all CPU features enabled
make run-smp

# Debug with GDB stub (-s -S)
make run-gdb

# Boot with UEFI firmware (OVMF)
make run-uefi

# Headless serial console on TCP port 4444 (for test automation)
make test-serial

# Boot and run the DOOM framebuffer test
make doom-test
```

## Testing

```bash
# Full test suite: build test kernel + boot QEMU + run 200+ tests
make test

# Strict check: -Werror build + tests + E2E smoke
make check

# Host-side libc unit tests
cd tests/host_libc && make && ./test_libc

# E2E QEMU smoke test (boot to shell + commands)
./tests/e2e.sh

# KUnit kernel tests (page alloc, slab, scheduler, VMM, security, power)
# Built into the test kernel, executed automatically during boot
```

The kernel has 200+ built-in tests plus expandable KUnit test suites covering: scheduler, VMM, PMM, slab, IPC, VFS, TCP, UDP, socket API, device drivers, security, and power management. Tests run in QEMU and report PASS/FAIL via serial. CI enforces all tests pass on every commit.

## Architecture

The OS is a monolithic x86-64 kernel booting via Multiboot1 (GRUB/QEMU). The kernel runs in the high half of the virtual address space (0xFFFF800000000000+) while userspace occupies the lower 512 GB. The boot sequence transitions from 32-bit to 64-bit long mode in boot.asm, then calls kernel_main which initializes subsystems in a linear ~29-step sequence: GDT → IDT → PIC → PMM → VMM → heap → slab → SMP → process → timer → keyboard → serial → PCI → storage → VFS → networking → ACPI → syscalls → shell → interrupts enabled.

Key design decisions:
- **Monolithic architecture** with all drivers in-kernel for performance
- **High-half VMA layout** with PHYS_TO_VIRT mapping via kernel PML4 entry 256
- **Bitmap PMM** with per-CPU hot caches and page reference counting for COW
- **4-level paging** with 2MB/1GB huge page support, KPTI for Meltdown mitigation
- **Preemptible kernel** with RCU for read-mostly data structures
- **Full pre-emptive priority scheduler** with CFS-compatible vruntime
- **VFS layer** with dentry caching, mount table, and 22+ filesystem implementations
- **Custom TCP/IP stack** with 8 congestion control algorithms and full protocol support
- **340+ built-in shell commands** for system administration and development

See [ARCHITECTURE.md](ARCHITECTURE.md) for a detailed walkthrough of the kernel design, boot sequence, memory layout, subsystem relationships, and data flow.

See [MODULARITY.md](MODULARITY.md) for the monolithic-to-modular transition plan — turning the kernel into a loadable module system.

Also see [docs/architecture-diagram.html](docs/architecture-diagram.html) for an interactive visual diagram.

## Acknowledgments

This kernel is developed as a community-driven hobby OS project. It stands on the shoulders of:

- The OSDev community (wiki, forums, example code)
- The Linux kernel (inspiration for APIs, algorithms, and design patterns)
- QEMU for providing an excellent development and testing platform
- LLVM/clang and GCC toolchains
- NASM assembler

## License

MIT — see LICENSE file.
