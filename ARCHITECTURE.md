# OS Kernel Architecture

## Overview

This OS is an x86-64 long-mode hobby kernel booting via Multiboot1 (GRUB/QEMU). It implements a monolithic kernel architecture with a high-half virtual memory layout. All kernel code and data resides at virtual addresses `0xFFFF800000000000+` while the lower 512 GB of virtual address space is available per-process for userspace.

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
      0xFFFF8000200000 - ...               : Kernel heap (kmalloc)
      ...                                   : Kernel stacks + guard pages
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
11. Initialization order (linear, ~29 steps):
    1. `gdt_init()` — final GDT with TSS, IST entries
    2. `idt_init()` — IDT with interrupt gates and IST assignments
    3. `pic_remap()` — remap PIC IRQs to vectors 0x20-0x2F
    4. `pmm_init()` — detect physical memory from Multiboot, build bitmap
    5. `vmm_init()` — finalize page tables, enable NX, PAT
    6. `heap_init()` — set up kmalloc arena
    7. `slab_init()` — initialize kmem_cache subsystem
    8. `smp_init()` — detect APs via ACPI MADT, send INIT-SIPI-SIPI
    9. `process_init()` — create idle process, process table
    10. `timer_init()` — program PIT/HPET/TSC deadline timer (100 Hz)
    11. `keyboard_init()` — PS/2 keyboard interrupt handler
    12. `serial_init()` — COM1/COM2 serial console
    13. `pci_init()` — enumerate PCI bus, discover devices
    14. `ahci_init()`, `ata_init()` — storage device detection
    15. `fs_init()` — mount root filesystem (tmpfs or disk)
    16. `net_init()` — initialize networking stack + NIC
    17. `acpi_init()` — parse ACPI tables, battery, thermal
    18. `syscall_init()` — set up MSR_LSTAR syscall entry
    19. `shell_init()` — launch shell on /dev/console
    20. `asm("sti")` — enable interrupts → idle loop

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

Supports: copy-on-write fork (via `vmm_clone_pml4`), NX/SMEP enforcement, MAP_POPULATE, MMIO mapping for device drivers, demand paging with lazy allocation.

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

Multi-class scheduler with CFS (Completely Fair Scheduler) for normal tasks and prioritized scheduling for RT tasks.

```
Scheduling classes:
  SCHED_DEADLINE  → EDF, budget replenishment (GRUB)
  SCHED_FIFO      → run until blocked, priority-sorted
  SCHED_RR        → round-robin with RT timeslice
  SCHED_OTHER     → CFS vruntime scheduling
  SCHED_IDLE      → lowest priority, background only

Per-CPU runqueue:
  → cfs_rq: red-black tree ordered by vruntime
  → rt_rq: priority-bitmap + linked lists
  → deadline_rq: rb_root ordered by deadline

Scheduler decisions:
  pick_next_task() → highest priority class → pick within class
  Context switch via switch.asm (save/restore registers, CR3)
```

Features:
- PELT (Per-Entity Load Tracking): running average of CPU utilization
- NUMA-aware task placement with automatic migration
- Load balancing across CPUs (periodic pull, idle push)
- Core scheduling (hyperthread safety)
- CPU hotplug support with task migration
- NO_HZ_FULL: adaptive tick on isolated CPUs

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
- High-resolution timers (hrtimer): O(1) red-black tree
- Timerfd: userspace timer via file descriptor

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

Mounted filesystems are tracked in a mount table with propagation types (SHARED/SLAVE/PRIVATE) for namespace support. The VFS supports: file locks (POSIX advisory), inotify/fanotify, xattr, O_NONBLOCK, O_DIRECT, fallocate, and splice.

### Supported Filesystems

| FS | Read/Write | Features |
|----|-----------|----------|
| **tmpfs** | R/W | Dynamic sizing, symlinks, device nodes, O_TMPFILE |
| **ext2** | R/W | Sparse files, large files, symlinks, fast symlinks, HTree indexes |
| **fat32** | R/W | Long filename (LFN) read + write, volume labels, FAT12/16/32 |
| **iso9660** | RO | Rock Ridge (POSIX), Joliet (Unicode), multi-session |
| **procfs** | R/O | /proc/{uptime,meminfo,cpuinfo,stat,self,interrupts,...} |
| **sysfs** | R/O | kobject tree, kernel parameters, device hierarchy |
| **devfs** | R/W | Dynamic device node creation, hotplug |
| **tarfs** | RO | Embedded initramfs, TAR archive mount |
| **cpio** | RO | cpio archive mount |
| **romfs** | RO | Simple read-only filesystem |
| **overlay** | R/W | Union mount (upper + lower dirs, whiteouts) |

### Networking Stack

The network stack is a full in-kernel TCP/IP implementation with socket API:

```
Application Layer:
  HTTPd, Telnetd, SSHd, DHCP, DNS, FTP, WireGuard

Transport Layer (socket API):
  TCP — full state machine: CLOSED → SYN-SENT → ESTABLISHED → ...
         Sliding window, congestion control (Reno, CUBIC, BBR)
         RACK loss detection, TFO, SYN cookies
  UDP — connected sockets, multicast, broadcast, checksums

Network Layer:
  IP routing table, fragmentation/reassembly, ICMP, IGMP
  IPIP/GRE tunnels, VXLAN, IPVS, netfilter (conntrack, NAT)

Link Layer:
  ARP cache (with timeout/retry), VLAN 802.1Q, bridge (STP)
  RPS/RFS: receive packet steering by flow hash
  
Device Layer:
  e1000, virtio-net, loopback, TUN/TAP
  multi-queue RSS, interrupt moderation, LRO
```

Socket operations: `socket()`, `bind()`, `connect()`, `listen()`, `accept()`, `send()`, `recv()`, `poll()`/`select()`, `getsockopt()`/`setsockopt()`.

### IPC & Synchronization

