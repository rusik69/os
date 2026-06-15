# OS Kernel Architecture

## Overview

This OS is an x86-64 long-mode hobby kernel booting via Multiboot1 (GRUB/QEMU). It implements a monolithic kernel architecture with a high-half virtual memory layout. All kernel code and data resides at virtual addresses `0xFFFF800000000000+` while the lower 512 GB of virtual address space is available per-process for userspace.

**Scale:** ~317K lines of C across 999 source files, 453 headers, 80+ subsystems. 15MB kernel binary. 226 loadable kernel modules. 356+ shell commands. Boots in QEMU and real hardware.

```
Boot:  Multiboot1 -> boot.asm (32-bit) -> long_mode_entry -> kernel_main
        ↓                ↓                    ↓
        GRUB loads       Identity-map 1 GB    Enter 64-bit mode
        at 0x100000      PAE + paging         Initialize subsystems
```

## Memory Layout

```
Physical Memory:
  0x0000000000000000 - 0x000000000009FC00  : Low memory (IVT, BDA, EBDA)
  0x0000000000100000 - ...                  : Kernel .text/.data/.bss
  ...                                       : Free (PMM managed)
  0x0000000008000000 - ...                  : PCI MMIO, ACPI tables
  0x0000000100000000+                       : Hotplug memory / high phys

Virtual Memory (per-process):
  0x0000000000000000 - 0x00007FFFFFFFFFFF  : Userspace (lower half)
  0xFFFF800000000000 - 0xFFFFFFFFFFFFFFFF  : Kernel (higher half)
      0xFFFF800000000000 - 0xFFFF8000100000 : Kernel .text (PHYS_TO_VIRT)
      0xFFFF8000100000 - 0xFFFF8000200000  : Kernel .rodata/.data/.bss
      0xFFFF8000200000 - ...                : Kernel heap (kmalloc)
      0xFFFF800100000000 - 0xFFFF800140000000: Module region (64MB, RX/RO/RW)
      ...                                   : Kernel stacks + guard pages
      ...                                   : io_uring SQ/CQ rings
```

The kernel uses a `PHYS_TO_VIRT` macro: `virt = (phys + KERNEL_VMA_OFFSET)` where `KERNEL_VMA_OFFSET = 0xFFFF800000000000`. PML4 entry 256 maps the kernel's PDPT which itself maps the kernel's PDs — everything from 0xFFFF800000000000 upward.

## Boot Sequence

### Phase 1: boot.asm (32-bit)
1. GRUB loads the kernel at physical `0x100000` via the Multiboot1 header
2. `_start` (32-bit): clears BSS, sets up identity-mapped page tables (PAE format)
3. PML4[0] = identity map PDPT (covers first 1GB)
4. PML4[256] = same PDPT → high-half mapping after paging enabled
5. Enables PAE → sets EFER.LME (long mode) → enables paging (CR0.PG)
6. Far jump to `long_mode_entry` (64-bit CS)

### Phase 2: long_mode_entry (64-bit)
7. Sets up GDT with 64-bit code/data segments (ring 0 and ring 3)
8. Loads GS/FS base for per-CPU data (MSR GS_BASE/FS_BASE)
9. Sets up initial kernel stack (4 pages + guard page)
10. Calls `kernel_main`

### Phase 3: kernel_main
11. Initialization order (linear, ~35 steps):
    1. `early_serial_init()` — early serial output before full console
    2. `gdt_init()` — final GDT with TSS, IST entries
    3. `idt_init()` — IDT with interrupt gates and IST assignments
    4. `pic_remap()` — remap PIC IRQs to vectors 0x20-0x2F
    5. `pmm_init()` — detect physical memory from Multiboot, build bitmap
    6. `vmm_init()` — finalize page tables, enable NX, PAT
    7. `heap_init()` — set up kmalloc arena
    8. `slab_init()` — initialize kmem_cache subsystem
    9. `smp_init()` — detect APs via ACPI MADT, send INIT-SIPI-SIPI
    10. `process_init()` — create idle process, process table
    11. `timer_init()` — program PIT/HPET/TSC deadline timer (100 Hz) + hrtimer
    12. `keyboard_init()` — PS/2 keyboard interrupt handler
    13. `serial_init()` — COM1/COM2 serial console
    14. `pci_init()` — enumerate PCI bus, discover devices
    15. `ahci_init()`, `ata_init()` — storage device detection
    16. `nvme_init()` — NVMe subsystem initialization
    17. `fs_init()` — mount root filesystem (tmpfs or disk)
    18. `net_init()` — initialize networking stack + NIC
    19. `efi_runtime_init()` — UEFI runtime services
    20. `acpi_init()` — parse ACPI tables, battery, thermal
    21. `tpm_init()` — TPM 2.0 driver initialization
    22. `syscall_init()` — set up MSR_LSTAR syscall entry
    23. `module_init()` — load built-in modules from initramfs
    24. `kvm_init()` — KVM virtualization setup
    25. `bpf_init()` — eBPF subsystem initialization
    26. `io_uring_init()` — async I/O ring setup
    27. `shell_init()` — launch shell on /dev/console
    28. `asm("sti")` — enable interrupts → idle loop

## Subsystem Architecture

### Physical Memory Manager (PMM)

**File:** `src/memory/pmm.c`

The PMM is a bitmap-based allocator with per-CPU hot caches. The physical memory map is scanned on boot (from Multiboot info) and all available regions are tracked in a bitmap where each bit represents one 4 KB page.

```
pmm_bitmap: [bit 0][bit 1][bit 2] ...
            page 0  page 1  page 2

Key operations:
  pmm_alloc_frame()   — find first zero bit, set it, return physical addr
  pmm_free_frame()    — clear bit (force-free, used for page-table pages)
  pmm_unref_frame()   — decrement refcount, free if 0 (COW-safe)
  pmm_ref_frame()     — increment refcount for COW sharing
```

Features:
- Page reference counting for COW fork support
- Per-CPU page cache (hot list) for lockless fast allocation
- Contiguous allocation (order > 0) with fallback
- Memory hotplug support
- OOM-safe return (returns NULL instead of panicking)

### Virtual Memory Manager (VMM)

**File:** `src/memory/vmm.c`

4-level page tables (PML4 → PDPT → PD → PT) mapped via `PHYS_TO_VIRT`. Each process gets its own PML4 with userspace mappings.

```
PML4 (per-process) → PDPT (shared kernel entries) → PD → PT → 4 KB pages
                     ↓ 2 MB pages (if PD entry is huge page)
```

Key operations:
- `vmm_map_page(virt, phys, flags)` — map a single 4KB page
- `vmm_unmap_page(virt)` — unmap, optionally free phys
- `vmm_get_physaddr(virt)` — walk page tables, return physical address
- `vmm_map_large_page(virt, phys, level)` — map 2MB or 1GB huge page

Supports: copy-on-write fork (via `vmm_clone_pml4`), NX/SMEP enforcement, MAP_POPULATE, MMIO mapping for device drivers, demand paging with lazy allocation, NUMA-aware page placement, transparent huge pages (THP), KSM (Kernel Same-page Merging).

