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