| Mechanism | File | Description |
|-----------|------|-------------|
| Pipe | `src/ipc/pipe.c` | Unidirectional data stream (O_NONBLOCK, poll) |
| FIFO (named pipe) | `src/ipc/fifo.c` | Named pipe via VFS |
| Shared Memory | `src/ipc/shm.c` | POSIX shared memory regions |
| Mutex | `src/ipc/mutex.c` | Priority inheritance, MCS optimistic spinning |
| Semaphore | `src/ipc/semaphore.c` | Counting semaphore |
| POSIX Semaphore | `src/ipc/posix_sem.c` | Named/unnamed POSIX semaphores |
| RWSEM | `src/kernel/rwsem.c` | Reader/writer semaphore |
| Eventfd | `src/ipc/eventfd.c` | Event notification FD (counting, semaphore mode) |
| Signalfd | `src/ipc/signalfd.c` | Signal delivery via FD |
| Timerfd | `src/ipc/timerfd.c` | Timer expiry via FD |
| Message Queue | `src/ipc/mqueue.c` | POSIX message queues |
| Completion | `src/include/completion.h` | Lightweight one-shot sync |
| Waitqueue | `src/ipc/waitqueue.c` | Blocked task list for condition waiting |

### Device Driver Model

Drivers are organized by bus type and follow a probe/remove lifecycle:

```
PCI bus (pci.c):
  → Enumerate bus 0 recursively → discover devices
  → Read config space → match vendor/device ID
  → Call driver probe → initialize device
  → Allocate MSI/MSI-X vectors → register IRQ handler

ACPI bus (acpi.c):
  → Parse RSDP → RSDT/XSDT → table headers
  → MADT: APIC topology, interrupt overrides
  → DSDT/SSDT: device hierarchy, power management
  → FADT: PM registers, wakeup, ACPI mode

Each driver implements:
  probe(device)   — detect, initialize, register IRQ
  remove(device)  — shutdown, free resources, unregister
  suspend/restore — power management
```

### Interrupt Flow (Example: e1000 NIC)

```
1. e1000 TX writes to TX descriptor ring
2. Device asserts INTx/MSI/MSI-X → I/O APIC → Local APIC
3. CPU receives interrupt → saves context → calls idt_entry()
4. idt_entry() → irq_handler() → e1000_isr()
5. e1000_isr(): reads ICR (cause), processes TX completions, frees buffers
6. netif_rx(): delivers packet to upper layers (TCP/IP stack)
7. Return from interrupt: restore context, iretq
```

## Kernel Hardening

The kernel implements multiple security mechanisms:

- **KASLR**: kernel base randomized at boot, module base randomized per load
- **SMAP/SMEP/UMIP**: supervisor access/execution/user-mode prevention
- **NX**: non-executable pages enforced on all data/stack/heap mappings
- **ASLR**: per-exec randomization of stack, heap, mmap, VDSO
- **Seccomp-BPF**: syscall filtering via Berkeley Packet Filter programs
- **Landlock**: path-based Mandatory Access Control (stackable rules)
- **YAMA**: ptrace scope restriction (0 = disabled, 4 = full lockdown)
- **CET Shadow Stack**: ROP mitigation via shadow return address stack
- **Stack Guard**: unmapped page below each kernel stack (overflow detection)
- **Slab Poisoning**: freed objects overwritten with poison values
- **Lockdep**: runtime lock ordering validation (deadlock detection)
- **KPAC/KRBS**: kernel pointer authentication (where hardware supports)

## Testing Infrastructure

The kernel has three tiers of testing:

1. **In-kernel tests** (`src/test/test.c`): 200+ tests running in QEMU, reporting via serial. Cover: scheduler (fork/exec/exit/wait), VM (map/unmap/COW/shared mappings), PMM (alloc/free/refcount), slab (alloc/free/alignment), IPC (pipe/shm/semaphore/mqueue), VFS (open/read/write/seek/close), TCP/UDP socket API, device probing, syscall interface.

2. **Host-side tests** (`tests/host_libc/`): kernel libc functions compiled and run on Linux host against glibc baseline. Covers string, printf, stdlib, bitops, CRC, SHA-256, AES.

3. **E2E QEMU smoke test** (`tests/e2e.sh`): boots the kernel, interacts via serial console, validates boot sequence, shell commands, networking (DHCP, TCP), and filesystem operations.

## Performance Considerations

- **Cache locality**: per-CPU data structures (page/slab caches, runqueues) minimize cross-CPU traffic
- **Lock contention**: MCS optimistic spinning, RCU for read-mostly data, per-CPU locking
- **Memory bandwidth**: huge pages (2MB/1GB) for kernel and userspace, reducing TLB misses
- **Interrupt mitigation**: MSI/MSI-X per-queue, interrupt moderation (e1000 ITR), softirq coalescing
- **I/O efficiency**: I/O schedulers (deadline/CFQ), block layer merging, readahead, page cache

## Build System

The kernel is built with a GCC/ccache cross toolchain (`x86_64-elf-gcc`). The Makefile supports multiple targets:

```
make              — debug build (no optimization, full assertions)
make release      — optimized build (-O2, stripped)
make build-strict — -Werror + cppcheck static analysis
make run          — build + launch QEMU
make debug        — build + launch QEMU with GDB stub (-s -S)
make clean        — remove build artifacts
```

The linker script (`linker.ld`) defines the memory layout with proper section ordering, alignment, and high-half VMA assignment.

## Network Stack Architecture

The network stack is a layered in-kernel TCP/IP implementation with a BSD socket API, netfilter packet filtering, and support for multiple NIC drivers. It spans `src/net/`, `src/drivers/` (NIC drivers), and `src/include/` (net headers).