**Huge Page Migration** (`src/memory/hugepage_migration.c`): Moves transparent huge pages between physical locations without splitting. Uses PMD-level migration entries. Called from NUMA balancing and memory compaction paths.

**Page Pool** (`src/memory/page_pool.c`): Pre-allocated page caches for fast NIC driver buffer allocation. NAPI-compatible, with DMA address caching and recycling.

**NUMA Balancing** (`src/memory/numa_balancing.c`): Periodic page table scanning to detect NUMA locality faults, triggering automatic page migration to the accessing node. PTE access/dirty bit tracking with multi-second scan intervals.

**KSM (Kernel Same-page Merging)** (`src/memory/ksm.c`): Scans anonymous pages, merges identical pages into single COW pages. Configurable scan rate, page age thresholds, and sleep intervals.

**MGLRU (Multi-Gen LRU)** (`src/memory/mglru.c`): Alternative page reclaim algorithm using multiple generation lists instead of a single LRU. Reduces page reclaim overhead, improves OOM behavior under memory pressure.

### Slab Allocator

**File:** `src/memory/slab.c`

General-purpose object cache based on the classic slab design. Manages objects of fixed size efficiently with per-CPU caches.

```
kmem_cache (for each size/type):
  → cpu_cache[] (per-CPU free list — lockless fast path)
  → slabs_full[page]     → objects in use
  → slabs_partial[page]  → some free objects
  → slabs_free[page]     → all objects free

  Object layout: [obj | redzone] [obj | redzone] ...
```

Features:
- Per-CPU cache arrays for cache-hot lockless allocation
- Object poisoning (0x6a for alloc, 0x6b for free)
- Redzone bytes at object boundaries (buffer overflow detection)
- Random freelist order (heap exploit mitigation)
- kmem_cache_create/destroy for typed allocations

### Scheduler

**File:** `src/process/scheduler.c`

Multi-class scheduler with CFS (Completely Fair Scheduler) and EEVDF (Earliest Eligible Virtual Deadline First) for normal tasks, plus prioritized scheduling for RT tasks.

```
Scheduling classes:
  SCHED_DEADLINE  → EDF, budget replenishment (GRUB)
  SCHED_FIFO      → run until blocked, priority-sorted
  SCHED_RR        → round-robin with RT timeslice
  SCHED_OTHER     → CFS or EEVDF vruntime scheduling
  SCHED_IDLE      → lowest priority, background only

Per-CPU runqueue:
  → cfs_rq: red-black tree ordered by vruntime (CFS) or eligible deadline (EEVDF)
  → rt_rq: priority-bitmap + linked lists
  → deadline_rq: rb_root ordered by deadline

Scheduler decisions:
  pick_next_task() → highest priority class → pick within class
  Context switch via switch.asm (save/restore registers, CR3)
```

Features:
- PELT (Per-Entity Load Tracking): running average of CPU utilization
- EEVDF: eligible virtual deadline scheduling for better latency fairness
- NUMA-aware task placement with automatic migration
- Load balancing across CPUs (periodic pull, idle push)
- Core scheduling (hyperthread safety)
- CPU hotplug support with task migration
- NO_HZ_FULL: adaptive tick on isolated CPUs
- Idle injection (`src/kernel/idle_inject.c`): forced idle cycles for thermal management and power capping

### Interrupt Handling

Interrupts flow through a layered hierarchy:

```
Device IRQ → I/O APIC → Local APIC → IDT entry → irq_handler → driver
             (or PIC)   (or x2APIC)

Exception → CPU directly → IDT entry → exception_handler → fault.c

IPI → Local APIC → smp_call_function / TLB shootdown
```

