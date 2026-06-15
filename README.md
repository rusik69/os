# OS — x86-64 Hobby Kernel

[![CI](https://github.com/rusik69/os/actions/workflows/ci.yml/badge.svg)](https://github.com/rusik69/os/actions/workflows/ci.yml)
[![LOC](https://img.shields.io/badge/LOC-317K-blue)](https://github.com/rusik69/os)
[![C Files](https://img.shields.io/badge/C%20files-1453-blue)](https://github.com/rusik69/os)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![Arch](https://img.shields.io/badge/arch-x86__64-blue)](https://github.com/rusik69/os)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen)](CONTRIBUTING.md)

A production-oriented x86-64 hobby operating system kernel written in C17 and NASM assembly. **~317K lines of C across 999 source files, 453 headers, 80+ subsystems** — boot, memory management, process/scheduler, VFS + 30+ filesystems, full TCP/IP networking with 10+ congestion control algorithms, SMP, drivers (PCI, ACPI, NVMe, AHCI, USB, virtio, e1000), in-kernel C compiler, DOS emulator, GUI, DOOM port, and 356+ shell commands.

**Status:** Active development. Boots in QEMU (and real hardware with UEFI/BIOS). Self-hosted CI via GitHub Actions runner. Production-grade hobby OS with container orchestration, security features, performance infrastructure, eBPF, KVM virtualization, and clustering.

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
| `src/boot/`   | Multiboot1 entry, 32→64-bit transition, page table setup, UEFI runtime |
| `src/kernel/` | Core kernel: GDT/IDT, syscalls (500+), interrupts, SMP, RCU, lockdep, ASLR, security (IMA, SMACK, seccomp), eBPF, perf, ftrace, kprobes, uprobes |
| `src/memory/` | PMM (bitmap allocator), VMM (4-level paging), slab, heap, KSM, THP, MGLRU, NUMA balancing, compaction, page pool, huge page migration, zram/zswap |
| `src/process/`| Scheduler (CFS + EEVDF), signals, process lifecycle, users/groups, PELT, idle injection |
| `src/fs/`     | VFS layer + 30+ filesystem implementations |
| `src/net/`    | Full TCP/IP stack, 8 congestion control algos, UDP, IPv6, SCTP, DCCP, MPTCP, AF_UNIX, AF_PACKET, WireGuard, IPsec, bridging, netfilter, XDP, bonding, 6LoWPAN, TIPC, L2TP |
| `src/drivers/`| PCI, ACPI, NVMe (multipath, PMR), AHCI (NCQ), ATA, e1000 (RSS), virtio-blk/net/gpu/input/rng/scsi/console/fs/iommu, USB EHCI/xHCI, keyboard, mouse, VGA, framebuffer, AC97 audio, TPM 2.0, watchdog, I3C, SPI, EDAC, iSCSI, FCoE, DRBD, Ceph/RBD, vhost-scsi/blk, VFIO, vDPA, balloon |
| `src/ipc/`    | Pipes, FIFOs, shared memory, mutexes, semaphores, eventfd, signalfd, timerfd, mqueue, futex (robust, PI, bitset, requeue) |
| `src/shell/`  | Built-in shell with 356+ commands, command table, scripting, arrays, job control |
| `src/lib/`    | Kernel libc: printf, string, bitmap, CRC, AES, ChaCha20, SHA256, SHA512, MD5, radix tree, UUID |
| `src/compiler/`| In-kernel C compiler (cc, as, ld) — compiles C to ELF at runtime |
| `src/doom/`   | DOOM game engine port (raycaster, renderer, textures) |
| `src/dos/`    | DOS API emulation (INT 21h, program loading) |
| `src/gui/`    | Graphical window system with widgets and mouse support |
| `src/include/`| Master headers for all subsystems (453 headers) |
| `src/test/`   | Kernel-resident test suite (200+ tests + KUnit) |
| `tests/`      | Host-side libc tests, E2E QEMU smoke tests |
| `docker/`     | CI container definitions |
| `docs/`       | Architecture diagram and documentation |

## Features

### Kernel
- 64-bit long mode, 4-level paging with 2MB/1GB huge pages
- SMP: multi-CPU bringup via ACPI MADT, x2APIC, TSC deadline timer
- Preemptible kernel with lockdep deadlock detection
- RCU (Read-Copy-Update) with expedited grace periods, nocb offload, stall detection, polled mode, priority boosting
- KASAN-light (memory corruption detection), KFENCE, KCSAN, KMSAN, kmemleak, UBSan
- Kernel ASLR, NX enforcement, SMAP/SMEP/UMIP, CET Shadow Stack, MPK/pkeys
- KPTI (Kernel Page Table Isolation) with PCID/INVPCID
- Stack guard pages, kernel stack canaries, stackleak erasure
- Seccomp-BPF, Landlock LSM, SMACK LSM, YAMA ptrace scope
- Audit subsystem, IMA (Integrity Measurement), EVM, IPE
- Lockdown mode, module signing (RSA+SHA256), dmesg with kptr_restrict
- OOM killer with process scoring
- eBPF: verifier, maps (array/hash/perf), programs (kprobe/tracepoint/XDP), helpers
- KDB in-kernel debugger
- Hung task detection, workqueue OOM handling
- RANDSTRUCT structure randomization

### Memory Management
- Physical Memory Manager: bitmap-based, refcounted (COW), per-CPU hot cache
- Virtual Memory Manager: 4-level paging, copy-on-write fork, per-process PML4
- Slab allocator with per-CPU caches and object poisoning
- Kernel heap (kmalloc/kfree, 12MB limit)
- KSM (Kernel Same-page Merging), THP (Transparent Hugepages) with khugepaged
- MGLRU (Multi-Generational LRU) page reclaim
- ZRAM compressed RAM block device (with writeback to backing store), ZSWAP
- CMA (Contiguous Memory Allocator), memory compaction, hotplug
- Page poisoning, page owner tracking, memory policy
- NUMA balancing: page migration scanner, per-node stats
- IOMMU support, huge pages (2MB/1GB)
- Page pool: DMA page recycling for network drivers
- DMA-buf: buffer sharing between device drivers
- Huge page migration: 2MB page copy+remap with split fallback
- Idle injection: thermal/power management via duty-cycled HLT

### Process Management
- CFS-compatible vruntime scheduling + EEVDF (Earliest Eligible Virtual Deadline First)
- Round-robin + priority scheduler with aging
- 256 process slots, fork/exec/waitpid/exit
- Signals (32 POSIX signals), signal masking, sigaction
- POSIX thread support (clone, TLS via FSGSBASE)
- Process groups, sessions, job control
- User/group permissions, capabilities
- RLIMIT enforcement (process_rlimit)
- chroot, namespaces (PID, mount, net, user, cgroup, IPC, time)

### Filesystem Types
- **ext2** — read/write, directories, symlinks, HTree indexes, flex_bg
- **ext4** — extent trees, flex_bg descriptors, inline data, dir hash tree
- **FAT32** — read/write, long filenames, mkdir, rmdir
- **exFAT** — FAT chain walk, stream extension entries, up-case table (read-only)
- **NTFS** — MFT parsing, non-resident data runs, attribute enumeration (read-only)
- **Btrfs** — chunk tree, fs tree walk, inline/extent-backed data (read-only)
- **HFS+** — B-tree catalog, extents overflow, extended attributes (read-only)
- **HFS** — Hierarchical File System (Apple) (read-only)
- **tmpfs** — RAM-based, directories, symlinks, device nodes
- **ISO9660** — CDROM with Rock Ridge and Joliet extensions
- **squashfs** — compressed read-only filesystem (LZSS)
- **cramfs** — compressed ROM filesystem
- **minix** — MINIX v1/v2 filesystem
- **UFS** — Unix File System (BSD)
- **SYSV** — System V filesystem
- **ADFS** — Acorn Disc Filing System
- **BFS** — BeOS filesystem
- **ReiserFS** — B* tree, stat and directory items (read-only)
- **tarfs** — read-only tar archive filesystem
- **romfs** — simple read-only ROM filesystem
- **cpio** — initramfs cpio archive filesystem
- **devfs** — dynamic device node creation
- **procfs** — /proc/{uptime,meminfo,cpuinfo,stat,self,loadavg,pressure}
- **sysfs** — kobject-based device model, kernel parameter export
- **debugfs** — kernel debug data virtual filesystem
- **FUSE** — Filesystem in Userspace
- **OverlayFS** — union mount stacking with copy-up-on-write
- **NFS** — NFSv3 client and server (TCP)
- **CIFS/SMB** — SMB 2.0.2 client (negotiate, session setup, read)
- **fs-verity** — Merkle tree integrity verification
- Page cache (with readahead), buffer cache, file advisories
- Extended attributes (xattr), POSIX ACL, fanotify/inotify directory monitoring
- Filesystem freeze/thaw, quota enforcement
- Device mapper: linear, crypt (AES-XTS), RAID, zero, error, snapshot, verity, era

### Networking
- **TCP** — full state machine, sliding window, congestion control (Reno, CUBIC, BBR, BBRv2, BIC, Vegas, Westwood, Illinois, Hybla), RACK loss detection, TFO, SYN cookies, auto-tuning
- **UDP** — connected sockets, multicast, broadcast, UDP-Lite
- **IP** — routing table, fragmentation, ICMP, ICMPv6
- **IPv6** — full IPv6 stack, Neighbor Discovery, SLAAC, fragmentation
- **ARP** — Address Resolution Protocol
- **SCTP** — Stream Control Transmission Protocol
- **DCCP** — Datagram Congestion Control Protocol
- **MPTCP** — Multipath TCP
- **TIPC** — Transparent Inter-process Communication protocol
- **CAN** — Controller Area Network (AF_CAN)
- **AF_PACKET** — raw packet sockets (SOCK_RAW / SOCK_DGRAM) with PACKET_MMAP
- **AF_UNIX** — Unix domain sockets (stream + datagram, SCM_RIGHTS/CREDENTIALS)
- **SOCKS5** — SOCKS5 proxy client (connect/bind/UDP associate)
- **netlink** — kernel-userspace communication (NETLINK_RAS for HW events)
- **VLAN** — 802.1Q VLAN tagging
- **VXLAN** — Virtual eXtensible LAN (RFC 7348)
- **GRE** — Generic Routing Encapsulation (RFC 2784)
- **IPIP** — IP-in-IP tunneling (RFC 2003)
- **L2TPv3** — Layer 2 Tunneling Protocol
- **PPTP** — Point-to-Point Tunneling Protocol
- **WireGuard** — secure VPN tunnel (ChaCha20Poly1305)
- **MACsec** — IEEE 802.1AE media access control security
- **6LoWPAN** — IPv6 over Low-Power Wireless Personal Area Networks
- **IPoIB** — IP over InfiniBand
- **IPsec** — IP Security (AH/ESP, IKEv2 via pf_key)
- **Bonding** — link aggregation (balance-rr, active-backup, balance-xor, broadcast)
- **XDP** — eXpress Data Path (per-interface hook with DROP/PASS/TX)
- **Bridge** — learning bridge with STP, IGMP snooping, VLAN filtering
- **STP** — Spanning Tree Protocol (802.1D)
- **GARP** — Generic Attribute Registration Protocol
- **LACP** — Link Aggregation Control Protocol (802.3ad)
- **MRP** — Multiple Registration Protocol (802.1Q)
- **LLDP** — Link Layer Discovery Protocol
- DHCP client with lease management, renewal
- DNS stub resolver with caching, /etc/resolv.conf
- HTTPd, Telnetd, SSHd (SSH v2), FTP client
- Netfilter: packet filtering, NAT, conntrack (helpers: FTP, SIP, etc.), nf_tables, ebtables, arptables
- Traffic control: HTB, fq_codel, pfifo_fast, cake
- IPVS load balancing, IGMP snooping
- Network namespaces, TUN/TAP interfaces
- RPS/RFS/XPS: receive/transmit packet steering

### PCI / ACPI / Device Drivers
- **PCI** — full enumeration, MSI/MSI-X, PCIe capabilities, AER (persistent logging), DPC, SR-IOV (VT-d), hotplug
- **ACPI** — tables (MADT, DSDT, SSDT, FADT, HPET, LPIT, NFIT), EC, thermal zones, power button, platform profile, CPPC, battery
- **PNP** — Plug and Play device enumeration
- **Storage:**
  - **NVMe** — multi-queue (admin/submission/completion), sanitize, PMR (persistent memory region), multipath
  - **AHCI** — NCQ (32 tags), port multiplier, TRIM
  - **ATA PIO** — legacy IDE, ATAPI
  - **virtio-blk** — multi-queue virtio block device
  - **virtio-scsi** — virtio SCSI controller
  - **USB MSC** — USB mass storage class, UAS
  - **NBD** — Network Block Device
  - **Loop** — loopback block device
  - **Ramdisk** — ramdisk block device
  - **PMEM** — persistent memory
  - **MD (RAID)** — RAID0, RAID1, RAID10 (DM-RAID)
  - **Multipath** — device mapper multipath I/O
  - **iSCSI** — iSCSI initiator (TCP login, SCSI READ10/WRITE10 over PDUs)
  - **NVMe-oF** — NVMe over Fabrics target (TCP, fabric connect)
  - **FCoE** — Fibre Channel over Ethernet initiator
  - **DRBD** — Distributed Replicated Block Device (Protocol C)
  - **Ceph/RBD** — RADOS block device client
  - **dm-era** — Device mapper era (write tracking) target
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
  - **Wifi** — USB WiFi (stub)
  - **Gadget** — USB gadget framework (UDC core, mass storage function)
- **Virtio:**
  - **virtio-blk** — multi-queue block
  - **virtio-net** — LRO
  - **virtio-gpu** — GPU/display
  - **virtio-input** — input devices
  - **virtio-rng** — entropy source
  - **virtio-scsi** — SCSI controller
  - **virtio-console** — console device
  - **virtio-fs** — filesystem device (FUSE protocol)
  - **virtio-iommu** — IOMMU device (domain isolation)
- **Virtualization Backend:**
  - **KVM** — minimal hypervisor (VMX/SVM, EPT/NPT, /dev/kvm ioctl, single vCPU)
  - **vhost-scsi** — in-kernel virtio-scsi target
  - **vhost-blk** — in-kernel virtio-blk target
  - **VFIO** — userspace driver interface (/dev/vfio containers)
  - **vDPA** — virtio Data Path Acceleration framework
  - **Balloon** — virtuall balloon with compaction support
  - **UIO** — Userspace I/O framework
- **Display / Graphics:**
  - **Framebuffer** — VGA text mode, Bochs VBE, simplefb
  - **Intel GPU** — integrated graphics
  - **DRM** — Direct Rendering Manager skeleton (drm_core, drm_gem, drm_dumb, bochs_drm)
  - **EDID** — Extended Display Identification Data parsing
  - **Boot splash** — logo, version string, progress spinner, fade-out
- **Audio:**
  - **AC97** — PCM playback, recording
  - **Sound Core** — OSS sound layer, MIDI
  - **Sndstat** — sound status
- **Miscellaneous:**
  - **PS/2** — keyboard, mouse
  - **Serial** — COM1/COM2 serial console (16550 UART), early serial (pre-MMU)
  - **HPET** — High Precision Event Timer
  - **PIT** — Programmable Interval Timer
  - **RTC** — Real-Time Clock (CMOS + HPET + periodic)
  - **CMOS** — CMOS/NVRAM access
  - **Watchdog** — hardware watchdog timer with pretimeout NMI, configurable governor
  - **SPI** — Serial Peripheral Interface bus
  - **I2C / SMBus** — I2C and SMBus controllers (multi-master, slave mode)
  - **GPIO** — General Purpose I/O with interrupt support
  - **EDAC** — Error Detection And Correction (DRAM error reporting), extended RAS
  - **GHES** — Generic Hardware Error Source (ACPI)
  - **I3C** — Improved Inter-Integrated Circuit bus (master/slave)
  - **TPM TIS** — Trusted Platform Module 2.0 TIS interface (PCR, attestation, RNG seed)
  - **DMI** — Desktop Management Interface (SMBIOS)
  - **IPMI KCS** — Intelligent Platform Management Interface
  - **Firmware Class** — generic firmware loading API
  - **pvpanic** — QEMU panic device
  - **ivshmem** — QEMU inter-VM shared memory
  - **9pnet virtio** — 9P2000.L virtio transport
  - **VMware balloon** — memory balloon driver
  - **VMware PVSCSI** — paravirtual SCSI
  - **RAS netlink** — hardware error reporting via NETLINK_RAS
- **Power Management:**
  - ACPI thermal zones, battery, cpufreq governors (ondemand, conservative, userspace, schedutil)
  - Cpuidle (ladder, teo, haltpoll, menu), devfreq, energy model
  - Suspend/resume (S2RAM, S2Idle), PM QoS, PM runtime
  - RAPL — Running Average Power Limit

### Architecture Support
- **x86-64** — long mode, 4-level paging, canonical addresses
- **SMP** — multi-CPU via ACPI MADT, APIC, x2APIC, TSC deadline timer, CPU hotplug
- **IOMMU** — Intel VT-d DMA remapping
- **SMAP/SMEP/UMIP** — kernel protection features
- **CET** — Control-flow Enforcement (shadow stack, IBT)
- **KPTI** — Kernel Page Table Isolation with PCID/INVPCID
- **FSGSBASE** — userspace TLS via MSRs
- **RDPID** — fast TSC/processor ID
- **NX** — non-executable page enforcement
- **PKEYS** — Memory Protection Keys (pkey_alloc/free/mprotect/mpx)
- **Memory hotplug** — physical memory add/remove at runtime

### Security Features
- **Seccomp-BPF** — syscall filtering (SECCOMP_RET_KILL/TRAP/ALLOW/LOG/TRACE)
- **Landlock** — path-based mandatory access control
- **SMACK** — Simplified Mandatory Access Control (label-based MAC, SMACK64 xattr)
- **YAMA** — ptrace scope restriction (levels 0-4)
- **IMA** — Integrity Measurement Architecture (measure, appraise, policy)
- **EVM** — Extended Verification Module
- **IPE** — Integrity Policy Enforcement
- **TPM 2.0** — attestation (TPM2_Quote), AIK NVRAM storage, RNG seeding
- **UEFI Secure Boot** — variable verification, module signing lockdown
- **Lockdown** — kernel lockdown mode (integrity, confidentiality)
- **PKEY** — Memory Protection Keys for userspace
- **Stackleak** — kernel stack erasure on syscall exit
- **KASLR** — kernel ASLR, module KASLR, strong KASLR
- **KASAN-light** — kernel address sanitizer (out-of-bounds, use-after-free)
- **KFENCE** — low-overhead memory error detector
- **KCSAN** — Kernel Concurrency Sanitizer (data race detection)
- **KMSAN** — Kernel Memory Sanitizer (uninitialized memory detection)
- **UBSan** — Undefined Behavior Sanitizer
- **kmemleak** — kernel memory leak detector
- **Module signing** — cryptographically signed modules (SHA-256/RSA, PKCS#7)
- **Audit** — audit subsystem, syscall auditing, path auditing
- **dmesg_restrict** — kernel address display control
- **kptr_restrict** — pointer display restriction
- **Kernel stack canaries** — -fstack-protector-strong (per-task random canary)
- **Stack guard pages** — guard pages between kernel stacks
- **SLAB poisoning** — freed memory poisoning, freelist randomization
- **RANDSTRUCT** — compile-time structure layout randomization
- **Namespaces:** PID, mount, network, user (full UID/GID maps), cgroup, IPC, time

### Performance & Scaling
- **EEVDF** — Earliest Eligible Virtual Deadline First scheduler
- **MGLRU** — Multi-Generational LRU page reclaim
- **Ftrace** — function tracer, function graph tracer, trace events, tracepoints (sched, IRQ, timer, net, block, mm, syscall), trace_printk
- **Perf events** — hardware PMU counters, context-switch/page-fault/mmap tracking, sampling, flame graphs
- **PSI** — Pressure Stall Information (CPU, memory, IO) with /proc/pressure/
- **Kprobes/kretprobes** — dynamic instrumentation
- **Uprobes** — user-space dynamic tracing (int3+TF single-step)
- **eBPF** — verifier, JIT compiler, maps (array/hash/perf), kprobe/tracepoint/XDP programs, helpers
- **Jump labels** — static key patching
- **RCU** — expedited grace periods, nocb mode (per-CPU offloading), stall detection, polled, priority boosting
- **PELT** — Per-Entity Load Tracking for scheduler
- **MCS lock** — optimistic spinning for mutexes
- **Workqueues** — bound and unbound worker pools, OOM handling
- **Softirq/kSoftirqd** — fair softirq scheduling
- **Tasklets** — bottom-half deferral mechanism
- **RPS/RFS/XPS** — receive/transmit packet steering
- **I/O schedulers** — deadline, CFQ, noop
- **TLB shootdown** — IPI-based TLB flushing, avoidance for single-thread processes
- **Per-CPU caches** — lockless page and slab caches
- **NOHZ** — tickless idle, adaptive tick
- **Core scheduling** — hyperthread-aware security (SMT sibling isolation)
- **NUMA balancing** — page migration scanner, per-node access tracking

### Virtualization
- **KVM** — minimal hypervisor: VMX/SVM detection, EPT/NPT, /dev/kvm ioctls (CREATE_VM, CREATE_VCPU, SET_USER_MEMORY_REGION, RUN), single guest/vCPU, VM exit handling (EPT violation, I/O, HLT)
- **vhost-scsi** — in-kernel virtio-scsi SCSI target (READ10, WRITE10, INQUIRY)
- **vhost-blk** — in-kernel virtio-blk block target (READ, WRITE, FLUSH, DISCARD)
- **VFIO** — userspace driver interface (/dev/vfio containers, groups, device assignment)
- **vDPA** — virtio Data Path Acceleration: abstract ops, software vDPA, virtio-vdpa adapter
- **Balloon** — virtio-balloon with page isolation/migration for compaction
- **Virtio-fs** — FUSE protocol filesystem device
- **Virtio-iommu** — IOVA→phys mapping, domain-based isolation

### User Space
- Built-in shell: 356+ commands (coreutils, networking, admin, dev, debug, games)
- Scripting: variables (including arrays), pipelines, redirection, job control, background tasks, conditionals, while/for loops, arithmetic expansion $((...)), heredocs
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
- Pod abstraction: create, start, stop, add containers, health checks (liveness, readiness, startup)
- Service abstraction: stable endpoints with round-robin load balancing
- Controllers: Deployment (rolling updates), StatefulSet, DaemonSet, Job/CronJob
- Container checkpoint/restore, security scanning, seccomp notify
- Scheduler policies: spread, binpack, random (with node affinity, taints/tolerations)
- HPA/VPA: horizontal/vertical pod autoscaling
- Custom Resource Definitions (CRDs) and operator framework
- RBAC: role-based access control, secrets management
- Resource limits, cgroup interfaces (CPU, memory, PID, IO, cpuset, freezer)

## Container Platform

The kernel includes a complete container platform with an OCI-compatible runtime, Kubernetes-inspired orchestration, and CLI tooling — all running in-kernel.

### Container Runtime

**Files:** `src/container/runtime.c`, `src/container/config.c`, `src/container/state.c`, `src/container/storage.c`, `src/container/image.c`, `src/container/network.c`

The container runtime provides OCI-compatible lifecycle management:

- **Lifecycle operations:** create, start, stop, delete, exec, attach, logs, pause/unpause, wait, top, stats
- **cgroup integration:** per-container CPU, memory, and PID cgroup controllers. CPU shares, quota/throttle, memory limits with OOM kill, PID limits, IO controller, cpuset, freezer.
- **Namespace isolation:** PID, mount, network, user, cgroup, IPC, time, UTS namespaces for each container.
- **Image management:** pull from OCI-compatible registries, push, tag, list, remove, prune, save/load. Supports layer caching and manifest v2.
- **Storage drivers:** OverlayFS with layer management (whiteout files, opaque directories). Copy-on-write with per-layer reference counting.
- **Container inspect:** full JSON metadata dump including mounts, network settings, environment, and resource limits.
- **Checkpoint/restore:** full container state checkpoint to disk and restore with CRIU-like kernel-level support.
- **Security scanning:** in-kernel vulnerability scanning of container images against known CVEs.
- **Seccomp notify:** per-container seccomp profiles with notify-on-violation.

### Orchestration Features

**Files:** `src/container/orch.c`, `src/container/controllers.c`

- **Pods:** groups of containers sharing network namespace, volumes, and lifecycle. Readiness, liveness, and startup probes.
- **Services:** stable virtual IP endpoints backed by label-selected pods. IPVS load balancing (round-robin, least connections, hash) or iptables DNAT. DNS discovery via built-in cluster DNS.
- **Controllers:** Deployment (replica management with rolling updates), StatefulSet (ordered pods with stable identities), DaemonSet (one pod per node), Job/CronJob (batch workloads), ReplicaSet.
- **HPA/VPA:** horizontal/vertical pod autoscaling with configurable metrics and stabilization windows.
- **Custom Resource Definitions (CRDs):** define custom resource types with schema validation.
- **Operator framework:** controllers that watch custom resources and reconcile cluster state.
- **Scheduler policies:** spread, binpack, random with node affinity, taints/tolerations, resource-based filtering.
- **RBAC/Secrets:** role-based access control, encrypted secret storage with pod injection.

### CLI Tools

| Command | Description |
|---------|-------------|
| `ctr` | Container runtime CLI — create, start, stop, exec, logs, pull, push images |
| `crictl` | CRI-compatible debug tool — inspect containers, pods, and images |
| `orchctl` | Orchestration CLI — manage pods, services, deployments, controllers |
| `compose` | Compose file runner — `compose up/down/ps/logs` from docker-compose YAML |

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
```

The kernel has 200+ built-in tests plus expandable KUnit test suites covering: scheduler, VMM, PMM, slab, IPC, VFS, TCP, UDP, socket API, device drivers, security, and power management. Tests run in QEMU and report PASS/FAIL via serial. CI enforces all tests pass on every commit.

## Architecture

The OS is a monolithic x86-64 kernel booting via Multiboot1 (GRUB/QEMU). The kernel runs in the high half of the virtual address space (0xFFFF800000000000+) while userspace occupies the lower 512 GB. The boot sequence transitions from 32-bit to 64-bit long mode in boot.asm, then calls kernel_main which initializes subsystems in a linear sequence: GDT → IDT → PIC → early serial → PMM → VMM → heap → slab → SMP → process → timer → keyboard → serial → PCI → storage → VFS → networking → ACPI → TPM → syscalls → boot splash → shell → interrupts enabled.

Key design decisions:
- **Monolithic architecture** with all drivers in-kernel for performance
- **High-half VMA layout** with PHYS_TO_VIRT mapping via kernel PML4 entry 256
- **Bitmap PMM** with per-CPU hot caches and page reference counting for COW
- **4-level paging** with 2MB/1GB huge page support, KPTI for Meltdown mitigation
- **Preemptible kernel** with RCU for read-mostly data structures
- **EEVDF + CFS** scheduler with 256 process slots
- **VFS layer** with dentry caching, mount table, and 30+ filesystem implementations
- **Custom TCP/IP stack** with 8 congestion control algorithms and 30+ protocol implementations
- **356+ built-in shell commands** for system administration and development
- **eBPF + KVM + vhost** for programmability and virtualization

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