```
Application Layer
     ↕  sys_socket/sys_send/sys_recv/etc.
┌─────────────────────────────────────────────┐
│  Socket Layer  (src/net/socket.c)           │
│  File descriptor integration, protocol      │
│  dispatch (AF_INET, AF_UNIX, AF_PACKET,     │
│  AF_NETLINK, AF_CAN)                        │
├─────────────────────────────────────────────┤
│  Transport Layer                            │
│  ┌───────────────────────────────────────┐  │
│  │ TCP (src/net/net_tcp.c)               │  │
│  │   Full state machine, congestion      │  │
│  │   control (Reno, CUBIC, BBR), RACK    │  │
│  │   loss detection, TFO, SYN cookies    │  │
│  └───────────────────────────────────────┘  │
│  ┌───────────────────────────────────────┐  │
│  │ UDP (src/net/net_udp.c)               │  │
│  │   Connected sockets, multicast,       │  │
│  │   broadcast, checksums                │  │
│  └───────────────────────────────────────┘  │
│  ┌───────────────────────────────────────┐  │
│  │ SCTP / DCCP / MPTCP                  │  │
│  └───────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│  Network Layer (src/net/net.c)              │
│  ┌───────────────────────────────────────┐  │
│  │ IPv4: routing table, fragmentation,   │  │
│  │       reassembly, ICMP, IGMP          │  │
│  │ IPv6: SLAAC, NDP, ICMPv6              │  │
│  │ ARP:  cache with timeout/retry,       │  │
│  │       pending resolution queue        │  │
│  │ Tunnels: IPIP, GRE, VXLAN             │  │
│  └───────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│  Netfilter (src/net/netfilter.c)            │
│  ┌───────────────────────────────────────┐  │
│  │ Five hook points (PREROUTING, LOCAL_IN,│  │
│  │ FORWARD, LOCAL_OUT, POSTROUTING)      │  │
│  │ Packet filtering rules (ip_tables)    │  │
│  │ Connection tracking (conntrack)       │  │
│  │ NAT (MASQUERADE, DNAT, SNAT)         │  │
│  └───────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│  Driver Layer (src/net/netdevice.c,         │
│               src/drivers/e1000.c, ...)     │
│  ┌───────────────────────────────────────┐  │
│  │ NIC drivers → netdevice registration  │  │
│  │ e1000, virtio-net, loopback, TUN/TAP  │  │
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

```c
struct net_device {
    char          name[NETDEV_NAME_MAX];  /* e.g. "eth0", "virtio0" */
    uint8_t       mac[6];                /* MAC address */
    int           ifindex;               /* assigned by netif_register */
    netdev_tx_fn  transmit;              /* send an Ethernet frame */
    netdev_rx_fn  receive;               /* poll for received frame */
    int           mtu;                   /* maximum transmission unit */
    int           flags;                 /* IFF_UP, etc. */
    void         *priv;                  /* driver-private data */
};
```

Key operations: `netif_register`, `netif_unregister`, `netif_send`, `netif_recv`, `netif_get`. The core networking code (`net_link_send`/`net_link_recv` in `net.c`) prefers the netdevice layer if interfaces are registered, falling back to direct driver calls for legacy compatibility.

Supported NICs:
- **e1000** — Intel PRO/1000, QEMU `-device e1000` (MSI-X, multi-queue RSS, interrupt moderation)
- **virtio-net** — Paravirtualized virtio network device
- **loopback** — Internal loopback interface
- **TUN/TAP** — Userspace packet injection (`src/net/tun.c`)
- **veth** — Virtual Ethernet pair for network namespaces (`src/net/veth.c`)

Receive-side scaling: RPS (Receive Packet Steering) distributes packets across CPUs by flow hash (`src/net/rps.c`). The bridge (`src/net/bridge.c`) supports STP (Spanning Tree Protocol) and IGMP snooping for multicast filtering.

### Network Layer

**Files:** `src/net/net.c`, `src/net/ipv6.c`, `src/include/net_internal.h`

The IP layer is the heart of `net.c`, handling:

- **IPv4 routing** — A static routing table (`struct rt_entry rt_table[]`) with up to `RT_MAX_ENTRIES` entries. Each entry specifies destination network, gateway, netmask, and interface index. Forwarding is controlled by `net_ip_forwarding` (`/proc/sys/net/ipv4/ip_forward`).
- **IP fragmentation/reassembly** — Outgoing large packets are fragmented; incoming fragments are reassembled in a timeout-limited buffer.
- **ICMP** — Echo request/reply, destination unreachable, time exceeded, parameter problem.
- **IGMP** — Multicast group membership for IP multicast (`src/net/igmp.c`).
- **IPv6** — Separate source (`src/net/ipv6.c`) with SLAAC (Stateless Address Autoconfiguration) via Router Advertisements, NDP (Neighbor Discovery Protocol), and ICMPv6.

**ARP** (`src/include/net_internal.h`):
- Cache with 16 entries, 300-second timeout, automatic probe retry (3 attempts at 1-second intervals).
- Pending resolution queue: up to 8 Ethernet frames buffered while waiting for MAC resolution.
- ARP announcement on link up, gratuitous ARP for address changes.

**Tunnels:**
- IPIP (`src/net/ipip.c`) — IP-in-IP encapsulation (RFC 2003)
- GRE (`src/net/gre.c`) — Generic Routing Encapsulation (RFC 2784)
- VXLAN (`src/net/vxlan.c`) — Virtual eXtensible LAN (RFC 7348)

### Transport Layer

**TCP** (`src/net/net_tcp.c`):

Full state machine with 11 states:

```
CLOSED → LISTEN → SYN_SENT → SYN_RECEIVED → ESTABLISHED
                                                ↓
                    FIN_WAIT → FIN_WAIT_2 → CLOSING → TIME_WAIT → CLOSED
                    CLOSE_WAIT → LAST_ACK