Key structures:
- IDT: 256 entries, IST for double-fault (#DF), NMI, MCE
- IRQ descriptor table: maps vector → handler, data, flags
- Softirq: deferred interrupt processing (ksoftirqd)
- Tasklet: lightweight deferred work on softirq context
- Workqueue: process-context deferred work, bound/unbound pools

Timers:
- PIT (100 Hz legacy), HPET (high precision), TSC deadline (per-CPU)
- High-resolution timers (hrtimer): O(1) red-black tree, per-CPU, CLOCK_MONOTONIC/REALTIME/BOOTTIME
- Timerfd: userspace timer via file descriptor
- hrtimer slack for power-efficient coalescing

### io_uring

**File:** `src/kernel/io_uring.c`

Async I/O framework with shared submission/ completion queues between kernel and userspace.

```
Submission Queue (SQ) — ring buffer in shared memory
  → 256 entries max, IORING_OP_{READV,WRITEV,NOP,FASYNC,POLL_ADD,POLL_REMOVE,...}
  → sqe->opcode, fd, off, addr, len, flags, user_data

Completion Queue (CQ) — ring buffer in shared memory
  → cqe->res, user_data, flags (IORING_CQE_F_MORE)
  → Non-polled (IRQ-driven completion)

Submission flow:
  userspace fills SQE → io_uring_enter() syscall → kernel consumes SQE → I/O issued → completion posted to CQ
```

Features: supported operations (READV, WRITEV, NOP, FSYNC, POLL_ADD/REMOVE), IORING_SETUP_SQPOLL, batched submission/completion, user_data-based completion matching.

### SMP (Symmetric Multi-Processing)

**File:** `src/kernel/smp.c`

AP bringup via ACPI MADT (x2APIC entries). Uses the standard INIT-SIPI-SIPI sequence.

```
BSP → reads MADT → discovers AP processor entries
    → writes AP trampoline to PCPU low memory (0x8000)
    → sends INIT IPI → SIPI → SIPI
    → APs execute trampoline → enable paging → jump to ap_main()
    → ap_main() → local APIC init → enable interrupts → idle loop
```

Features:
- per-CPU data via GS segment (GS_BASE MSR)
- SMP TLB shootdown via IPI
- Multi-queue device support (NVMe, virtio, e1000)
- CPU hotplug (offline/online at runtime)
- NUMA topology discovery (ACPI PPTT/SRAT)
- Processor grouping for affinity control

### Virtual Filesystem (VFS)

**File:** `src/fs/fs.c`, `src/kernel/vfs.c`

The VFS layer provides a Linux-like abstraction over multiple filesystem implementations.

```
System calls → VFS layer → filesystem ops
  open()       vfs_open()   → fs->open()
  read()       vfs_read()   → fs->read()
  write()      vfs_write()  → fs->write()
  close()      vfs_close()  → fs->close()
  mount()      vfs_mount()  → fs->mount()

Path resolution:
  resolve(path) → walk dentries → find inode
  Cached dentries → dcache lookup → or → traverse on disk
```

Mounted filesystems are tracked in a mount table with propagation types (SHARED/SLAVE/PRIVATE) for namespace support. The VFS supports: file locks (POSIX advisory), inotify/fanotify, xattr, O_NONBLOCK, O_DIRECT, fallocate, splice, POSIX ACLs, per-user quotas.

### Supported Filesystems

| FS | Type | Read/Write | Key Features |
|----|------|-----------|--------------|
| **tmpfs** | In-memory | R/W | Dynamic sizing, symlinks, device nodes, O_TMPFILE |
| **ramfs** | In-memory | R/W | Simple RAM-backed FS without size limit |
| **fat32** | Disk | R/W | FAT12/16/32, LFN read+write, volume labels |
| **ext2** | Disk | R/W | Sparse files, large files, symlinks, fast symlinks, HTree |
| **ext4** | Disk | R/W | Extents, HTree dirs, large inodes, nanosecond timestamps |
| **btrfs** | Disk | R/O | Copy-on-Write, extents, checksums, subvolumes |
| **ntfs** | Disk | R/O | NTFS basic read support, MFT-based directory traversal |
| **exFAT** | Disk | R/O | Large file support, exFAT allocation table |
| **hfsplus** | Disk | R/O | HFS+ with B-tree catalogs and extents |
| **reiserfs** | Disk | R/O | ReiserFS 3.x, B*-tree directory structure |
| **iso9660** | Disk | RO | Rock Ridge (POSIX), Joliet (Unicode), multi-session |
| **squashfs** | Disk | RO | Compressed read-only filesystem |
| **cramfs** | Disk | RO | Compressed ROM filesystem |
| **cifs** | Network | R/W | SMB/CIFS client protocol, NTLM auth, oplocks |
| **nfs** | Network | R/W | NFS client (v2/v3) |
| **nfsd** | Network | R/W | NFS server (v3), export table, fsid-based exports |
| **tarfs** | Archive | RO | Embedded initramfs, TAR archive mount |
| **cpio** | Archive | RO | cpio archive mount |
| **romfs** | Archive | RO | Simple read-only filesystem |
| **procfs** | Pseudo | RO | /proc/{uptime,meminfo,cpuinfo,stat,self,interrupts,...} |
| **sysfs** | Pseudo | RO | kobject tree, kernel parameters, device hierarchy |
| **devfs** | Pseudo | R/W | Dynamic device node creation, hotplug |
| **debugfs** | Pseudo | R/W | Kernel debug data, register dumps |
| **overlay** | Union | R/W | Union mount (upper + lower dirs, whiteouts, copy-up) |
| **FUSE** | User | R/W | Userspace filesystem via FUSE protocol |
| **minix** | Disk | R/W | Minix filesystem (v1/v2/v3) |
| **ufs** | Disk | R/W | Unix File System (FFS) |
| **sysv** | Disk | R/W | System V filesystem |
| **hfs** | Disk | R/W | Hierarchical File System (Mac) |
| **erofs** | Disk | RO | Enhanced Read-Only Filesystem |
| **f2fs** | Disk | R/W | Flash-Friendly Filesystem |
| **jffs2** | Flash | R/W | Journaling Flash File System v2 |
| **nilfs2** | Disk | R/W | Log-structured filesystem |

On-disk filesystem source files:

| FS | Source | Notes |
|----|--------|-------|
| FAT32 | `src/fs/fat32.c`, `src/fs/vfat_shortname.c` | VFAT long filename support |
| ext2 | `src/fs/ext2.c` | HTree directory indexing |
| ext4 | `src/fs/ext4.c` | Extents, flex_bg groups |
| btrfs | `src/fs/btrfs.c` | COW, checksum verification |
| ntfs | `src/fs/ntfs.c` | MFT, attribute resolution |
| exFAT | `src/fs/exfat.c` | exFAT directory/bitmap |
| hfsplus | `src/fs/hfsplus.c` | B-tree catalog search |
| reiserfs | `src/fs/reiserfs.c` | ReiserFS block keying |
| CIFS | `src/fs/cifs.c` | SMB dialects, NTLM auth |
| nfsd | `src/fs/nfsd.c` | NFS v3 RPC server |
| iso9660 | `src/fs/iso9660.c` | Rock Ridge + Joliet |
| tmpfs | `src/fs/tmpfs.c` | Dynamic memory-backed |
| procfs | `src/fs/procfs.c` | Process info, system stats |
| sysfs | `src/fs/sysfs.c` | Kernel object tree |
| devfs | `src/fs/devfs.c` | Device node management |
| tarfs | `src/fs/tarfs.c` | TAR archive support |
| cpio | `src/fs/cpio.c`, `src/fs/initramfs.c` | Initramfs support |
| romfs | `src/fs/romfs.c` | Minimal ROM FS |
| squashfs | `src/fs/squashfs.c` | Compressed FS |
| overlay | `src/kernel/overlay.c`, `src/fs/overlay_enhance.c` | Union mounts |
| FUSE | `src/fs/fuse.c` | Userspace filesystem |

### Block Cache and Buffer Cache

The kernel uses a two-tier caching strategy for block-level I/O:

**Page Cache** (`src/fs/page_cache.c`, `src/include/page_cache.h`):

Generic file data cache that caches whole pages (4 KB) keyed by `(inode, block_index)`. 1024 entries managed with LRU eviction.

```
page_cache_entry {
    uint64_t  ino;          /* inode number */
    uint64_t  block;        /* block index within file */
    uint64_t  phys_addr;    /* physical address of cached page */
    void     *data;         /* kernel virtual address */
    int       flags;        /* PAGE_CACHE_DIRTY, etc. */
    uint64_t  last_access;  /* LRU timestamp */
    int       prefetched;   /* loaded by readahead, not yet accessed */
}
```

Key features:
- **Dirty writeback** — configurable background ratio (default 10%) and throttle ratio (50%). Pages are flushed to disk when thresholds are crossed, or on explicit `sync()`/`fsync()`.
- **Readahead** — window-based prefetching (`READAHEAD_WINDOW_MIN`/`MAX`, configurable). Adaptive window sizing based on access pattern.
- **Working-set estimation** — tracks active cache entries via exponential decay on access counters.
- **Cache statistics** — hits, misses, evictions, dirty forced writes exposed via `/proc/cachestat`.

**Buffer Cache** (`src/fs/bufcache.c`):

Lower-level sector cache (512 bytes per entry) used primarily by FAT32 and other block-level users. 64 entries with LRU eviction, hash-bucket lookup, and dirty tracking.

```
bc_entry {
    uint64_t  lba;          /* sector address */
    uint8_t   dev_id;       /* block device id */
    uint8_t   valid;        /* data valid */
    uint8_t   dirty;        /* modified, needs write-back */
    uint8_t   data[512];    /* cached sector data */
}
```

- **LRU eviction** with a doubly-linked list.
- **Dirty write-back** on eviction.
- **Stats:** hits, misses, writes exposed for diagnostics.

**Block I/O Scheduler** (`src/fs/iosched.c`): Implements deadline and CFQ-like policies for merging and reordering block requests.

**Initramfs** (`src/fs/initramfs.c`): Embedded initramfs on disk image, built from cpio/tar archives. Mounted early at boot before the root filesystem is available. Also loads kernel modules from `/modules/`.

### Mount Table and Namespace Support

**Global mount table:** `struct vfs_mount mounts[VFS_MAX_MOUNTS]` (16 entries). Each mount entry records mountpoint path, filesystem ops, private data, flags, bind mount source, encryption state.

**Mount namespace** (`src/kernel/mnt_namespace.c`, `src/include/mnt_namespace.h`):

Per-process mount namespace with copy-on-clone semantics:

```
struct mnt_namespace {
    int              refcount;
    struct vfs_mount mounts[VFS_MAX_MOUNTS]; /* per-ns mount table */
    int              num_mounts;
};
```

- **Root namespace** — wraps the global mount table. Created once during VFS init.
- **CLONE_NEWNS** — creates a new namespace with a deep copy of the parent's mount table.
- **Propagation types** — SHARED, SLAVE, PRIVATE (implemented in `src/kernel/fs_mount_prop.c`).

## Networking Stack

The network stack is a layered in-kernel TCP/IP implementation with a BSD socket API, netfilter packet filtering, and support for multiple NIC drivers.

```
Application Layer
     ↕  sys_socket/sys_send/sys_recv/etc.
┌─────────────────────────────────────────────┐
│  Socket Layer  (src/net/socket.c)           │
│  File descriptor integration, protocol      │
│  dispatch (AF_INET, AF_INET6, AF_UNIX,      │
│  AF_PACKET, AF_NETLINK, AF_CAN, AF_TIPC,   │
│  AF_XDP)                                    │
├─────────────────────────────────────────────┤
│  Transport Layer                            │
│  ┌───────────────────────────────────────┐  │
│  │ TCP (src/net/net_tcp.c)               │  │
│  │   Full state machine, congestion      │  │
│  │   control (Reno, CUBIC, BBRv1/v2,    │  │
│  │   BIC, Vegas, Westwood, Hybla,        │  │
│  │   Illinois), RACK loss detection,     │  │
│  │   TFO, SYN cookies, SACK, MD5,       │  │
│  │   MPTCP                              │  │
│  └───────────────────────────────────────┘  │
│  ┌───────────────────────────────────────┐  │
│  │ UDP (src/net/net_udp.c)               │  │
│  │   Connected sockets, multicast,       │  │
│  │   broadcast, checksums                │  │
│  └───────────────────────────────────────┘  │
│  ┌───────────────────────────────────────┐  │
│  │ SCTP / DCCP / L2TP / PPTP            │  │
│  └───────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│  Network Layer (src/net/net.c)              │
│  ┌───────────────────────────────────────┐  │
│  │ IPv4: routing table, fragmentation,   │  │
│  │       reassembly, ICMP, IGMP          │  │
│  │ IPv6: SLAAC, NDP, ICMPv6              │  │
│  │ ARP:  cache with timeout/retry,       │  │
│  │       pending resolution queue        │  │
│  │ Tunnels: IPIP, GRE, VXLAN, IPsec     │  │
│  │ Bonding: 802.3ad (LACP), balance-xor │  │
│  └───────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│  Netfilter (src/net/netfilter.c)            │
│  ┌───────────────────────────────────────┐  │
│  │ Five hook points (PREROUTING, LOCAL_IN,│  │
│  │ FORWARD, LOCAL_OUT, POSTROUTING)      │  │
│  │ nf_tables ruleset, conntrack, NAT,    │  │
│  │ SOCKS5 proxy                          │  │
│  └───────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│  Advanced Networking                        │
│  ┌───────────────────────────────────────┐  │
│  │ XDP (eXpress Data Path) — BPF-based  │  │
│  │   early packet processing before      │  │
│  │   entering the network stack          │  │
│  │ TIPC — Transparent Inter-Process     │  │
│  │   Communication (cluster messaging)   │  │
│  │ 6LoWPAN — IPv6 over Low-Power WPAN   │  │
│  │ MACsec — IEEE 802.1AE link security   │  │
│  └───────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│  Driver Layer (src/net/netdevice.c,         │
│               src/drivers/)                 │
│  ┌───────────────────────────────────────┐  │
│  │ NIC drivers → netdevice registration  │  │
│  │ e1000, virtio-net, loopback, TUN/TAP, │  │
│  │ veth, bonding, IPVS                   │  │
│  │ Multi-queue RSS, interrupt moderation │  │
│  │ RPS/RFS: receive steering by flow hash│  │
│  └───────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│  Link Layer                                 │
│  Ethernet framing, VLAN 802.1Q, bridging    │
│  (STP, IGMP snooping), LACP, LLDP, MACsec  │
└─────────────────────────────────────────────┘
```

### Driver Layer

**Files:** `src/net/netdevice.c`, `src/drivers/e1000.c`, `src/drivers/virtio_net.c`, `src/include/netdevice.h`

The netdevice layer provides a registration-based abstraction over physical and virtual NICs. Each NIC driver fills in a `struct net_device` with a name, MAC address, and transmit/receive callbacks, then calls `netif_register()` to make the interface available.

Supported NICs:
- **e1000** — Intel PRO/1000, QEMU `-device e1000` (MSI-X, multi-queue RSS, interrupt moderation)
- **virtio-net** — Paravirtualized virtio network device (multi-queue, indirect descs)
- **loopback** — Internal loopback interface
- **TUN/TAP** — Userspace packet injection (`src/net/tun.c`)
- **veth** — Virtual Ethernet pair for network namespaces (`src/net/veth.c`)
- **bonding** — Link aggregation (`src/drivers/bonding.c`): 802.3ad (LACP), balance-xor, active-backup, broadcast

Receive-side scaling: RPS (Receive Packet Steering) distributes packets across CPUs by flow hash (`src/net/rps.c`). The bridge (`src/net/bridge.c`) supports STP (Spanning Tree Protocol) and IGMP snooping for multicast filtering.

### Network Layer

**Files:** `src/net/net.c`, `src/net/ipv6.c`, `src/include/net_internal.h`

The IP layer handles:

- **IPv4 routing** — Static routing table with up to `RT_MAX_ENTRIES` entries. Forwarding via `net_ip_forwarding` (`/proc/sys/net/ipv4/ip_forward`).
- **IP fragmentation/reassembly** — Fragmentation of outgoing large packets, timeout-limited reassembly of incoming fragments.
- **ICMP** — Echo request/reply, destination unreachable, time exceeded, parameter problem.
- **IGMP** — Multicast group membership.
- **IPv6** — SLAAC via Router Advertisements, NDP, ICMPv6.
- **ARP** — 16-entry cache, 300s timeout, 3-retry probe, pending resolution queue (8 frames).
- **Bonding** — LACP 802.3ad, balance-xor by flow hash, active-backup failover, MII monitoring.
- **Tunnels:** IPIP (RFC 2003), GRE (RFC 2784), VXLAN (RFC 7348), IPsec (ESP/AH)

**XDP (eXpress Data Path)** (`src/net/xdp.c`): BPF-based early packet processing at the driver level, before SKB allocation. Supports XDP_DROP, XDP_PASS, XDP_TX actions. Zero-copy frame delivery to userspace via AF_XDP sockets.

**TIPC (Transparent Inter-Process Communication)** (`src/net/tipc.c`): Cluster-oriented messaging protocol with native service addressing, topology-aware routing, and bearer redundancy.

**L2TPv3** (`src/net/l2tp.c`): Layer 2 Tunneling Protocol v3 for carrying L2 frames over IP networks. Unmanaged tunnel mode with UDP or IP encapsulation.

**PPTP** (`src/net/pptp.c`): Point-to-Point Tunneling Protocol (RFC 2637) with GRE encapsulation and control channel messaging.

**6LoWPAN** (`src/net/sixlowpan.c`): IPv6 over IEEE 802.15.4 low-power wireless networks. Header compression, fragmentation/reassembly for small MTU links.

### Transport Layer

**TCP** (`src/net/net_tcp.c`):

Full state machine (11 states), 16-entry connection table, per-connection locking.

- **Congestion control:** Reno, CUBIC, BBRv1/v2, BIC, Vegas, Westwood, Hybla, Illinois
- **Loss detection:** RACK (Recent ACKnowledgment), dupACK fast retransmit, PRR (RFC 6937)
- **Features:** TFO (RFC 7413), SYN cookies, SACK, Nagle, delayed ACK, keepalive, MD5 signatures, window scaling, MPTCP (multi-path TCP)

**UDP** (`src/net/net_udp.c`): Connection table with up to `MAX_UDP_BINDINGS`, connected sockets for `send()`/`recv()`, broadcast/multicast, checksum verification.

**Other transport protocols:** SCTP (Stream Control Transmission Protocol), DCCP (Datagram Congestion Control Protocol), L2TP, PPTP.

### Socket Layer

**Files:** `src/net/socket.c`, `src/net/socket_ext.c`, `src/include/socket.h`

BSD socket API integrated with the file descriptor system. Socket table with 32 entries, fd mapping at offset 100.

Supported address families:
- **AF_INET** — IPv4 TCP/UDP
- **AF_INET6** — IPv6 (autoloads ipv6 module)
- **AF_UNIX** — Unix domain sockets (`src/net/af_unix.c`)
- **AF_PACKET** — Raw packet sockets (`src/net/af_packet.c`)
- **AF_NETLINK** — Kernel-userspace communication (`src/net/netlink.c`)
- **AF_CAN** — SocketCAN protocol (`src/net/can.c`)
- **AF_TIPC** — TIPC cluster messaging
- **AF_XDP** — Express Data Path sockets

Socket operations: `socket()`, `bind()`, `connect()`, `listen()`, `accept()`, `send()`, `recv()`, `sendto()`, `recvfrom()`, `poll()`/`select()`, `getsockopt()`/`setsockopt()`, `close()`.

### Netfilter

**Files:** `src/net/netfilter.c`, `src/net/nf_tables.c`, `src/net/conntrack.c`, `src/include/netfilter.h`

Linux-compatible packet filtering with five hook points (PREROUTING, LOCAL_IN, FORWARD, LOCAL_OUT, POSTROUTING). Priority-sorted handler chains.

**Packet filtering:** Up to 64 static rules matching on src/dst IP, port, protocol. nf_tables interface via `src/net/nf_tables.c`.

**Connection tracking** (`src/net/conntrack.c`): 256 concurrent connections, TCP/UDP/ICMP state machines, tuple-based lookup, protocol-specific timeouts, helper modules (FTP, SIP).

**NAT:** Up to 16 rules — SNAT (MASQUERADE) and DNAT (port forwarding). Works with conntrack.

**SOCKS5** (`src/net/socks5.c`): SOCKS5 proxy client with TCP-connect-based tunneling, username/password authentication, and remote DNS resolution.

## eBPF Subsystem

**Files:** `src/kernel/bpf_verifier.c`, `src/kernel/bpf_maps.c`, `src/kernel/bpf_progs.c`, `src/kernel/bpf_helpers.c`, `src/include/{bpf_verifier,bpf_maps,bpf_progs,bpf_helpers}.h`

A minimal but functional eBPF (extended Berkeley Packet Filter) subsystem supporting program verification, map storage, and helper functions.

```
BPF Program Lifecycle:
  load → verifier → JIT/map setup → attach → run → unload

Verification:
  → Control-flow graph construction
  → Dead code elimination
  → Register state tracking (type, bounds, nullness)
  → Stack depth validation
  → Store/load bounds checking
  → Helper call validation
```

**BPF Verifier** (`src/kernel/bpf_verifier.c`):
- Checks for unreachable instructions, out-of-bounds memory access, uninitialized register use
- Tracks same-value registers for safe comparison pruning
- Validates helper function IDs and argument types
- Limits instruction count, stack depth, and map access patterns
- Dead code elimination and path pruning for performance

**BPF Maps** (`src/kernel/bpf_maps.c`):
- **Array maps** — fixed-size, pre-allocated, indexed by key
- **Hash maps** — dynamic, key-value store with chaining
- **Per-CPU maps** — per-CPU variants of array and hash
- **Perf event maps** — BPF ring buffer for perf events
- Operations: `lookup`, `update`, `delete`, `get_next_key`

**BPF Programs** (`src/kernel/bpf_progs.c`):
- Program types: socket filter, XDP, kprobe, tracepoint, perf event
- Immediate interpreter (no JIT) that walks eBPF bytecodes
- Execute in a sandboxed environment with register file and stack frames
- Return values interpreted per program type

**BPF Helpers** (`src/kernel/bpf_helpers.c`):
- `bpf_map_lookup_elem`, `bpf_map_update_elem`, `bpf_map_delete_elem`
- `bpf_get_prandom_u32`, `bpf_trace_printk`, `bpf_ktime_get_ns`
- `bpf_get_current_pid_tgid`, `bpf_get_current_comm`
- `bpf_perf_event_output`, `bpf_skb_load_bytes`, `bpf_skb_store_bytes`
- `bpf_tail_call` for chaining programs

**BPF Syscall** (`src/kernel/bpf.c`): Userspace interface via `bpf()` syscall with commands: `BPF_PROG_LOAD`, `BPF_MAP_CREATE`, `BPF_MAP_LOOKUP_ELEM`, `BPF_MAP_UPDATE_ELEM`, `BPF_MAP_DELETE_ELEM`, `BPF_MAP_GET_NEXT_KEY`.

## Security Subsystem

### Kernel Hardening

The kernel implements multiple security mechanisms:

- **KASLR**: kernel base randomized at boot, module base randomized per load
- **SMAP/SMEP/UMIP**: supervisor access/execution/user-mode prevention
- **NX**: non-executable pages enforced on all data/stack/heap mappings
- **ASLR**: per-exec randomization of stack, heap, mmap, VDSO
- **KPTI**: Kernel Page Table Isolation with PCID/INVPCID
- **Seccomp-BPF**: syscall filtering via BPF programs (`src/kernel/seccomp.c`)
- **Landlock**: path-based Mandatory Access Control (stackable rules, `src/kernel/landlock.c`)
- **SMACK**: Simplified Mandatory Access Control for Kernel (`src/kernel/smack.c`)
- **YAMA**: ptrace scope restriction (0 = disabled, 4 = full lockdown)
- **CET Shadow Stack**: ROP mitigation via shadow return address stack
- **Stack Guard**: unmapped page below each kernel stack (overflow detection)
- **Stack Canary**: per-task stack canary values
- **Stackleak**: erases kernel stack on syscall exit
- **Slab Poisoning**: freed objects overwritten with poison values
- **Lockdep**: runtime lock ordering validation (deadlock detection)
- **KPAC/KRBS**: kernel pointer authentication (where hardware supports)

### SMACK LSM

**File:** `src/kernel/smack.c`

Simplified Mandatory Access Control for Kernel (SMACK) — a label-based LSM similar to Linux SMACK. Each subject (process) and object (file, IPC, socket) carries a SMACK label. Access is granted if the subject's label matches the object's label or if a rule explicitly permits the access.

- **Label assignment:** default label in `struct task_smack`, file labels from extended attributes
- **Rule table:** `smack_rule` entries with subject/object/access triples
- **Access types:** read, write, execute, append, transmute (label change on exec)
- **Integration:** file permission checks, IPC permission checks, socket security
- **Sysfs interface:** `/sys/fs/smackfs/` for rule management

### TPM 2.0

**Files:** `src/drivers/tpm_tis.c`, `src/kernel/tpm_attest.c`, `src/kernel/tpm_rng.c`

TPM 2.0 support via TIS (TPM Interface Specification) interface:

- **TPM driver** — TPM TIS 1.3 FIFO communication, locality management, burst count handling
- **Attestation** (`src/kernel/tpm_attest.c`) — TPM2_PCR_Read/Extend, TPM2_Quote, TPM2_GetRandom, TPM2_Create/ActivateCredential for remote attestation
- **RNG** (`src/kernel/tpm_rng.c`) — TPM 2.0 random number generator feeding kernel entropy pool
- **PCR measurements** — TPM PCR[0-15] extended with boot components, initramfs, kernel image
- **TPM2 Key** — RSA key generation and sealing for measured boot chain

### UEFI Secure Boot

**File:** `src/kernel/efi_secureboot.c`

- EFI Secure Boot variable parsing (`SetupMode`, `SecureBoot`, `PK`, `KEK`, `db`, `dbx`)
- Signature verification (PKCS#7, Authenticode)
- Kernel image signature validation at boot
- `mokutil`-compatible Machine Owner Key (MOK) management

### Integrity and Names paces

- **IMA (Integrity Measurement Architecture)** — measures file hashes before access, stores in PCRs
- **EVM (Extended Verification Module)** — verifies extended attribute integrity using HMAC
- **IPE (Integrity Policy Enforcement)** — enforces integrity policies on file execution
- **IPC Namespace** (`src/kernel/ipc_namespace.c`) — isolated System V IPC and POSIX message queues per namespace
- **Time Namespace** (`src/kernel/time_namespace.c`) — per-namespace CLOCK_MONOTONIC offset (for container migration of monotonic timers)

## Virtualization

### KVM

**File:** `src/kernel/kvm.c`, `src/include/kvm.h`

In-kernel virtual machine monitor supporting hardware-accelerated virtualization (Intel VMX, AMD SVM). The KVM module provides full virtualization capabilities:

```
KVM Architecture:

Userspace (QEMU/EMU):
  ┌────────────────────────────────────┐
  │  /dev/kvm interface                │
  │  KVM_CREATE_VM → KVM_CREATE_VCPU  │
  │  KVM_RUN → handle exits            │
  └────────────────────────────────────┘
              │ ioctl
              ▼
Kernel KVM Module:
  ┌────────────────────────────────────┐
  │  VMCS/VMCB management             │
  │  EPT/NPT nested page tables       │
  │  Exit handling (IO, MMIO, MSR,    │
  │    HLT, CPUID, CR access)         │
  │  Interrupt injection (APICv)      │
  │  vCPU scheduling on host CPUs     │
  └────────────────────────────────────┘
```

Features: VM lifecycle management, EPT/NPT for nested paging, MSR bitmaps for fast MSR access, APICv for virtual interrupt delivery, dirty page tracking for live migration, in-kernel I/O bus for fast MMIO emulation.

### vhost

- **vhost-scsi** (`src/drivers/vhost_scsi.c`): In-kernel SCSI target for virtio-scsi, offloading data plane from QEMU
- **vhost-blk** (`src/drivers/vhost_blk.c`): In-kernel block device backend for virtio-blk, zero-copy data transfer

### VFIO

**File:** `src/drivers/vfio.c`

VFIO (Virtual Function I/O) framework for userspace driver access to devices. Supports:
- Device groups and container-based isolation
- DMA remapping (IOMMU-backed)
- Interrupt remapping (MSI/MSI-X)
- Device region access (BAR, config space)
- PCI SR-IOV VF assignment

### virtio-fs / virtio-iommu / vDPA

- **virtio-fs** (`src/drivers/virtio_fs.c`): Shared filesystem between host and guest using FUSE-over-virtio. DAX for direct memory mapping, file system passthrough semantics.
- **virtio-iommu** (`src/drivers/virtio_iommu.c`): IOMMU paravirtualized via virtio. Page table management, device address space isolation.
- **vDPA** (`src/drivers/vdpa.c`): vDPA (virtio Data Path Acceleration) framework. Hardware offload of virtio data plane while retaining virtio control path.

### Balloon

**File:** `src/drivers/balloon.c`

Virtio memory balloon for dynamic guest memory management. Inflate/deflate via virtio requests, host notification on page release, stats reporting (free memory, total memory). Compaction-friendly (cooperative page release).

## Storage Subsystem

### iSCSI

**File:** `src/drivers/iscsi.c`, `src/include/iscsi.h`

iSCSI initiator for accessing remote block devices over IP networks:

- Full iSCSI session management (login, parameter negotiation, logout)
- CHAP authentication support
- Command queuing with immediate and unsolicited data
- Error recovery (connection reinstatement, task management functions)
- Multi-connection sessions (MC/S) for load balancing

### NVMe over Fabrics (NVMe-oF)

**File:** `src/drivers/nvmf.c`, `src/include/nvmf.h`

NVMe-oF target implementation:

- RDMA (InfiniBand/RoCE) and TCP transport binding
- Queue pair management, controller ID assignment
- Namespace export, PRP/SGL data placement
- Discovery service integration

### FCoE

**File:** `src/drivers/fcoe.c`, `src/include/fcoe.h`

Fibre Channel over Ethernet (FCoE) initiator:

- FCoE frame encapsulation (FC over Ethernet)
- FIP (FCoE Initialization Protocol) for VLAN discovery and login
- FC-2 layer: sequences, exchanges, frame multiplexing
- Virtual Fabric support

### DRBD

**File:** `src/drivers/drbd.c`, `src/include/drbd.h`

Distributed Replicated Block Device (DRBD) for block-level replication:

- Synchronous, asynchronous, and semi-synchronous replication modes
- Primary/Secondary and Dual-Primary roles
- Automatic failover with quorum-based decision
- Online verify and resync
- Three-way replication support

### Ceph/RBD

**File:** `src/drivers/rbd.c`, `src/include/rbd.h`

Ceph RADOS Block Device (RBD) client:

- librados protocol for OSD communication
- CRUSH map-based placement
- Snapshot and clone support
- Layered image support (differential read)
- Object striping across OSDs

### Device Mapper

**File:** `src/drivers/dm-era.c`, `src/include/dm-era.h`

Device mapper ERA target for thin provisioning:

- Tracks changed blocks since a given era
- Era-based snapshots and rollback
- Integration with device-mapper framework

### NVMe Multipath

Multipath I/O for NVMe with ANA (Asymmetric Namespace Access) support: path selection by I/O policy (round-robin, least-queued, latency-based), failover on transport errors, persistent discovery log controller addressing.

## Performance & Observability

### Performance Monitoring Subsystem

- **Perf events** (`src/kernel/perf_event.c`): Hardware PMU counters, software events (context switches, page faults, migrations), tracepoints. Ring-buffer output with mmap integration.
- **ftrace** (`src/kernel/ftrace.c`): Function tracer with dynamic patching, stack trace capture, and event triggers. Supports function_graph tracer, event filtering, and trace_marker.
- **Kprobes** (`src/kernel/kprobes.c`): Dynamic breakpoint insertion at any instruction address. Pre/post handler execution, fault handling. Support for jprobes and kretprobes (return value capture).
- **Uprobes** (`src/kernel/uprobes.c`): Userspace dynamic instrumentation. Breakpoint injection at userspace addresses, handler callbacks in kernel context, single-stepping with XOL (exec out of line) area.

### RAS (Reliability, Availability, Serviceability)

**Files:** `src/kernel/ras_netlink.c`, `src/kernel/edac.c`

- **RAS netlink** (`src/kernel/ras_netlink.c`): Userspace notification of hardware errors (corrected ECC, uncorrected memory errors, PCIe AER). Netlink multicast to registered listeners.
- **EDAC** (`src/drivers/edac.c`): Memory error detection and correction reporting. ECC syndrome decoding for DRAM rank/bank/row diagnosis.
- **hung_task** (`src/kernel/hung_task.c`): Detects tasks stuck in D-state past configurable timeout. Logs backtrace, triggers panic or sysrq on repeated violations.
- **UBSan** (`src/kernel/ubsan.c`): Undefined Behavior Sanitizer runtime. Traps signed integer overflow, shift-out-of-bounds, null pointer arithmetic, and type mismatch violations.
- **Hung task detector** — periodically checks for tasks stuck in uninterruptible sleep (`/proc/sys/kernel/hung_task_timeout_secs`).

## Debugging Infrastructure

**KDB** (`src/drivers/kdb.c`): In-kernel debugger accessible via serial console or keyboard (SysRq). Commands: backtrace (`bt`), memory dump (`md`), register dump (`rd`), breakpoint management (`bp`), step execution (`ss`), process list (`ps`), module list, symbol lookup. Integration with KGDB for remote GDB debugging.

**Debug memory tools:**
- **KASAN-light** — memory corruption detection (use-after-free, out-of-bounds) via shadow memory
- **KFENCE** — low-overhead use-after-free detection with object quarantine
- **KCSAN** — data race detection with watchpoints
- **KMSAN** — uninitialized memory detection
- **Kmemleak** — kernel memory leak detection with periodic scanning
- **Lockdep** — runtime lock ordering validation

## eBPF Tracing

BPF programs can attach to kprobes (`SEC("kprobe/sys_*")`), tracepoints, and perf events for dynamic tracing. The bpf helper `bpf_trace_printk()` writes to the kernel trace log. Combined with perf event maps, BPF programs produce structured tracing data consumers can read via the BPF syscall.

## Kernel Modules

**Files:** `src/kernel/module.c`, `src/kernel/module_elf.c`, `src/kernel/module_deps.c`, `src/kernel/module_alias.c`, `src/kernel/module_compress.c`, `src/kernel/module_signature.c`, `src/kernel/module_async.c`, `src/kernel/module_autoload.c`

The kernel supports loadable modules as standalone `.ko` files. 226 modules compile from `obj-m` entries in the Makefile.

```
Module Loading:
  insmod → syscall → module loader → ELF relocation → memory allocation
          ↓                                                    ↓
       Read .ko file                                   Module in RX/RO/RW
       Parse ELF headers                               64MB region
       Verify signature (RSA)                          Symtab registration
       Resolve symbols (EXPORT_SYMBOL)                 module_init() call
       Apply relocations                               
```

**Module infrastructure:**
- `.ko` ELF loader — full RELA relocation for x86-64, GOT/PLT handling
- Module state machine: LOADING → LIVE → UNLOADING → DEAD
- 64MB module region (0xFFFF800100000000) with per-section page permissions (RX/RO/RW)
- EXPORT_SYMBOL system — 50+ symbols exported across kernel subsystems
- Module signatures (RSA-2048/SHA-256) — verified before loading
- Module dependency tracking (`src/kernel/module_deps.c`)
- Module aliasing for `request_module()` autoload
- Module compression (xz/gzip) — decompressed on load
- Module autoload via `request_module()` on device discovery

**User commands:** `insmod.elf`, `modprobe.elf`, `rmmod.elf`, `lsmod.elf`, `modinfo.elf` (all in `/userspace/`)
**Initramfs integration:** 226 modules bundled in initramfs at `/modules/`, auto-loaded on boot via `/etc/modules` list

**Build:** `make modules` produces all module `.ko` files; `make` builds both kernel and modules together if `obj-m` is non-empty.

## Cluster Architecture

**The cluster subsystem has been moved to userspace.** See `userspace/clusterd/` and `docs/cluster-architecture.md`.

Architecture:

```
┌──────────────────────────────────────────────┐
│              Userspace Clusterd              │
│  ┌──────────┐  ┌──────────┐  ┌────────────┐ │
│  │   Raft   │  │  Gossip  │  │   REST     │ │
│  │ Consensus│  │ Protocol │  │    API     │ │
│  └────┬─────┘  └──────────┘  └─────┬──────┘ │
│       │             │               │        │
│  ┌────▼─────────────▼───────────────▼──────┐ │
│  │         Netlink Bridge                  │ │
│  └────────────────┬────────────────────────┘ │
└───────────────────┼──────────────────────────┘
                    │ netlink
                    ▼
┌──────────────────────────────────────────────┐
│           Kernel (minimal cluster hooks)      │
│  ┌──────────┐  ┌──────────┐  ┌────────────┐  │
│  │  IPVS    │  │Conntrack │  │ Network    │  │
│  │  LB      │  │   NAT    │  │  Policies  │  │
│  └──────────┘  └──────────┘  └────────────┘  │
└──────────────────────────────────────────────┘
```

- **clusterd** — standalone userspace daemon (`userspace/clusterd/clusterd.c`) implementing Raft consensus, SWIM-style gossip membership, and a REST API for cluster management
- **Netlink bridge** — clusterd communicates with kernel via `AF_NETLINK` for IPVS, conntrack, and network policy management
- **Raft:** leader election (150-300ms randomized timeouts), log replication, key-value store
- **Gossip:** suspicion-based failure detection, infection-style state dissemination
- **orchctl** — CLI tool for cluster management (list nodes, manage pods, inspect services)

## Shell Subsystem

**Files:** `src/shell/`, `cmd_table.inc`, `cmds/`

The built-in shell features 356+ commands with scripting support, job control, and command completion.

**Builtins added in recent batches:**
- **eval** — constructs and executes commands from arguments
- **read** — reads a line from stdin into shell variables (with `-p` prompt, `-t` timeout, `-n` char limit, `-r` raw mode, `-d` delimiter, `-a` array mode)
- **type** — displays command type (builtin, alias, function, executable path)
- **dirs/pushd/popd** — directory stack management
- **Arrays** — indexed shell arrays (`arr=(a b c)`, `echo ${arr[1]}`, `${#arr[@]}`)

**Shell features:** variable expansion (`${var:-default}`, `${var:+alt}`, `${#var}`), command substitution, pipeline with 64KB double-buffered I/O, tab-completion, persistent history, and a command table (`cmd_table.inc`) automatically generated from command registration.

## Filesystem Stack Architecture

The filesystem stack implements a Linux-compatible VFS layer with 30+ on-disk, in-memory, network, and pseudo-filesystems.

```
System Calls (open/read/write/close/stat/mount/umount)
     ↕
┌─────────────────────────────────────────────┐
│  VFS Layer                                  │
│  vnode operations, path resolution,         │
│  dentry cache (LRU, shrink under OOM),      │
│  file locks, inotify/fanotify, xattr, ACLs  │
├─────────────────────────────────────────────┤
│  Mount System                               │
│  Mount table (global + per-namespace),      │
│  propagation types, bind mounts,            │
│  mount namespace (CLONE_NEWNS)              │
├─────────────────────────────────────────────┤
│  30+ Filesystems                            │
│  Disk: FAT32, ext2, ext4, btrfs, NTFS,     │
│        exFAT, HFS+, ReiserFS, iso9660,      │
│        squashfs, cramfs, f2fs, erofs,       │
│        jffs2, nilfs2, minix, ufs, sysv, hfs │
│  Network: CIFS, NFS (client), NFSd (server) │
│  In-memory: tmpfs, ramfs, tarfs, cpio, romfs│
│  Pseudo: procfs, sysfs, devfs, debugfs,     │
│          overlay, FUSE                      │
├─────────────────────────────────────────────┤
│  Block Cache & Buffer Cache                 │
│  Page cache (LRU, dirty writeback, readahead)│
│  Buffer cache (64-entry LRU, sector-level)   │
│  Block I/O scheduler (deadline, CFQ)        │
├─────────────────────────────────────────────┤
│  Block Device Layer                         │
│  ATA, AHCI (NCQ), NVMe (multipath, PMR),    │
│  virtio-blk, iSCSI, NVMe-oF, FCoE, DRBD,   │
│  Ceph/RBD, DM-era, loop, nbd, ramdisk,      │
│  MD RAID, device-mapper                     │
└─────────────────────────────────────────────┘
```

## Production Hardening

The codebase underwent systematic production-readiness hardening:

### Build System Hardening
- `-Werror` in default CFLAGS — zero compiler warnings
- `-fstack-clash-protection` — stack clash attack mitigation
- `-z relro -z now` (LDFLAGS) — full RELRO, GOT read-only after relocation
- `-fstack-protector-strong` — stack canary for all functions with local buffers
- `-fstackleak` — kernel stack erasure on syscall exit (information leak prevention)
- Static analysis via `cppcheck` target

### Memory Safety
- `kmalloc_array`/`kcalloc_array`/`krealloc_array` wrappers — overflow-checked allocation for all multiply-based sizes
- All integer-overflow sites in kmalloc fixed (30+ locations)
- 31 unsafe strcpy/strcat calls replaced with strncpy/strncat across 14 files
- Stack canary per-task for kernel stack overflow detection

### Error Handling
- 60+ `return -1` sites replaced with proper negative errno values
- All implicit function declarations fixed (12 files, ~15 sites)
- 88 compiler warnings eliminated to zero
- OOM paths return NULL instead of panicking

### Locking Correctness
- `rwlock.h` IRQ state corruption in contention path fixed
- `rwsem.c` lockdep ordering bug (lock_acquire before CAS) fixed
- `io_uring.c` integer overflow in kmalloc for iovec count fixed
- `fs/fs.c` silent write failure — `(void)ata_write_sectors()` captured and logged
- `netconsole.c` — `spinlock_irqsave_acquire` in IRQ context for kprintf hook
- `rcu.c` — added `rcu_gp_lock` spinlock for grace period state race

## Testing Infrastructure

The kernel has three tiers of testing:

1. **In-kernel tests** (`src/test/test.c`): 200+ tests running in QEMU, reporting via serial. Cover: scheduler (fork/exec/exit/wait), VM (map/unmap/COW/shared mappings), PMM (alloc/free/refcount), slab (alloc/free/alignment), IPC (pipe/shm/semaphore/mqueue), VFS (open/read/write/seek/close), TCP/UDP socket API, device probing, syscall interface.

2. **KUnit** (`src/test/kunit_tests.c`): In-kernel unit testing framework with test case, test suite, and assertion API. Designed for subsystem-level tests without booting full QEMU.

3. **UBSan tests**: Kernel boots with `CONFIG_UBSAN` for automatic undefined behavior detection.

4. **Host-side tests** (`tests/host_libc/`): kernel libc functions compiled and run on Linux host against glibc baseline. Covers string, printf, stdlib, bitops, CRC, SHA-256, AES.

5. **E2E QEMU smoke test** (`tests/e2e.sh`): boots the kernel, interacts via serial console, validates boot sequence, shell commands, networking (DHCP, TCP), and filesystem operations.

## Performance Considerations

- **Cache locality**: per-CPU data structures (page/slab caches, runqueues) minimize cross-CPU traffic
- **Lock contention**: MCS optimistic spinning, RCU for read-mostly data, per-CPU locking
- **Memory bandwidth**: huge pages (2MB/1GB) for kernel and userspace, reducing TLB misses
- **Interrupt mitigation**: MSI/MSI-X per-queue, interrupt moderation (e1000 ITR), softirq coalescing, NAPI polling
- **I/O efficiency**: I/O schedulers (deadline/CFQ), block layer merging, readahead, page cache, io_uring async I/O
- **Network performance**: RPS/RFS flow steering, XDP fast path, multi-queue RSS

## Build System

The kernel is built with a GCC/ccache cross toolchain (`x86_64-elf-gcc`). The Makefile supports multiple targets:

```
make              — debug build (no optimization, full assertions) + modules
make release      — optimized build (-O2, stripped)
make build-strict — -Werror + cppcheck static analysis
make modules      — build all kernel modules (.ko files)
make run          — build + launch QEMU
make debug        — build + launch QEMU with GDB stub (-s -S)
make clean        — remove build artifacts
```

The linker script (`linker.ld`) defines the memory layout with proper section ordering, alignment, and high-half VMA assignment.

**Module build:** 226 `.ko` files produced from `obj-m` entries. Modules are compiled with `-DMODULE` flag and linked as relocatable ELF64 objects. Module region at `0xFFFF800100000000` (64MB) is divided into RX/RO/RW subregions for code, read-only data, and writable data respectively.