```

- **Connection table:** `struct tcp_conn tcp_conns[MAX_TCP_CONNS]` (16 entries), protected by `tcp_lock`.
- **Congestion control:** Reno (AIMD), CUBIC (RFC 8312), BBR (BBRv1/v2, `src/net/tcp_bbr.c` + `src/net/tcp_bbr2.c`), and legacy variants (BIC, Vegas, Westwood, Hybla, Illinois).
- **Loss detection:** RACK (Recent ACKnowledgment) for faster loss recovery, plus classic dupACK-based fast retransmit. Proportional Rate Reduction (PRR, RFC 6937) controls data sending during recovery.
- **Features:** TCP Fast Open (TFO, RFC 7413), SYN cookies for SYN flood defense, selective ACK (SACK), Nagle's algorithm, delayed ACK, keepalive probes, TCP MD5 signatures, window scaling.
- **Listeners:** `struct tcp_listener net_listeners[]` supports callback-based and accept-queue-based models.

**UDP** (`src/net/net_udp.c`):
- Connection table: `struct udp_binding net_udp_bindings[]` with up to `MAX_UDP_BINDINGS`.
- Connected sockets (`connect()` on UDP) track remote address for `send()`/`recv()`.
- Broadcast and multicast delivery.
- UDP checksum verification on receive, optional checksum generation on transmit.

**Other transport protocols:**
- SCTP (`src/net/sctp.c`), DCCP (`src/net/dccp.c`), MPTCP — multi-path TCP (`src/net/mptcp.c`)

### Socket Layer

**Files:** `src/net/socket.c`, `src/net/socket_ext.c`, `src/include/socket.h`

The socket layer implements the BSD socket API and integrates with the file descriptor system:

```
Socket table: struct socket socket_table[SOCK_MAX] (max 32)

Socket → fd mapping: fd = slot + 100 (offset from regular file descriptors)
```

Supported domain/protocol families:
- **AF_INET** — IPv4 TCP/UDP (dispatches to `net_tcp.c`/`net_udp.c`)
- **AF_INET6** — IPv6 (autoloads ipv6 module via `request_module`)
- **AF_UNIX** — Unix domain sockets (`src/net/af_unix.c`)
- **AF_PACKET** — Raw packet sockets (`src/net/af_packet.c`)
- **AF_NETLINK** — Kernel-userspace communication (`src/net/netlink.c`)
- **AF_CAN** — SocketCAN protocol (`src/net/can.c`)

Socket operations: `socket()`, `bind()`, `connect()`, `listen()`, `accept()`, `send()`, `recv()`, `sendto()`, `recvfrom()`, `poll()`/`select()`, `getsockopt()`/`setsockopt()`, `close()`. Socket states: CREATED, BOUND, LISTENING, CONNECTING, CONNECTED, CLOSING, CLOSED.

The socket layer also supports `/proc/net/tcp`, `/proc/net/udp`, and `/proc/net/sockstat` via procfs integration.

### Netfilter

**Files:** `src/net/netfilter.c`, `src/net/nf_tables.c`, `src/net/conntrack.c`, `src/include/netfilter.h`

A Linux-compatible packet filtering framework with five hook points:

| Hook | Point | Direction |
|------|-------|-----------|
| `NF_INET_PRE_ROUTING` | Right after packet reception | Incoming |
| `NF_INET_LOCAL_IN` | Before delivery to local sockets | Incoming |
| `NF_INET_FORWARD` | Before forwarding to another interface | Forwarded |
| `NF_INET_LOCAL_OUT` | After local socket output | Outgoing |
| `NF_INET_POST_ROUTING` | Just before transmitting on the wire | Outgoing |

Each hook point maintains a priority-sorted linked list of registered handler functions. Handlers return `NF_ACCEPT`, `NF_DROP`, or `NF_REJECT`.

**Packet filtering:** Up to 64 static rules matching on src/dst IP, src/dst port, and protocol. Rules can be added/removed via sysctl and the nf_tables interface (`src/net/nf_tables.c`).

**Connection tracking** (`src/net/conntrack.c`):
- Tracks up to 256 concurrent connections (`NF_CONNTRACK_MAX`).
- Per-entry state machine: TCP (SYN_SENT → ESTABLISHED → FIN_WAIT → ...), UDP (UNREPLIED → ASSURED), ICMP (REQUEST → REPLY).
- Tuple-based lookup (src_ip, dst_ip, src_port, dst_port, protocol).
- Timeout management with protocol-specific default timeouts.
- Helper modules for FTP, SIP, etc. (`src/net/conntrack_helpers.c`).

**NAT:** Up to 16 NAT rules supporting SNAT (source NAT for MASQUERADE) and DNAT (destination NAT for port forwarding). Works in conjunction with conntrack for connection tracking.

## Cluster Architecture

The cluster subsystem implements a Kubernetes-inspired container orchestration platform built directly into the kernel. It spans `src/cluster/`, `src/container/`, `src/orch/`, and `src/include/` (cluster, container, orch headers).

``` 
┌──────────────────────────────────────────────────────────────┐
│                    Cluster Management                          │
├──────────────────────────────────────────────────────────────┤
│  ┌──────────┐  ┌──────────┐  ┌──────────┐                   │
│  │   Raft   │  │  Gossip  │  │   Node   │  ┌──────────────┐ │
│  │ Consensus│◄─┤ Protocol │◄─┤  Manager │  │  Controllers │ │
│  └────┬─────┘  └──────────┘  └────┬─────┘  └──────┬───────┘ │
│       │                            │               │         │
│  ┌────▼─────┐  ┌──────────┐  ┌────▼─────┐  ┌──────▼───────┐ │
│  │  Raft KV │  │   Mesh   │  │ Network  │  │  HPA/VPA/    │ │
│  │  Store   │  │ Overlay  │  │ Policies │  │ Autoscaler   │ │
│  └──────────┘  └──────────┘  └──────────┘  └──────────────┘ │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────┐ │
│  │   CRD    │  │ Upgrade  │  │  Node    │  │  Runtime     │ │
│  │  Engine  │  │ Manager  │  │ Problem  │  │  Security    │ │
│  └──────────┘  └──────────┘  └──────────┘  └──────────────┘ │
└──────────────────────────────────────────────────────────────┘
         │                      │
         ▼                      ▼
┌─────────────────┐   ┌──────────────────┐
│ Container       │   │ Orchestration    │
│ Runtime (OCI)   │   │ API Server       │
└─────────────────┘   └──────────────────┘
```

### Cluster Topology

**Files:** `src/cluster/raft.c`, `src/cluster/gossip.c`, `src/cluster/node.c`, `src/cluster/cluster.c`

The cluster uses Raft consensus for consistent distributed state and a gossip protocol for membership and failure detection.

**Raft Consensus (`src/cluster/raft.c`, `src/cluster/raft_kv.c`):**

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│   Leader    │◄────┤  Follower   │◄────┤  Follower   │
│             │     │             │     │             │
│ Term N      │     │ Term N      │     │ Term N      │
│ Log Index M │     │ Log Index M │     │ Log Index M │
└──────┬──────┘     └─────────────┘     └─────────────┘
       │
       │ AppendEntries (heartbeat + log replication)
       └───────────────────────────────────────────────►
```

- **Leader election** — randomized election timeouts (150-300ms) prevent split votes. Candidates request votes via `RequestVote` RPC. A node becomes leader when it receives votes from a majority.
- **Log replication** — the leader appends entries to its local log, then replicates them to followers via `AppendEntries` RPC. Entries are committed once a majority acknowledge.
- **Raft KV store** (`raft_kv.c`) — a linearizable key-value store backed by Raft consensus. Operations: `put(key, value)`, `get(key)`, `delete(key)`. Used for distributed coordination, service discovery, and configuration storage.
- **Snapshot & compaction** — the log is periodically snapshotted to bound growth. Snapshot installation via `InstallSnapshot` RPC for slow/fresh followers.

**Gossip Protocol (`src/cluster/gossip.c`):**

- **SWIM-style membership** — each node maintains a partial view of the cluster and periodically gossips a subset of members to random peers.
- **Suspicion-based failure detection** — nodes are marked `Suspicious` after missed heartbeats, then `Dead` after a configurable suspicion timeout. This prevents false positives from transient network issues.
- **State sync** — cluster membership, node health, and metadata are disseminated via gossip messages. Each message contains a generation counter for conflict resolution.
- **Infection-style dissemination** — new information spreads exponentially: each node that learns of a change includes it in its next gossip round, achieving O(log N) convergence.

**Node Manager (`src/cluster/node.c`):**

```c
struct cluster_node {
    uint32_t    id;              /* unique node identifier */
    char        hostname[64];    /* node hostname */
    uint32_t    ip;              /* management IP address */
    uint16_t    port;            /* cluster communication port */
    uint32_t    state;           /* HEALTHY, UNHEALTHY, UNREACHABLE */
    uint64_t    last_heartbeat;  /* timestamp of last ping */
    uint32_t    cpu_capacity;    /* millicores */
    uint64_t    mem_capacity;    /* bytes */
    uint32_t    pod_capacity;    /* max pods */
    uint32_t    pod_count;       /* current pod count */
};
```

- **Registration** — nodes join by contacting any existing cluster member. Registration data propagates via gossip.
- **Health reporting** — periodic heartbeats with resource utilization (CPU, memory, disk, network).
- **Leader election** — orchestration controller leader elected from among healthy nodes.
- **Pod assignment** — scheduler places pods on nodes based on resource availability, affinity rules, and taints/tolerations.
- **Service endpoint sync** — when pod endpoints change, the node manager updates the service proxy table and propagates via gossip.

### Pod Abstraction

**Files:** `src/container/runtime.c`, `src/container/state.c`, `src/container/orch.c`, `src/orch/pod_health.c`, `src/orch/pod_security.c`

Pods are the smallest deployable unit — one or more containers with shared network namespace, storage volumes, and lifecycle.

```c
struct pod {
    uint32_t        id;            /* pod UID */
    char            name[128];     /* pod name */
    uint32_t        namespace_id;  /* namespace */
    struct pod_spec spec;          /* desired state */
    struct pod_status status;      /* current state */
    container_t    *containers;    /* member containers */
    int             num_containers;
    struct pod_net  net;           /* network namespace, IP, ports */
    struct pod_vol  volumes[8];    /* mounted volumes */
    int             num_volumes;
    uint32_t        node_id;       /* assigned node */
    uint64_t        created_at;    /* creation timestamp */
    uint32_t        restart_count; /* restart policy counter */
};
```

**Pause Container (infrastructure container):**
- Every pod runs a `pause` container first, which holds the pod's network namespace and cgroup parent.
- All application containers in the pod share the pause container's network stack (IP address, port space).
- The pause container is a minimal process that sleeps indefinitely (`pause()` syscall).
- On pod teardown, the pause container is the last to be stopped, ensuring network namespace cleanup.

**Lifecycle:**
```
Pending → Running → Succeeded
              ↓
           Failed     → (restart policy: Always/OnFailure/Never)
              ↓
         Evicted (OOM, node failure, preemption)
```

**Health Checking** (`src/orch/pod_health.c`):
- **Readiness probe** — determines if the pod is ready to serve traffic. Probes: TCP socket check, HTTP GET, command execution, gRPC health check.
- **Liveness probe** — determines if the pod is still running correctly. Same probe types as readiness. Failed liveness → pod restart.
- **Startup probe** — delays liveness checks until the pod has had time to initialize.
- **Probe parameters:** `initial_delay_seconds`, `period_seconds`, `timeout_seconds`, `success_threshold`, `failure_threshold`.
- **Health aggregation:** per-container health is aggregated to pod-level health. A pod is Ready only when all its containers pass readiness checks.

### Service Abstraction

**Files:** `src/container/service_proxy.c`, `src/orch/namespace.c`, `src/cluster/mesh.c`

Services provide stable network endpoints for a set of pods, decoupling clients from individual pod IPs.

```c
struct service {
    uint32_t        id;            /* service UID */
    char            name[128];     /* service name */
    uint32_t        namespace_id;  /* namespace */
    char            cluster_ip[16];/* virtual IP address */
    uint16_t        port;          /* service port */
    uint16_t        target_port;   /* container port */
    const char     *protocol;      /* TCP, UDP, SCTP */
    uint32_t        selector_key;  /* label key for pod selection */
    uint32_t        selector_val;  /* label value for pod selection */
    struct pod_endpoint endpoints[32]; /* backing pod endpoints */
    int             num_endpoints;
    lb_strategy_t   lb;            /* load-balancing strategy */
};
```

**Virtual IP (ClusterIP):**
- Each service gets a stable virtual IP from the cluster CIDR range.
- The service proxy intercepts traffic to the VIP via IPVS or iptables rules and load-balances across healthy pod endpoints.
- VIPs survive pod restarts and scaling events — only the endpoint set changes.

**DNS Discovery:**
- Built-in DNS server resolves `<service>.<namespace>.svc.cluster` to the service VIP.
- Pods discover services via the cluster DNS resolver (configured via `/etc/resolv.conf` in each container).
- DNS records are updated automatically when services or endpoints change.

**Proxy Mechanisms:**
- **IPVS** — kernel-level Layer 4 load balancing with scheduling algorithms: round-robin, least connections, source hashing, destination hashing.
- **iptables** — DNAT-based proxy using netfilter rules for per-service port mapping.
- **Userspace proxy** — fallback mode where a proxy process accepts connections and forwards to pods.
- **Session affinity** — optional sticky sessions based on client IP (via IPVS persistence templates).

**Service Types:**
| Type | Description | Accessible |
|------|-------------|-----------|
| ClusterIP | Internal virtual IP | Within cluster only |
| NodePort | Static port on every node | External via `<nodeIP>:<nodePort>` |
| LoadBalancer | External load balancer integration | External via LB address |
| ExternalName | DNS CNAME alias | Via DNS |

### Controllers

**Files:** `src/cluster/controllers.c`, `src/cluster/hpa.c`, `src/cluster/crd.c`, `src/orch/events.c`

Controllers implement the control loop pattern: observe current state, compare to desired state, and take action to reconcile.

**Horizontal Pod Autoscaler (HPA) (`src/cluster/hpa.c`):**

```
Metrics Collector ──► HPA Controller ──► Scale Target
     │                      │
     ▼                      ▼
  CPU/memory            Compute desired   Update replica count
  utilization             replicas        on the target (e.g.,
  (from cgroup stats)    desired = ceil(    Deployment, StatefulSet)
                          current * (current_utilization
                                     / target_utilization))
```

- **Metrics sources:** CPU utilization (from cgroup CPU accounting), memory utilization (from cgroup memory stats), custom metrics (via external metrics API).
- **Target utilization:** configurable per-HPA (e.g., `targetCPUUtilizationPercentage: 80`).
- **Scaling behavior:** configurable stabilization window, scale-up/down policies, cooldown periods.
- **Supported targets:** Deployments, StatefulSets, ReplicaSets, ReplicationControllers.

**StatefulSet Controller:**
- Manages stateful applications with stable network identities and persistent storage.
- Pods are created in order (0 to N-1) and deleted in reverse order.
- Each pod gets a unique network identity (`<name>-<ordinal>`) and a dedicated PVC.
- Supports rolling updates with ordered pod replacement.
- On failure, the controller recreates the pod with the same identity and storage.

**CronJob Controller:**
- Runs jobs on a time-based schedule (standard cron syntax with second precision).
- Supports concurrency policies: Allow, Forbid, Replace.
- Job history limits (successful/failed jobs retained).
- Timezone support for timezone-aware scheduling.

**Custom Resource Definition (CRD) Engine (`src/cluster/crd.c`):**
- Define new resource types at runtime via CRD objects.
- CRDs support arbitrary schema validation (field types, required fields, nested structures).
- Custom resources are stored in the Raft KV store and watched by controllers.
- CRD lifecycle: register → validate → store → watch → reconcile.
- Controllers can watch custom resources and reconcile to external systems.

**Operator Framework:**
- Built-in operator pattern: a controller watches a custom resource and reconciles the cluster state.
- Operators can manage complex applications with domain-specific logic.
- Operator lifecycle: install operator → register CRD → create custom resource → operator reconciles.
- Cluster event system (`src/orch/events.c`) feeds operator event loops.

### Network Overlay

**Files:** `src/cluster/overlay.c`, `src/cluster/mesh.c`, `src/cluster/network_policy.c`

The cluster implements VXLAN-based overlay networking for inter-node pod communication, with optional WireGuard encryption.

``` 
┌───────────────────┐       VXLAN Tunnel       ┌───────────────────┐
│   Node A          │◄─────────────────────────►│   Node B          │
│                   │                           │                   │
│  ┌─────┐  ┌─────┐│     ┌──────────────┐      │┌─────┐  ┌─────┐  │
│  │Pod1 │  │Pod2 ││     │   Overlay    │      ││Pod3 │  │Pod4 │  │
│  │10.0.1.2│10.0.1.3│     │   Network    │      ││10.0.2.2│10.0.2.3│
│  └──┬──┘  └──┬──┘│     │ 10.0.0.0/16  │      │└──┬──┘  └──┬──┘  │
│     │        │    │     └──────────────┘      │   │        │      │
│  ┌──▼────────▼──┐ │                           │ ┌─▼────────▼──┐  │
│  │  veth pair   │ │                           │ │  veth pair   │  │
│  │  → cbr0      │ │                           │ │  → cbr0      │  │
│  └───────┬──────┘ │                           │ └──────┬───────┘  │
│          │         │                           │        │          │
│  ┌───────▼──────┐  │                           │ ┌──────▼───────┐  │
│  │  VXLAN VTEP  │  │                           │ │  VXLAN VTEP  │  │
│  │  (vxlan0)    │  │                           │ │  (vxlan0)    │  │
│  │  192.168.1.1 │  │                           │ │  192.168.1.2 │  │
│  └───────┬──────┘  │                           │ └──────┬───────┘  │
│          │         │                           │        │          │
│   Underlay: eth0   │                           │   Underlay: eth0  │
└───────────────────┘                           └───────────────────┘
```

**VXLAN Overlay (`src/cluster/overlay.c`):**
- VXLAN encapsulation (RFC 7348) with 24-bit VNI for network isolation.
- Each namespace gets a unique VNI, providing Layer 2 isolation between tenants.
- UDP encapsulation over the physical underlay network (port 8472).
- ARP suppression and MAC learning via the built-in forwarding database (FDB).
- Broadcast, unknown-unicast, and multicast (BUM) traffic replicated via IP multicast or head-end replication.

**Network Policy (`src/cluster/network_policy.c`):**
- **Ingress rules:** allow traffic from pods matching label selectors, IP CIDR blocks, or namespaces.
- **Egress rules:** allow outbound traffic to pods, CIDR blocks, or DNS names.
- **Policy types:** Allow (whitelist) and Deny (blacklist), with Deny taking precedence.
- **Namespace isolation:** per-namespace default policies (Deny All, Allow All, or custom).
- **Rule evaluation:** policies are compiled into nftables rulesets on each node for efficient packet filtering.

**Ingress Controller:**
- **NodePort:** each service port is exposed on a static port (30000-32767) on all cluster nodes.
- **LoadBalancer:** integrates with external load balancers (or uses IPVS/TUN for direct routing).
- **HTTP routing:** path-based and host-based routing to backend services, with TLS termination and session affinity.
- The ingress controller runs as a DaemonSet, listening on host ports and proxying to service endpoints.

**WireGuard Mesh (`src/cluster/mesh.c`):**
- Full-mesh WireGuard VPN between cluster nodes for encrypted overlay traffic.
- Automatic peer discovery and key rotation via the Raft KV store.
- Supports split-tunnel mode: cluster traffic via WireGuard, external traffic via the physical interface.

## Filesystem Stack Architecture

The filesystem stack implements a Linux-compatible VFS layer with multiple on-disk, in-memory, and pseudo-filesystems.

```
System Calls (open/read/write/close/stat/mount/umount)
     ↕
┌─────────────────────────────────────────────┐
│  VFS Layer                                  │
│  ┌───────────────────────────────────────┐  │
│  │ vnode operations (vfs_ops)            │  │
│  │   → open, read, write, close, seek    │  │
│  │   → stat, readdir, ioctl, truncate    │  │
│  │ Path resolution (dcache + walk)       │  │
│  │ Dentry cache (LRU, shrink under OOM)  │  │
│  │ File locks (POSIX advisory)           │  │
│  │ Inotify/fanotify, xattr, POSIX ACLs   │  │
│  └───────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│  Mount System                               │
│  ┌───────────────────────────────────────┐  │
│  │ Mount table (global + per-namespace)  │  │
│  │ Propagation types: SHARED/SLAVE/PRIVATE│  │
│  │ Bind mounts, recursive bind mounts    │  │
│  │ Mount namespace (CLONE_NEWNS)         │  │
│  └───────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│  Supported Filesystems                      │
│  ┌───────────────────────────────────────┐  │
│  │ Disk-backed: FAT32, ext2, iso9660,    │  │
│  │              squashfs, cramfs, minix,  │  │
│  │              ufs, sysv, adfs, bfs,     │  │
│  │              hfs, erofs, f2fs, jffs2,  │  │
│  │              nilfs2, nfs               │  │
│  │ In-memory:   tmpfs, ramfs, tarfs,     │  │
│  │              cpio, romfs               │  │
│  │ Pseudo:      procfs, sysfs, devfs,    │  │
│  │              debugfs, overlay, FUSE    │  │
│  └───────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│  Block Cache & Buffer Cache                 │
│  ┌───────────────────────────────────────┐  │
│  │ Page cache (src/fs/page_cache.c)      │  │
│  │   LRU eviction, dirty writeback,      │  │
│  │   readahead, working-set estimation   │  │
│  │ Buffer cache (src/fs/bufcache.c)      │  │
│  │   64-entry LRU sector cache per dev   │  │
│  │   Dirty tracking, write-back on evict │  │
│  │ Block I/O scheduler (src/fs/iosched.c)│  │
│  └───────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│  Block Device Layer                         │
│  ATA, AHCI, NVMe, virtio-blk, loop, nbd,   │
│  ramdisk, MD RAID, device-mapper, bcache    │
└─────────────────────────────────────────────┘
```

### VFS Layer

**Files:** `src/kernel/vfs.c`, `src/include/vfs.h`, `src/fs/vfs_enhance.c`

The VFS (Virtual Filesystem) layer provides a uniform interface over all filesystem implementations. Key abstractions:

- **vnode** (`struct fs_inode`) — per-file metadata: inode number, size, type, permissions, link count, timestamps, block pointers.
- **superblock** (`struct fs_super`) — per-filesystem metadata: total blocks, file and block limits, bitmap, root inode.
- **mount entry** (`struct vfs_mount`) — per-mount binding: mountpoint path, filesystem ops table (`struct vfs_ops`), private data, flags (read-only, bind mount, encryption).

Operation dispatching:

```
open(path)   → vfs_open()   → resolve dentry → mount lookup → mount->ops->open()
read(fd)     → vfs_read()   → file → mount → mount->ops->read()
write(fd)    → vfs_write()  → file → mount → mount->ops->write()
close(fd)    → vfs_close()  → file → mount → mount->ops->close()
stat(path)   → vfs_stat()   → resolve dentry → mount->ops->stat()
mount(dev,   → vfs_mount()  → lookup fs type → create superblock →
  mp, type)                    install in mount table
```

**Path resolution** (`dcache_lookup`/`dcache_add` in `src/kernel/vfs.c`):
- Fixed-size dentry cache (`DCACHE_SIZE` entries) keyed by absolute path.
- LRU eviction on insert; `dcache_shrink()` called from OOM path.
- `mnt_ns_resolve()` matches the longest prefix against the mount table to find the target filesystem.

**Additional VFS features:**
- **File locks** (`src/drivers/file_lock.c`) — POSIX advisory locks (F_RDLCK, F_WRLCK, F_UNLCK).
- **Inotify/fanotify** (`src/kernel/fsnotify.c`) — filesystem event notification.
- **xattr** (`src/fs/xattr.c`) — extended attributes for security labels and user metadata.
- **POSIX ACLs** (`src/fs/posix_acl.c`) — fine-grained access control with ACL_USER, ACL_GROUP, ACL_MASK, ACL_OTHER.
- **Quotas** (`src/fs/quota.c`) — per-user block/inode limits.

### Supported Filesystems

| FS | Type | Read/Write | Key Features |
|----|------|-----------|--------------|
| **tmpfs** | In-memory | R/W | Dynamic sizing, symlinks, device nodes, O_TMPFILE |
| **ramfs** | In-memory | R/W | Simple RAM-backed FS without size limit |
| **fat32** | Disk | R/W | FAT12/16/32, LFN read+write, volume labels |
| **ext2** | Disk | R/W | Sparse files, large files, symlinks, fast symlinks, HTree |
| **iso9660** | Disk | RO | Rock Ridge (POSIX), Joliet (Unicode), multi-session |
| **squashfs** | Disk | RO | Compressed read-only filesystem |
| **cramfs** | Disk | RO | Compressed ROM filesystem |
| **tarfs** | Archive | RO | Embedded initramfs, TAR archive mount |
| **cpio** | Archive | RO | cpio archive mount |
| **romfs** | Archive | RO | Simple read-only filesystem |
| **procfs** | Pseudo | RO | `/proc/{uptime,meminfo,cpuinfo,stat,self,interrupts,...}` |
| **sysfs** | Pseudo | RO | kobject tree, kernel parameters, device hierarchy |
| **devfs** | Pseudo | R/W | Dynamic device node creation, hotplug |
| **debugfs** | Pseudo | R/W | Kernel debug data, register dumps |
| **overlay** | Union | R/W | Union mount (upper + lower dirs, whiteouts, copy-up) |
| **FUSE** | User | R/W | Userspace filesystem via FUSE protocol |
| **minix** | Disk | R/W | Minix filesystem (v1/v2/v3) |
| **ufs** | Disk | R/W | Unix File System (FFS) |
| **sysv** | Disk | R/W | System V filesystem |
| **hfs** | Disk | R/W | Hierarchical File System (Mac) |
| **nfs** | Network | R/W | Network Filesystem (client) |
| **erofs** | Disk | RO | Enhanced Read-Only Filesystem |
| **f2fs** | Disk | R/W | Flash-Friendly Filesystem |
| **jffs2** | Flash | R/W | Journaling Flash File System v2 |
| **nilfs2** | Disk | R/W | Log-structured filesystem |

**On-disk filesystem source files:**

| FS | Source | Notes |
|----|--------|-------|
| FAT32 | `src/fs/fat32.c`, `src/fs/vfat_shortname.c` | VFAT long filename support |
| ext2 | `src/fs/ext2.c` | HTree directory indexing |
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
- **Readahead** — window-based prefetching (`READAHEAD_WINDOW_MIN`/`MAX`, configurable). Adaptive window sizing based on access pattern. Per-file readahead trackers (`src/fs/page_cache.c`).
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

- **LRU eviction** with a doubly-linked list. Access frequency counter for working-set estimation.
- **Dirty write-back** on eviction — guaranteed before a dirty entry is reused.
- **Stats:** hits, misses, writes exposed for diagnostics.

**Block I/O Scheduler** (`src/fs/iosched.c`):
Implements standard I/O scheduling policies (deadline, CFQ-like) for merging and reordering block requests.

**Initramfs** (`src/fs/initramfs.c`):
Embedded initramfs on disk image, built from cpio/tar archives. Mounted early at boot before the root filesystem is available.

### Mount Table and Namespace Support

**Global mount table:** Defined as `struct vfs_mount mounts[VFS_MAX_MOUNTS]` (16 entries). Each mount entry records:
- Mountpoint path (e.g., `/mnt/usb`)
- `struct vfs_ops *ops` — filesystem-specific operation table
- `void *priv` — per-filesystem private data (e.g., FAT32 device state)
- Flags (`MS_RDONLY`, `MS_BIND`, encryption state)
- Bind mount source path (for bind mounts)
- Journaling state, encryption key

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
- **CLONE_NEWNS** — creates a new namespace with a deep copy of the parent's mount table. Subsequent `mount()`/`umount()` operations affect only that namespace.
- **Reference counted** — freed when the last process using it exits or calls `unshare()`.
- **Propagation types** — mounts can be SHARED (events propagate to peers), SLAVE (receive events from master), or PRIVATE (fully isolated). Implemented in `src/kernel/fs_mount_prop.c`.

**Mount API:**
- `vfs_mount(mountpoint, ops, priv, flags)` — install a new mount in the current namespace.
- `vfs_umount(mountpoint)` — remove a mount.
- `mnt_ns_resolve(ns, path)` — find the best-matching mount for a given path (longest prefix match).
- `mnt_ns_list_mounts(ns, buffer)` — enumerate mounts (for `/proc/mounts`).
- `mnt_ns_sync(ns)` — flush all dirty buffers on mounted filesystems.
