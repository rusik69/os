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

The kernel uses a high-half virtual memory layout on x86-64. Physical memory is identity-mapped at offset `KERNEL_VMA_OFFSET` (0xFFFF800000000000), so any physical address `phys` can be accessed as `PHYS_TO_VIRT(phys) = phys + 0xFFFF800000000000`.

**PML4 entries used:**
- Entry 0   → identity map (low 512 GB, removed after boot)
- Entry 256 → kernel high-half map (0xFFFF800000000000+)
- Entries 257–511 → per-process userspace (one PML4 per process)

### Physical Memory Layout

```
Address               Region                      Notes
────────────────────  ──────────────────────────  ──────────────────────────
0x0000000000000000    ┌──────────────────────┐
                      │ Low Memory           │ IVT (0x00000–0x003FF)
                      │                      │ BDA (0x00400–0x004FF)
                      │                      │ EBDA (0x9FC00+)
0x000000000009FC00    ├──────────────────────┤
                      │ BIOS / ACPI / SMBIOS │ Firmware data tables
0x0000000000100000    ├──────────────────────┤
                      │ Kernel binary        │ Loaded by GRUB (Multiboot)
                      │ .multiboot / .boot   │
0x0000000000126000    ├──────────────────────┤
                      │ Kernel .text / .roda-│ Linked at KERNEL_TEXT_LMA
                      │ ta / .data / .bss    │
kernel_end            ├──────────────────────┤
                      │ Free physical memory │ Managed by PMM bitmap
                      │ (PMM frames)         │ Per-CPU hot caches,
                      │                      │ refcounting, COW
0x0000000008000000    ├──────────────────────┤
                      │ PCI MMIO / ACPI      │ Device BARs, MMCFG,
                      │                      │ FADT, MADT
0x0000000100000000    ├──────────────────────┤
                      │ Hotplug / high mem   │ NUMA node memory,
                      │                      │ device-dax ranges
0xFFFFFFFFFFFFFFFF    └──────────────────────┘
```

### Virtual Memory Layout (per-process address space)

```
Half   Start                 End                Region          Details
─────  ─────                 ───                ──────          ───────
Lower  0x0000000000000000   0x00007FFFFFFFFFFF  Userspace       Process-private
       ┌───────────────────────────────────────────────────┐
       │  .text   (ELF entry, ASLR-randomized start)       │
       │  .rodata                                          │
       │  .data / .bss                                     │
       │  heap (brk, ASLR-randomized start address)        │
       │  mmap (shared libs, ASLR-randomized)              │
       │  stack (USER_STACK_SIZE = 64 KB + guard page,     │
       │         ASLR-randomized top)                      │
       └───────────────────────────────────────────────────┘
       (unmapped) — access causes page fault

Upper  0xFFFF800000000000   0xFFFFFFFFFFFFFFFF  Kernel space   All processes share
       ┌───────────────────────────────────────────────────┐
       │  Kernel direct map (PHYS_TO_VIRT)                  │
       │  VMA = phys + KERNEL_VMA_OFFSET                   │
       │  ┌─────────────────────────────────────────────┐   │
0xFFFF800000126000 │  .text section                        │ RX│
0xFFFF80000xxxxxxx │  .init.text / .rodata                 │ RO│
0xFFFF80001xxxxxxx │  .data / .bss                         │ RW│
       │  └─────────────────────────────────────────────┘   │
       │  ┌─────────────────────────────────────────────┐   │
0xFFFF800004200000 │  Kernel heap (kmalloc arena)           │ RW│
       │  │   HEAP_PHYS_BASE = 0x04200000 (66 MB)      │   │
       │  │   HEAP_MAX_SIZE = 64 MB                       │   │
       │  │   First-fit free-list with coalescing         │   │
       │  │   Initial: 16 KB, grows via heap_expand()     │   │
       │  │   Block header: magic(8)+size(8)+free(4)+     │   │
       │  │                    next(8)+prev(8) = 40 bytes │   │
       │  └─────────────────────────────────────────────┘   │
       │  ┌─────────────────────────────────────────────┐   │
0xFFFF800100000000 │  Module region (MODULES_VADDR)         │   │
       │  │   MODULES_SIZE  = 64 MB                       │   │
       │  │   .text   → RX  (code)                        │   │
       │  │   .rodata → RO  (constants)                   │   │
       │  │   .data   → RW  (globals)                     │   │
       │  │   .bss    → RW  (zero-fill)                   │   │
0xFFFF800140000000 │  ── Module region end (MODULES_END)   │   │
       │  └─────────────────────────────────────────────┘   │
       │  ┌─────────────────────────────────────────────┐   │
       │  │  Per-CPU kernel stacks                       │   │
       │  │  KERNEL_STACK_SIZE = 128 KB per task         │   │
       │  │  IRQ_STACK_SIZE    =  16 KB per CPU          │   │
       │  │  Guard page (unmapped) below each stack      │   │
       │  └─────────────────────────────────────────────┘   │
       │  ┌─────────────────────────────────────────────┐   │
       │  │  io_uring SQ / CQ rings                     │   │
       │  │  (shared mmap between kernel and userspace)  │   │
       │  └─────────────────────────────────────────────┘   │
       │  ┌─────────────────────────────────────────────┐   │
       │  │  Page tables (PML4 / PDPT / PD / PT)        │   │
       │  │  Allocated from PMM, accessed via            │   │
       │  │  PHYS_TO_VIRT                               │   │
       │  └─────────────────────────────────────────────┘   │
       │  ┌─────────────────────────────────────────────┐   │
       │  │  KASAN shadow memory                        │   │
       │  │  (if CONFIG_KASAN_LIGHT is enabled)          │   │
       │  └─────────────────────────────────────────────┘   │
0xFFFFFFFFFFFFFFFF └───────────────────────────────────────────┘
```

### Key Address Constants

| Constant          | Value                     | Description                               |
|-------------------|---------------------------|-------------------------------------------|
| `KERNEL_VMA_OFFSET` | `0xFFFF800000000000`    | High-half base for direct physical map    |
| `KERNEL_LMA`        | `0x100000`              | Physical load address (Multiboot entry)   |
| `KERNEL_TEXT_LMA`   | `0x126000`              | Physical address of `.text` section       |
| `MODULES_VADDR`     | `0xFFFF800100000000`    | Virtual base of loadable module region    |
| `MODULES_SIZE`      | `0x04000000` (64 MB)    | Size of module region                     |
| `MODULES_END`       | `0xFFFF800140000000`    | End of module region                      |
| `HEAP_MAX_SIZE`     | 64 MB                   | Maximum kernel heap (kmalloc) size        |
| `HEAP_INITIAL`      | 16 KB                   | Initial kernel heap reservation           |
| `KERNEL_STACK_SIZE` | 128 KB                  | Per-task kernel stack size                |
| `USER_STACK_SIZE`   | 64 KB                   | Per-process userspace stack size          |
| `IRQ_STACK_SIZE`    | 16 KB                   | Per-CPU interrupt stack size              |
| `PAGE_SIZE`         | 4096                    | x86-64 page size (4 KB)                   |

### Address Translation

Physical addresses are converted to kernel virtual addresses via the direct-map offset:

```c
#define KERNEL_VMA_OFFSET  0xFFFF800000000000ULL
#define PHYS_TO_VIRT(addr) ((void *)((uint64_t)(addr) + KERNEL_VMA_OFFSET))
#define VIRT_TO_PHYS(addr) ((uint64_t)(uintptr_t)(addr) - KERNEL_VMA_OFFSET)
```

**Example:** Kernel `.text` at physical `0x126000` is accessed at virtual `0xFFFF800000126000`.
PML4 entry 256 maps the kernel's PDPT which itself maps the kernel's PDs — everything from `0xFFFF800000000000` upward.

## Boot Sequence

The full boot path from power-on to userspace spans four distinct phases:
firmware → bootloader → kernel → init.

```
 BIOS          GRUB              boot.asm          kernel_main         init (PID 1)
 ┌─────┐      ┌──────┐          ┌────────┐         ┌──────────┐        ┌──────────┐
 │POST │ ───> │ Load │ ───────> │32-bit  │ ──────> │ Init all  │ ───> │ Fork     │
 │MBR  │      │kernel│          │startup │         │subsystems │       │shell loop│
 │Grub │      │at    │          │PAE+LM  │         │Spawn init │       │Reap kids │
 │menu │      │1 MB  │          │→64-bit │         │→ idle loop│       │Shutdown  │
 └─────┘      └──────┘          └────────┘         └──────────┘        └──────────┘
    ↓            ↓                  ↓                   ↓                   ↓
 Real mode    Protected          Long mode           Long mode           Long mode
 (16-bit)     (32-bit)           (64-bit)            (64-bit)            (64-bit)
                                  Ring 0              Ring 0              Ring 3
```

### Phase 0: BIOS / Firmware (real mode, 16-bit)

1. **Power-on reset** — CPU starts executing at `0xFFFFFFF0` (reset vector) in real mode.
   CS:IP = 0xF000:0xFFF0. Jumps to BIOS entry point.
2. **POST** — Power-On Self-Test: CPU/detection, memory sizing (via CMOS/SPD),
   chipset initialization, PCI bus enumeration, option ROM execution.
3. **Boot device selection** — BIOS checks boot order (floppy → HDD → CD-ROM → PXE).
   Reads the Master Boot Record (MBR, LBA 0) into `0x7C00` and transfers control.
4. **Bootloader (GRUB)** — The first 446 bytes of the MBR load GRUB's Stage 2 from
   the boot partition. GRUB reads `/boot/grub/grub.cfg`, presents a boot menu,
   and parses the target kernel's ELF / Multiboot1 header.
5. **Kernel loading** — The Multiboot1 header (at offset 0 of the kernel image)
   specifies `load_addr = 0x100000` (1 MB — the conventional x86 kernel load
   point above the 640 KB–1 MB BIOS/ROM hole). GRUB copies the kernel segments
   to physical memory and passes control to `_start` with:
   - **EAX** = `0x2BADB002` (Multiboot magic — validates the loader)
   - **EBX** = physical address of the Multiboot info structure

### Phase 1: boot.asm (32-bit protected mode → long mode)

6. `_start` (32-bit): saves the multiboot info pointer, sets up a bootstrap stack
   (128 KB in the `.boot` section), and builds page tables:
   - **PML4[0]** → `boot_pdpt` (identity map for physical 0x0–0x3FFFFFFF)
   - **PML4[256]** → same `boot_pdpt` (high-half alias: kernel at `0xFFFF800000000000+`)
   - **PDPT[0]** → `boot_pd` (512 × 2 MB huge pages = 1 GB, phys 0x0–0x3FFFFFFF)
   - **PDPT[3]** → `boot_pd2` (512 × 2 MB huge pages, phys 0xC0000000–0xFFFFFFFF / PCI MMIO)
7. Enables PAE (CR4.PAE) → sets IA32_EFER.LME (Long Mode Enable MSR bit 8) →
   enables paging (CR0.PG, which atomically activates long mode).
8. Loads the 64-bit GDT and executes a far jump to `long_mode_entry` (long mode CS).

### Phase 2: long_mode_entry (64-bit long mode)

9. Reloads all data segments with the 64-bit flat-model selectors.
10. Zeroes the `.bss` section (all uninitialized globals).
11. Initializes KASLR offset (stubbed to 0; `kaslr_init()` in C randomizes later).
12. Switches the stack pointer to the high-half VMA (`RSP += KERNEL_VMA_OFFSET`)
    so all C code uses `0xFFFF800000000000+` addresses from the start.
13. Calls `kernel_main(multiboot_magic, multiboot_info_phys)`.

### Phase 3: kernel_main — kernel subsystem initialization

14. Initialization order (linear, ~40+ steps in `src/kernel/kernel.c`):

    | # | Call                      | Purpose                                      |
    |---|---------------------------|----------------------------------------------|
    | 1 | `early_serial_init()`     | Early COM1 debug output before any state     |
    | 2 | `gdt_init()`              | Final GDT with TSS + IST entries             |
    | 3 | `pic_init()`              | Remap PIC IRQs to vectors 0x20–0x2F          |
    | 4 | `idt_init()`              | IDT with interrupt gates and IST assignments |
    | 5 | `stack_guard_init()`      | Guard pages below kernel stacks              |
    | 6 | `pmm_init()`              | Detect physical memory from Multiboot info   |
    | 7 | `ist_init()`              | IST stacks for #DF / NMI / MCE              |
    | 8 | `fault_init()`            | Register exception handlers                  |
    | 9 | `vmm_init()`              | Finalize page tables, enable NX/PAT          |
    |10 | `cpu_security_init()`     | SMEP, SMAP, NXE, UMIP                        |
    |11 | `kpti_init()`             | Kernel Page-Table Isolation (Meltdown fix)   |
    |12 | `heap_init()`             | kmalloc arena (first-fit, 16 KB initial)     |
    |13 | `slab_init()`             | kmem_cache subsystem (per-CPU caches)        |
    |14 | `process_init()`          | Idle process, process table                  |
    |15 | `scheduler_init()`        | Per-CPU runqueues, pick_next_task            |
    |16 | `apic_init_local()`       | Local APIC (replaces PIC for interrupts)     |
    |17 | `smp_boot_aps()`          | INIT-SIPI-SIPI → discover + start AP cores   |
    |18 | `timer_init()`            | PIT / HPET / TSC deadline timer (100 Hz)    |
    |19 | `x2apic_init()`           | Switch to x2APIC mode (if CPU supports)      |
    |20 | `timers_init()`           | High-resolution timers, timerfd              |
    |21 | `workqueue_init()`        | Deferred work execution threads              |
    |22 | `modules_init()`          | Kernel module (kmod) API + symbol table      |
    |23 | `acpi_init()`             | Parse ACPI tables (FADT, MADT, DSDT, ...)    |
    |24 | `syscall_init()`          | MSR_LSTAR syscall fast-path                  |
    |25 | `vfs_init()`              | Virtual filesystem layer                     |
    |26 | `devtmpfs_init()`         | Dynamic /dev device node creation            |
    |27 | `procfs_init()`           | /proc virtual filesystem                     |
    |28 | `sysfs_init()`            | /sys kernel object tree                      |
    |29 | `keyboard_init()`         | PS/2 keyboard IRQ handler                    |
    |30 | `rtc_init()`              | Real-time clock (CMOS-based)                 |
    |31 | `ata_init()` / `ahci_init()` | Storage: ATA PIO / AHCI SATA            |
    |32 | `nvme_init()`             | NVMe SSD (queue pairs)                       |
    |33 | `fs_init()`               | Mount root filesystem (tmpfs or disk)        |
    |34 | `initramfs_extract()`     | Extract embedded CPIO archive                |
    |35 | `pci_init()`              | Enumerate PCI bus, discover devices          |
    |36 | `usb_init()`              | USB controller + device enumeration          |
    |37 | `net_init()`              | Network stack + NIC (e1000/virtio/vmxnet3)   |
    |38 | `dhcp_discover()`         | DHCP client for IP assignment                |
    |39 | `service_start()`         | Start telnetd, httpd, sshd                   |
    |40 | `container_init()`        | OCI container runtime                        |

15. After all subsystems are initialized, `kernel_main` loads the initrd from
    the Multiboot module (if present), then spawns userspace init:
    ```c
    int pid = process_spawn_kernel("/mnt/sbin/init");
    ```
    If `init=` was given on the kernel command line, that path is used instead.

16. The kernel module subsystem is initialized in two distinct phases.

    **Phase 3a — module API bring-up (early):**
    - `modules_init()` — allocates the module table (`MODULE_MAX` slots),
      locking, and the module-state machine (`UNUSED → LOADING → LIVE`).
    - `module_sig_init()` — sets up module signature/key verification.
    - `ksym_init()` — builds the exported-kernel-symbol table used for
      resolving `EXPORT_SYMBOL()` references when a module is loaded.
    - `blockdev_init()` — registers the block-device table *before*
      `do_initcalls()` runs the driver initcalls that register devices.

    **Phase 3b — on-demand module loading (after rootfs is mounted):**
    - `do_initcalls()` runs all registered driver/`module_init()` initcalls.
    - From any context, the kernel may call `request_module("name")` to
      autoload `/modules/<name>.ko` (mirroring Linux's `/lib/modules/` layout):
      * PCI subsystem → `request_module("pci:v...d...")` on device probe;
      * VFS on mount   → `request_module("ext2")` for an unregistered FS;
      * socket create  → `request_module("ipv6")` for an unsupported family.
    - `request_module()` runs the same ELF loader as `sys_init_module()`
      (userspace `msyscall`-style path) and may sleep on I/O. The full load
      pipeline is: verify signature → allocate module memory in the
      `MODULES_VADDR` region → relocate `.text/.rodata/.data/.bss` → resolve
      `EXPORT_SYMBOL` deps (loading them first via `request_module()`) →
      run the module's `init_module()` → mark state `LIVE`.
    - `/sbin/modprobe` and the `init_module`/`finit_module` syscalls expose
      the same path to userspace.

17. The boot thread then transitions to the **idle loop** — reaping zombie
    processes and executing `cpuidle_idle()` when no tasks are runnable.

### Phase 4: Userspace Init (PID 1)

17. **`/sbin/init`** (source: `userspace/init/init.c`) is the first userspace
    process. It manages the entire user session lifecycle:

    1. **Signal handler registration** — Installs SIGTERM → `shutdown_handler`
       and SIGINT → `forward_signal_to_child`. These custom handlers are
       registered before any `fork()` so children inherit `SIG_DFL`.
    2. **Console setup** — Opens `/dev/console` and `dup2()`s it onto
       stdin/stdout/stderr. Falls back to raw I/O if the device node doesn't
       exist (e.g., early boot without devtmpfs).
    3. **Shell spawn loop** — The main loop `fork()`s a child, attempts
       `execve("/bin/getty", ...)` for a login prompt, falling back to
       `execve("/bin/sh", ...)` if getty is unavailable. Saves child PID
       for signal forwarding, reaps zombie orphans, then blocks on `waitpid()`.
       On child exit the loop respawns (unless shutting down).
    4. **Zombie reaping** — `reap_children()` uses `waitpid(-1, WNOHANG)`
       in a loop to collect orphaned grandchildren reparented to PID 1.
    5. **Shutdown sequence** — On SIGTERM: sets `g_shutting_down`, forwards
       the signal to the child, waits for child to exit, then calls `sync()`
       twice and `reboot()`. If `reboot()` returns, enters a permanent HLT
       loop.
    6. **Fallback halt** — If `fork()` or `execve()` fails fatally, init
       prints an error and pauses forever (preventing a kernel panic from
       init's exit).

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

## Driver Model

This kernel uses a hybrid driver model combining **compile-time registration** (via the initcall system), **direct initialization in `kernel_main()`**, and **loadable kernel modules** (for runtime-loaded drivers). There is no unified `struct device_driver`/`struct device` bus abstraction like Linux; instead, each driver subsystem defines its own registration and probe mechanism.

### Driver Registration API Summary

Each driver subsystem exposes a registration entry point that a driver
calls at init (from an initcall, `kernel_main()`, or its `module_init`):

| Subsystem | Registration call | What it binds |
|-----------|-------------------|---------------|
| Initcall | `device_initcall(fn)` | Places `fn` in `.initcall.5`; run by `do_initcalls()` |
| Kernel module | `module_init(fn)` (+ `sys_init_module`/`finit_module`) | Loadable `.ko` ELF; `fn` runs as `init_module` |
| Block device | `blockdev_register(id, name, submit_fn, ...)` / `blockdev_register_legacy(...)` / `blockdev_register_scsi_cmd(...)` | Registers a block device with submit (or sync r/w) callbacks and capacity |
| Network device | `netif_register(struct net_device *dev)` | Binds a NIC name/MAC/`netdev_ops` into the netdevice layer |
| USB | `usb_register_driver(struct usb_driver *driver)` | Binds a `usb_device_id` table + probe/disconnect to the hub HCI |
| PCI | `pci_autoprobe_work()` → `request_module(modalias)` | Deferred modalias-based module autoload after boot |
| Char/dev node | `devtmpfs` + per-driver `mknod` | Creates dynamic device nodes under `/dev` |

The two main discovery paths are:

1. **Compile-time (initcall):** a driver's `device_initcall(fn)` runs once
   during `do_initcalls()`, calling its registration call directly on
   hardware that is statically known to exist.
2. **Deferred autoprobe (modalias):** for buses (notably PCI), discovered
   devices emit a modalias string; `pci_autoprobe_work()` runs in a
   workqueue and calls `request_module(modalias)`, which loads the matching
   `.ko` whose `module_init` then calls its registration call.

Loadable module drivers additionally must be exported (`EXPORT_SYMBOL`)
so the kernel symbol table can resolve their references; see the module
loading phases in the Boot Sequence section.

### Initcall-Based Driver Registration

The initcall system (`src/include/initcall.h`) provides a linker-section-based mechanism for registering driver initialization functions at compile time:

```c
typedef void (*initcall_t)(void);

#define pure_initcall(fn)       __define_initcall(fn, 0)   /* earliest */
#define core_initcall(fn)       __define_initcall(fn, 1)
#define postcore_initcall(fn)   __define_initcall(fn, 2)
#define arch_initcall(fn)       __define_initcall(fn, 3)
#define subsys_initcall(fn)     __define_initcall(fn, 4)
#define device_initcall(fn)     __define_initcall(fn, 5)   /* default for drivers */
#define fs_initcall(fn)         __define_initcall(fn, 4)   /* fs alias */
#define late_initcall(fn)       __define_initcall(fn, 5)   /* latest */
```

Each macro places a function pointer into a specific `.initcall.N` linker section. The linker script (linker.ld) emits these sections in level order. `do_initcalls()` in `src/kernel/kernel.c` iterates from `__initcall_start` to `__initcall_end`, calling every registered function once during boot.

**Typical driver usage:**

```c
#include "initcall.h"

static int my_driver_init(void) {
    /* probe hardware, register IRQ handlers, set up device state */
    return 0;
}
device_initcall(my_driver_init);
```

Drivers compiled as **loadable modules** use `module_init(fn)` instead. For built-in compilation, `module_init` maps to `device_initcall`; for module compilation it creates an `init_module` alias that the ELF module loader can call by name.

### Direct Initialization in `kernel_main()`

Many core drivers and infrastructure modules are called explicitly in `kernel_main()` as part of the 17-phase boot sequence (see Boot Sequence, Phase 3). This covers drivers that must be available before `do_initcalls()` runs or that have strict ordering dependencies:

| Phase | Drivers initialized | Examples |
|-------|-------------------|----------|
| 1 – Early bootstrap | Serial, VGA | `serial_init()`, `vga_init()` |
| 6 – Block foundations | Ramdisk, tmpfs, framebuffer console | `ramdisk_init()`, `tmpfs_init()` |
| 8 – Interrupt / SMP | Timer, APIC, workqueue | `timer_init()`, `apic_init_local()` |
| 10 – Device infrastructure | Keyboard, RTC, ACPI, serial IRQ | `keyboard_init()`, `rtc_init()`, `acpi_init()` |
| 13 – Block layer & storage | ATA, AHCI, NVMe, device-mapper | `ata_init()`, `ahci_init()`, `dm_init()` |
| 15 – PCI, GPU, USB | PCI bus, Intel GPU, USB core | `pci_init()`, `intel_gpu_init()`, `usb_init()` |
| 16 – Network | e1000, virtio-net, bridge, bonding | `e1000_init()`, `bridge_init()`, `bonding_init()` |

Drivers initialized this way have deterministic ordering and can rely on all preceding phases being complete.

### PCI Subsystem (`src/drivers/pci.c`)

The PCI subsystem is the most structured bus-level driver framework in the kernel. It provides:

**Configuration Space Access (two methods):**

1. **Legacy I/O Port (CF8/CFC):** Write bus/slot/func/offset to port 0xCF8, then read/write the 32-bit value via port 0xCFC. Used when PCIe ECAM is unavailable. Supports 256-byte standard config space.

2. **PCIe ECAM (memory-mapped):** The physical ECAM base address is read from the ACPI MCFG table and mapped into the kernel virtual address space using 2MB huge pages. Each device's config space is accessed by direct memory dereference at:
   ```
   virt_addr = ecam_base + (bus << 20) | (slot << 15) | (func << 12)
   ```
   This enables full 4096-byte extended config space access.

**Device Enumeration:**

```c
// Scan all 256 buses × 32 slots × 8 functions
for (bus = 0; bus < 256; bus++)
    for (slot = 0; slot < 32; slot++)
        for (func = 0; func < 8; func++)
            if (pci_device_exists(bus, slot, func))
                // multi-function detection via Header Type bit 7
```

A device is present if its Vendor ID register reads a value other than `0xFFFF`. Multi-function devices are detected when bit 7 of the header type register is set; otherwise only function 0 is probed.

**Device Matching (modalias):**

Each discovered PCI device generates a modalias string in the standard format:
```
pci:vXXXXdXXXXsvXXXXsdXXXXbcXXccXX
```
Where:
- `v` = vendor ID, `d` = device ID
- `sv` = subsystem vendor, `sd` = subsystem device
- `bc` = base class, `cc` = subclass

The modalias is used for **deferred autoprobe**: during early boot (when interrupts may be disabled and the module loader needs a preemptible context), devices are queued in a static array (`g_autoprobe_queue[PCI_AUTOPROBE_MAX_ENTRIES]`). Later, `pci_autoprobe_work()` runs in a workqueue context and calls `request_module()` for each queued modalias, triggering module autoloading.

**Capability List Traversal:**

```c
/* Standard capabilities (offset 0x34 pointer) */
for (cap = pci_read8(bus, slot, func, 0x34);
     cap != 0 && iterations < PCI_CAP_MAX_ITERATIONS;
     cap = next_cap)

/* PCIe extended capabilities (offset 0x100 range) */
for (ecap = 0x100;
     ecap != 0 && iterations < PCI_EXT_CAP_MAX_ITERATIONS;
     ecap = next_ecap)
```

Capability IDs include: MSI (0x05), MSI-X (0x11), PCIe (0x10), PM (0x01), LTR (0x18), ACS (0x0D), and others.

**Key PCI Driver API:**

| Function | Purpose |
|----------|---------|
| `pci_read16/32(bus, slot, func, offset)` | Read PCI config space (safe: returns 0xFFFF if no device) |
| `pci_write16/32(bus, slot, func, offset, val)` | Write PCI config space |
| `pci_find_device(vendor, device, out)` | Scan for a specific vendor:device |
| `pci_find_class(cls, sub, out)` | Scan for a class/subclass |
| `pci_read_bar(bus, slot, func, bar_index, out_val)` | Read Base Address Register |
| `pci_enable_bus_master(dev)` | Set bus master bit (DMA enable) |
| `pci_enable_msi(dev, vector)` | Configure MSI capability |
| `pci_enable_msix(dev, entries, nvec)` | Configure MSI-X vectors |
| `pci_find_cap(dev, cap_id)` | Find capability by ID |
| `pci_find_ext_cap(dev, cap_id)` | Find extended capability |
| `pci_find_pcie_cap(dev)` | Find PCIe capability |
| `pci_find_acs_cap(dev)` | Find ACS capability |

### Block Device Drivers

Block devices register via the block device layer (`src/include/blockdev.h`). Each driver provides read/write operations on a numbered device:

```c
struct block_device_ops {
    int (*read)(uint64_t lba, uint8_t *buffer, uint32_t sectors);
    int (*write)(uint64_t lba, const uint8_t *buffer, uint32_t sectors);
};

int blockdev_register(int dev_id, const char *name,
                      uint64_t num_sectors, uint32_t sector_size,
                      struct block_device_ops *ops);
```

Supported block device drivers:

| Driver | Source | Transport | Features |
|--------|--------|-----------|----------|
| ATA PIO | `src/drivers/ata.c` | Legacy IDE | PIO mode, LBA28/48, IRQ-driven |
| AHCI | `src/drivers/ahci.c` | PCI (SATA) | NCQ, multi-port, MSI |
| NVMe | `src/drivers/nvme.c` | PCIe | Queue pairs, PRP/SGL, MSI-X |
| virtio-blk | `src/drivers/virtio_blk.c` | virtio PCI | Multi-queue, indirect descs |
| Ramdisk | `src/drivers/ramdisk.c` | Memory | Static initramfs, dynamic ramdisks |
| Loop | `src/drivers/loop.c` | File-backed | Backed by regular file, offset support |
| Device-mapper | `src/drivers/dm-*.c` | Stackable | Linear, zero, error, crypt, verity, raid, snapshot, era |
| Multipath | `src/drivers/mpath.c` | Stackable | Path failover, I/O policy |
| iSCSI | `src/drivers/iscsi.c` | Network | Full session mgmt, CHAP auth, MC/S |
| NVMe-oF | `src/drivers/nvmf.c` | RDMA/TCP | Queue pairs, namespace export |
| FCoE | `src/drivers/fcoe.c` | Ethernet | FIP login, FC-2 framing |
| DRBD | `src/drivers/drbd.c` | Network | Sync/async replication, dual-primary |
| NBD | `src/drivers/nbd.c` | Network | Network block device protocol |
| Ceph RBD | `src/drivers/rbd.c` | Network | CRUSH placement, snapshots |
| bcache | `src/drivers/bcache.c` | Caching | SSD caching for HDD |

### Network Device Drivers

Network drivers register via the netdevice layer (`src/net/netdevice.c`):

```c
struct net_device {
    char name[IFNAMSIZ];
    uint8_t mac[6];
    int (*transmit)(struct net_device *dev, struct sk_buff *skb);
    int (*open)(struct net_device *dev);
    int (*close)(struct net_device *dev);
    // ...
};

int netif_register(struct net_device *dev);
```

Supported NIC drivers:

| Driver | Source | Features |
|--------|--------|----------|
| e1000 | `src/drivers/e1000.c` | MSI-X multi-queue, RSS, interrupt moderation (ITR), NAPI polling |
| virtio-net | `src/drivers/virtio_net.c` | Multi-queue, indirect descriptors, checksum offload |
| loopback | `src/net/loopback.c` | Internal loopback |
| TUN/TAP | `src/net/tun.c` | Userspace packet injection |
| veth | `src/net/veth.c` | Virtual Ethernet pair for net namespaces |
| bonding | `src/drivers/bonding.c` | 802.3ad (LACP), balance-xor, active-backup |
| vmxnet3 | `src/drivers/vmxnet3.c` | VMware VMXNET3, multi-queue |

Network features: RPS/RFS flow steering (`src/net/rps.c`), NAPI polling, multi-queue RSS, XDP fast path, checksum offload.

### USB Subsystem (`src/drivers/usb_core.c`)

USB uses a host-controller-centric model with device enumeration on the root hub:

```c
int usb_init(void);           // Initialize USB HCIs (EHCI/XHCI)
int usb_hub_init(void);       // Hub driver for port enumeration
int usb_msc_init(void);       // Mass Storage Class driver
int usb_hid_init(void);       // Human Interface Device driver
```

Key layers:
- **Host Controller**: EHCI (USB 2.0) and XHCI (USB 3.x) drivers manage transfer rings and port routing
- **Hub driver**: Detects connect/disconnect events, manages power switching
- **Device drivers**: Match against interface class/subclass/protocol (MSC, HID, CDC ACM, UAS, serial, wifi)

USB transfer types supported: control, bulk, interrupt, isochronous.

### Virtio Family

The virtio family of drivers uses a shared transport layer (`src/drivers/virtio_pci_modern.c`) with device-specific frontends:

| Driver | Source | Purpose |
|--------|--------|---------|
| virtio-net | `src/drivers/virtio_net.c` | Network interface |
| virtio-blk | `src/drivers/virtio_blk.c` | Block device |
| virtio-scsi | `src/drivers/virtio_scsi.c` | SCSI controller |
| virtio-gpu | `src/drivers/virtio_gpu.c` | 2D/3D graphics |
| virtio-input | `src/drivers/virtio_input.c` | Keyboard/mouse |
| virtio-console | `src/drivers/virtio_console.c` | Serial console |
| virtio-rng | `src/drivers/virtio_rng.c` | Entropy source |
| virtio-fs | `src/drivers/virtio_fs.c` | Shared filesystem (FUSE) |
| virtio-iommu | `src/drivers/virtio_iommu.c` | IOMMU |
| balloon | `src/drivers/balloon.c` | Memory balloon |

Transport layer handles: virtqueue negotiation, feature bit negotiation, MSI-X vector assignment, modern/legacy interface detection.

### DRM / GPU Drivers (`src/drivers/drm/`)

The DRM (Direct Rendering Manager) subsystem provides a unified interface for graphics output:

| Driver | Source | Description |
|--------|--------|-------------|
| DRM core | `src/drivers/drm/drm_core.c` | Mode setting, connector management, framebuffer, CRTC |
| bochs-drm | `src/drivers/drm/bochs_drm.c` | QEMU Bochs VGA (standard QEMU display) |
| simplefb | `src/drivers/drm/simplefb.c` | Simple framebuffer (early boot console handoff) |
| intel-gpu | `src/drivers/intel_gpu.c` | Intel integrated GPU (i915-like, native mode setting) |
| DRM atomic | `src/drivers/drm/drm_atomic.c` | Atomic modeset/plane update |

### Audio Drivers (`src/drivers/`)

| Driver | Source | Description |
|--------|--------|-------------|
| AC97 | `src/drivers/ac97.c` | AC97 audio codec (PCI, I/O port based) |
| Sound core | `src/drivers/sound_core.c` | /dev/dsp and /dev/mixer interface |
| OSS | `src/drivers/sound_oss.c` | Open Sound System compatibility |
| FM synth | `src/drivers/fm_synth.c` | FM synthesis (OPL2/OPL3) |
| MIDI | `src/drivers/sound_midi.c` | MIDI UART interface |
| PC speaker | `src/drivers/speaker.c` | PC timer-based beep |

### Misc / Platform Drivers

| Driver | Source | Description |
|--------|--------|-------------|
| TPM TIS | `src/drivers/tpm_tis.c` | TPM 2.0 TIS FIFO interface |
| IPMI KCS | `src/drivers/ipmi_kcs.c` | IPMI Keyboard Controller Style |
| Watchdog | `src/drivers/watchdog.c` | WDT timer reset |
| I6300ESB | `src/drivers/i6300esb.c` | Intel 6300ESB watchdog |
| HPET | `src/drivers/hpet.c` | High Precision Event Timer |
| I2C | `src/drivers/i2c.c` | I2C bus master/slave |
| SMBUS | `src/drivers/smbus.c` | System Management Bus |
| RTC/CMOS | `src/drivers/cmos.c` | MC146818 RTC via CMOS |
| ACPI | `src/drivers/acpi.c` | ACPI table parsing, EC, thermal |
| Serial | `src/drivers/serial.c` | UART 16550 (COM1-4) |
| Keyboard | `src/drivers/keyboard.c` | PS/2 keyboard (8042) |
| Mouse | `src/drivers/mouse.c` | PS/2 mouse |
| VGA | `src/drivers/vga.c` | VGA text/framebuffer modes |
| EDAC | `src/drivers/edac.c` | Error Detection And Correction |
| SPI | `src/drivers/spi.c` | Serial Peripheral Interface |
| GPIO IRQ | `src/drivers/gpio_irq.c` | GPIO interrupt controller |
| PVpanic | `src/drivers/pvpanic.c` | QEMU panic notification device |
| VMware balloon | `src/drivers/vmw_balloon.c` | VMware memory balloon |

### Loadable Module Drivers

226 kernel modules (`.ko` files) are built from `obj-m` entries across the source tree and bundled in initramfs. The module build system (`src/modules/drivers.mk`) organizes drivers into categories:

```
Core/basic     — e1000, speaker, coredump, floppy, virtio, NVMe, TPM, AHCI, USB
Device mapper  — dm-*, ramdisk, loop, bcache, mpath
Virtio family  — virtio_*, vhost_*, vfio, vdpa
Graphics       — intel_gpu, bochs, simplefb
Audio          — ac97, sound_*, fm_synth
PCIe           — pcie_aer, pcie_dpc, pcie_ptm, sriov
USB sub-modules — usb_uas, usb_serial, usb_cdc_ether, usb_wifi
Misc           — firmware_class, dma-api, i2c, smbus, watchdog, rtc, cmos, battery
```

Module loading flow:
```
insormod → syscall → module loader → ELF parse → RELA relocation → 
EXPORT_SYMBOL resolution → RSA-2048/SHA-256 signature verification →
module_init() call
```

See the **Kernel Modules** section below for more detail.

### Driver Source Files

All driver source code lives under `src/drivers/`, organized by protocol/hardware:

```
src/drivers/
├── pci.c              — PCI/PCIe config space access, enumeration
├── ata.c              — ATA PIO mode
├── ahci.c             — AHCI SATA (NCQ)
├── nvme.c             — NVMe SSD
├── e1000.c            — Intel PRO/1000 NIC
├── virtio_net.c       — Virtio network
├── virtio_blk.c       — Virtio block
├── usb_core.c         — USB host controller
├── usb_ehci.c         — EHCI USB 2.0
├── usb_xhci.c         — xHCI USB 3.x
├── drm/               — DRM core + GPU drivers
│   ├── drm_core.c
│   ├── bochs_drm.c
│   ├── simplefb_drm.c
│   └── drm_atomic.c
├── ac97.c             — AC97 audio
├── cmos.c             — RTC/CMOS
├── acpi.c             — ACPI tables
├── watchdog.c         — Watchdog timer
├── tpm_tis.c          — TPM 2.0
├── iscsi.c            — iSCSI initiator
├── nvmf.c             — NVMe-over-Fabrics
├── fcoe.c             — Fibre Channel over Ethernet
├── drbd.c             — Distributed Replicated Block Device
├── bonding.c          — NIC bonding
├── dm-crypt.c         — Device mapper crypto
├── dm-verity.c        — Device mapper integrity
└── ... (50+ additional driver files)
```

### Driver Initialization Summary

| Method | When | Used by |
|--------|------|---------|
| `kernel_main()` direct call | Phases 1–17 (ordered) | Core drivers (serial, VGA, timer, keyboard, ACPI, ATA, PCI, USB, network) |
| `device_initcall()` | After phase 9 (do_initcalls) | Additional built-in drivers, diagnostic modules |
| `module_init()` (module) | On `insmod`/`modprobe` | 226 loadable `.ko` drivers |
| PCI deferred autoprobe | Workqueue context | PCI driver autoloading by modalias |

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

A layered view of the VFS architecture:

```
┌────────────────────────────────────────────────────────────┐
│ Syscall Layer  (sys_open/sys_read/sys_write/sys_mount/...) │
│ syscalls.c, syscall_new.c                                  │
└──────────────────────────┬─────────────────────────────────┘
                           │ fd / inode translation
┌──────────────────────────▼─────────────────────────────────┐
│ VFS Core  (src/kernel/vfs.c)                               │
│   vfs_open/read/write/close/stat/create/unlink/rename/...  │
│   path resolution → resolve() walks mount table + dcache   │
│   mount/umount mgmt, mount namespaces, POSIX locks,        │
│   xattr table, file locks, quotas                          │
└──────────────────────────┬─────────────────────────────────┘
                           │ dispatches on vfs_mount.ops
┌──────────────────────────▼─────────────────────────────────┐
│ Per-Filesystem vfs_ops Dispatch Table                      │
│   read/write/stat/create/unlink/readdir + optional ops     │
│   (truncate/fallocate/dedup/resize/journal/link/symlink/   │
│    mknod/rename/tmpfile/ioctl/seek/xattr)                  │
└──────────────────────────┬─────────────────────────────────┘
                           │ priv → driver instance
┌──────────────────────────▼─────────────────────────────────┐
│ Filesystem Drivers                                        │
│  tmpfs  fat32  ext2  ext4  btrfs  ntfs  cifs  nfs  ...     │
└──────────────────────────┬─────────────────────────────────┘
                           │ block I/O
┌──────────────────────────▼─────────────────────────────────┐
│ Block Layer + Cache  (blockdev, blk_mq, page_cache, bcache)│
└────────────────────────────────────────────────────────────┘
```

Each `struct vfs_mount` binds one `vfs_ops` table and a private driver
instance to a mountpoint; `resolve()` picks the mount whose mountpoint
is the longest matching prefix of the requested path, then calls the
corresponding op with the mount's `priv`. Per-process mount namespaces
(`CLONE_NEWNS`) swap in a private copy of the mount table so containers
see an isolated view.

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

### VFS Operations and Data Structures

The VFS layer is defined by three primary data structures that together provide a complete filesystem abstraction:

#### struct vfs_ops — Filesystem Operations Dispatch Table

**Header:** `src/include/vfs.h`

Each mounted filesystem provides a `const struct vfs_ops` table implementing all VFS operations for that filesystem. This combines the roles of Linux's `super_operations`, `inode_operations`, and `file_operations` into a single per-filesystem dispatch table.

**Required operations (all filesystems must implement):**
- `read(priv, path, buf, max_size, *out_size)` — Read file contents into buffer, returns byte count or negative errno
- `write(priv, path, data, size)` — Write data to file, returns 0 or negative errno
- `stat(priv, path, *st)` — Get file metadata into `struct vfs_stat`, returns 0 or negative errno
- `create(priv, path, type)` — Create a new file/directory, returns 0 or negative errno
- `unlink(priv, path)` — Remove a file, returns 0 or negative errno
- `readdir(priv, path)` — List directory entries via `kprintf`, returns 0 or negative errno

**Optional operations (may be NULL; VFS applies sensible defaults or returns `-ENOTTY`):**
- `readdir_names(priv, path, names[], max)` — Get directory entry names as an array
- `truncate(priv, path, len)` — Truncate/extend file to given length
- `fallocate(priv, path, mode, offset, len)` — Pre-allocate disk space
- `dedup(priv, path1, path2)` — File deduplication (find and share identical blocks)
- `resize(priv, new_block_count)` — Resize the filesystem
- `journal_start/commit/abort(priv)` — Journal transaction management
- `link(priv, oldpath, newpath)` — Create a hard link
- `symlink(priv, target, linkpath)` — Create a symbolic link
- `readlink(priv, path, buf, bufsize)` — Read symlink target
- `mknod(priv, path, mode, dev_major, dev_minor)` — Create a device node
- `flush(priv)` — Flush cached data to backing store
- `set_time(priv, path, atime, mtime)` — Set file timestamps (with UTIME_NOW/UTIME_OMIT semantics)
- `rename(priv, old_path, new_path)` — Rename/move within the same filesystem
- `tmpfile(priv, mode)` — Create unnamed temporary file (O_TMPFILE)
- `ioctl(priv, path, cmd, arg)` — Device-specific file operations
- `seek(priv, path, offset, whence)` — Seek to data/hole boundary in sparse files
- `setxattr/getxattr/listxattr/removexattr(priv, path, ...)` — Extended attribute operations

The `<priv>` pointer from `struct vfs_mount` is passed as the first argument to every callback, allowing the filesystem driver to identify its instance state without global state.

#### struct vfs_mount — Mounted Filesystem Instance

**Header:** `src/include/vfs.h`

Represents a single mounted filesystem, binding a mountpoint path to a specific `vfs_ops` implementation and its private driver data. Analogous to Linux's `struct super_block` + `struct mount` combined.

```c
struct vfs_mount {
    char          mountpoint[64];      /* e.g. "/", "/mnt", "/boot" */
    const struct vfs_ops *ops;         /* per-filesystem dispatch table */
    void          *priv;               /* filesystem driver private data */
    int           flags;               /* MS_RDONLY, MS_BIND, etc. */
    char          bind_source[64];     /* for bind mounts: source path */
    int           is_bind;             /* 1 if this is a bind mount */
    int           journal_active;      /* 1 if journal transaction in progress */
    uint32_t      journal_seq;         /* transaction sequence number */
    int           encrypted;           /* 1 if per-mount encryption enabled */
    uint8_t       enc_key[16];         /* AES-128 encryption key */
};
```

**Lifecycle:**
- Created by `vfs_mount(path, ops, priv)` or `vfs_mount_ex(path, ops, priv, flags)`
- The mount table (`mounts[]`, up to `VFS_MAX_MOUNTS=16` entries) is protected by a spinlock (`mount_lock` in `vfs.c`)
- Removed by `vfs_umount(path)`, which checks for busy filesystems first (`vfs_umount_check_busy`)
- Per-process mount namespaces (via `CLONE_NEWNS`) each hold a private copy of the mount table

#### struct vfs_filesystem_type — Registered Filesystem Type

**Header:** `src/include/vfs.h`

Each filesystem driver calls `vfs_register_filesystem(name, ops)` at init time to make its type available. Entries are enumerated via `/proc/filesystems` for userspace inspection.

```c
struct vfs_filesystem_type {
    char name[32];                /* "ext2", "fat32", "tmpfs", etc. */
    const struct vfs_ops *ops;    /* default ops for this type */
    int registered;               /* 1 = type is registered */
};
```

### Path Resolution Algorithm

The core of the VFS is the path resolution algorithm, which translates a user-supplied path to a specific filesystem operation:

```
User path (may be relative)
  │
  ▼
vfs_abs_path() — convert relative to absolute
  ┌─────────────────────┐
  │ CWD-based conversion │  (uses current process's working directory)
  └─────────────────────┘
  │
  ▼
vfs_resolve_mount() — find best-matching mount entry
  ┌──────────────────────────────────────────────┐
  │ Walks mounts[] array, longest-prefix match   │
  │ e.g., path="/mnt/data/file.txt" matches     │
  │ mountpoint="/mnt" → subpath="/data/file.txt" │
  │ mountpoint="/"   → subpath="/mnt/data/..."   │
  └──────────────────────────────────────────────┘
  │
  ▼
vfs_ops->read(priv, subpath, ...) — dispatch to FS
  ┌──────────────────────────────────────┐
  │ FS operates relative to its own root  │
  │ No knowledge of global mount hierarchy│
  └──────────────────────────────────────┘
```

The longest-prefix match ensures that the most specific mount entry handles the path. This allows nested mounts (e.g., `/` at ext2 root, `/home` at tmpfs, `/mnt/usb` at FAT32) to coexist and function correctly.

### Dentry Cache (dcache)

**Files:** `src/include/dcache.h`, `src/kernel/vfs.c`

The dcache is a fixed-size array (`DCACHE_SIZE=128` entries) caching resolved path metadata to avoid repeated filesystem accesses for `stat()` and related calls.

```c
struct dcache_entry {
    char  path[DCACHE_PATH_LEN];   /* absolute path key (128 bytes) */
    void *mount;                    /* mount pointer for bulk invalidate */
    uint8_t  type;                  /* 1=file, 2=dir, 3=link */
    uint32_t size;
    uint16_t uid;
    uint16_t gid;
    uint16_t mode;
    uint32_t mtime;
    uint32_t atime;
    uint32_t nlink;
    uint32_t ino;                  /* inode number */
    uint16_t dev_major;
    uint16_t dev_minor;
    uint32_t last_tick;            /* LRU timestamp for eviction */
    int      in_use;               /* 1 = slot occupied */
};
```

**Cache operations:**
- `dcache_lookup(path, *st)` — Returns 0 on hit (data copied under lock), -1 on miss
- `dcache_add(path, mount, type, size, ...)` — Insert or update; evicts LRU entry if full
- `dcache_remove(path)` — Remove entry by path (on file deletion)
- `dcache_remove_mount(mount)` — Invalidate all entries for a mount (on umount)

**Eviction policy:** LRU (Least Recently Used) via a global monotonic tick counter. `dcache_shrink(n)` evicts up to `n` entries under memory pressure, called from the OOM handler. `dcache_evict_one()` removes the single oldest entry.

**Locking:** All dcache operations are protected by `dcache_lock` (spinlock) for SMP safety. The lookup function copies the result while holding the lock, so a successful lookup's data cannot be invalidated between lookup and use.

### Mount Namespace and Bind Mounts

#### Mount Namespaces

**Files:** `src/kernel/mnt_namespace.c`, `src/include/mnt_namespace.h`

Per-process mount namespaces provide isolated mount table views, typically used for containerization:

```c
struct mnt_namespace {
    int              refcount;
    struct vfs_mount mounts[VFS_MAX_MOUNTS];  /* per-ns mount table */
    int              num_mounts;
};
```

- **Root namespace** wraps the global mount table. Created once during `vfs_init()`.
- **CLONE_NEWNS** (via `unshare()` or `clone()`) creates a new namespace by deep-copying the parent's mount table.
- **Propagation types** (implemented in `src/kernel/fs_mount_prop.c`):
  - `SHARED` — mount events propagate to all peers in the namespace group
  - `SLAVE` — receives propagation events but does not send them
  - `PRIVATE` — fully isolated, no propagation in either direction

#### Bind Mounts

Bind mounts make a directory subtree visible at multiple locations in the mount hierarchy:

- `vfs_bind_mount(src, target)` — Attach `src` tree at `target` path
- `vfs_bind_mount_recursive(src, target)` — Recursive subtree bind (for mount namespaces)
- `vfs_is_bind_mount(path)` — Check if a path is a bind mount
- `vfs_bind_source(path)` — Get the original source path of a bind mount

Bind mounts are tracked via the `is_bind` field and `bind_source` path in `struct vfs_mount`. They share the same `ops` and `priv` as the original mount.

### File Locking, Extended Attributes, and POSIX ACLs

#### File Locking

**Header:** `src/include/vfs.h`

POSIX advisory file locks (`fcntl/F_SETLK`, `F_SETLKW`, `F_GETLK`) provide cooperative inter-process file synchronization:

```c
struct file_lock {
    int      l_type;          /* F_RDLCK, F_WRLCK, F_UNLCK */
    int      l_whence;        /* SEEK_SET, SEEK_CUR, SEEK_END */
    int64_t  l_start;         /* offset relative to whence */
    int64_t  l_len;           /* 0 = to EOF */
    int32_t  l_pid;           /* owning process PID */
    int      used;
    int      mandatory;       /* 1 = kernel-enforced mandatory lock */
    char     path_storage[64]; /* path the lock applies to */
};
```

**Conflict rules:**
- Multiple read locks (F_RDLCK) on the same range are compatible
- A write lock (F_WRLCK) conflicts with all other locks on the same range
- Locks are automatically released when the file descriptor is closed
- `vfs_setlk(path, flk, wait)` — Acquire/check/release a lock (wait=1 for F_SETLKW blocking)
- `vfs_getlk(path, flk)` — Test if a conflicting lock exists, returns info about the blocker

#### Extended Attributes (xattr)

Extended attributes provide a mechanism for associating metadata with files outside the standard stat structure. The kernel supports the `user.` namespace:

```c
struct xattr_entry {
    char  name[VFS_XATTR_NAME_MAX];    /* attr name (max 16 bytes) */
    char  value[VFS_XATTR_VALUE_MAX];  /* attr value (max 64 bytes) */
    int   size;                         /* actual value length */
    int   in_use;
};
```

**Limits:** Up to `VFS_XATTR_PER_INODE=4` extended attributes per file.

**Operations:** `vfs_setxattr()`, `vfs_getxattr()`, `vfs_listxattr()`, `vfs_removexattr()`.

Filesystems may override xattr handling by providing `setxattr`/`getxattr`/`listxattr`/`removexattr` in their `vfs_ops` table. If not provided, the VFS falls back to the global path-based xattr table.

#### POSIX ACLs

POSIX Access Control Lists extend the traditional UNIX permission model with arbitrary user and group entries:

```c
struct posix_acl_entry {
    uint16_t tag;   /* ACL_USER_OBJ, ACL_USER, ACL_GROUP_OBJ, ACL_GROUP, ACL_MASK, ACL_OTHER */
    uint16_t perm;  /* permission bits (r/w/x combination) */
    uint32_t id;    /* user/group ID (for ACL_USER/ACL_GROUP entries) */
};

struct posix_acl {
    struct posix_acl_entry entries[POSIX_ACL_MAX_ENTRIES];  /* max 3 entries */
    int count;                                               /* actual entry count */
};
```

**Access check order:**
1. If the process's UID matches the file owner, use `ACL_USER_OBJ` (owner permissions)
2. If the process's UID matches a named `ACL_USER` entry, use that entry (masked by `ACL_MASK`)
3. If the process's GID matches the file group or a named `ACL_GROUP`, use the matching entry (masked by `ACL_MASK`)
4. Otherwise, use `ACL_OTHER`
5. If no ACL exists, fall back to traditional mode bits

### VFS Permission Model

The permission model uses a layered approach, checked in order:

1. **DAC — UNIX mode bits:** Standard `rwx` permissions for owner/group/other, checked by `generic_permission()`.
2. **POSIX ACL:** If a file has an ACL, it is consulted first as described above.
3. **Landlock MAC:** Path-based Mandatory Access Control. During `vfs_open()`, `landlock_check_path()` verifies the requested access against the process's Landlock ruleset.
4. **Capability checks:** Privileged operations (mount, unmount, `pivot_root`) require `CAP_SYS_ADMIN`.

The `vfs_check_perms(path, uid, gid, op)` function provides a unified entry point that chains these checks appropriately for the requested operation (`VFS_R_OK=4` for read, `VFS_W_OK=2` for write, `VFS_X_OK=1` for execute, `VFS_F_OK=0` for existence).

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

#### Packet Data Flow

The receive and transmit paths below are grounded in the actual dispatch
code (`src/net/net.c`, `src/net/socket.c`, `src/net/net_tcp.c`).

```
RECEIVE (RX)                                           TRANSMIT (TX)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━            ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
NIC IRQ (e1000 / virtio-net / ...)                   Application (socket API)
    │                                                     │
    ▼                                                     ▼
net_rx_signal()  (waitqueue wake)                     handle_tcp() / handle_udp()
    │              ──────────────                       (net_tcp.c / net_udp.c)
    ▼                                                     │
net_poll()  (softirq drain, non-blocking)              send_ip() / send_ip_with_ttl()
    │              ──────────────                        │
    ▼                                                     ▼
net_link_recv()  (copies frame, net_lock)              netfilter  LOCAL_OUT  hook
    │              ──────────────                        │
    ▼                                                     ▼
(optional RPS/RFS flow-hash remap across CPUs)         conntrack (state tracking)
    │                                                     │
    ▼                                                     ▼
net_rx_dispatch()                                       netfilter  POST_ROUTING  hook
    │  ── XDP hook (DROP / TX / PASS)                     │
    ▼                                                     ▼
netfilter  PREROUTING  hook                              loopback check?  ──yes──▶ lo
    │                                                     │no
    ▼                                                     ▼
ARP (net.c) / IPv4 (handle_ip) / IPv6 (ipv6.c)         send_eth() (build eth header)
    │                                                     │
    ▼                                                     ▼
netfilter  LOCAL_IN  hook                              net_link_send()
    │                                                     │
    ▼                                                     ▼
handle_ip()  ── demux by protocol ──┐                  netdevice queue → NIC driver
    │                               │
    ├──▶ handle_icmp()        ┌─────┴──────┐
    ├──▶ handle_tcp()         │ forwarded   │
    ├──▶ handle_udp()         │ (net_ip_    │
    └──▶ handle_sctp()        │  forwarding)│
                             └────────────┘
    │
    ▼
socket.c dispatch → recv queue → user recv()
```

Every read/write of shared state (routing table, ARP cache, TCP
connections, UDP bindings/listeners) is guarded by `net_lock`
(`spinlock_t`). `net_poll()` is non-blocking and must not sleep,
so RX dispatch is driven from softirq/tasklet context while the
worker/softirq thread (`ksoftirqd`) handles the deferred work.

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

### Socket Buffer & Packet Data Path

The network stack processes packets through a layered pipeline from driver interrupt to application socket. Unlike Linux's `struct sk_buff`, this kernel uses fixed per-connection receive buffers (typically 1-4 KB) allocated during connection setup — packets are copied into socket receive queues during `net_poll()`, and the application reads from these queues via the socket API.

**Receive path (IRQ → application):**

```
NIC IRQ → net_rx_signal() → net_poll() → net_link_recv()
  → net_rx_dispatch()          [ethertype demux: ARP / IPv4 / IPv6]
    → nf_hook_traverse()       [netfilter PRE_ROUTING]
      → handle_ip()             [IP reassembly → protocol demux]
        → nf_hook_traverse()   [netfilter LOCAL_IN]
          → handle_tcp() / handle_udp() / handle_icmp()
            → skb_enqueue()     [socket receive buffer]
              → wake up application (poll/select)
```

**Transmit path (application → NIC):**

```
socket send()/sendto()
  → send_ip()                   [build IP header, fragment if needed]
    → nf_hook_traverse()       [netfilter LOCAL_OUT]
      → conntrack lookup
        → nf_hook_traverse()   [netfilter POST_ROUTING]
          → arp_resolve_or_queue() [MAC resolution]
            → send_eth()        [build Ethernet header]
              → net_link_send() [qdisc enqueue → driver dequeue]
                → NIC transmit
```

**Concurrent packet processing** is achieved through:
- **NAPI polling** — drivers batch packets during a single poll cycle, reducing interrupt overhead
- **RPS/RFS** (`src/net/rps.c`): Receive Packet Steering distributes flows across CPUs by hash; Receive Flow Steering tracks flow-to-CPU affinity for cache locality
- **XDP** (`src/net/xdp.c`): BPF-based early processing at the driver level before any kernel protocol parsing — supports `XDP_DROP`, `XDP_PASS`, `XDP_TX` actions
- **Per-connection locking** — each TCP/UDP connection has its own spinlock for socket buffer access

**Buffer management:** The kernel does not use a shared socket buffer allocator. Instead:
- Each TCP connection has a fixed receive ring (up to 16 segments × MSS) and a send buffer
- UDP uses per-slot datagram queues (up to 16 pending datagrams per socket)
- Raw sockets (AF_PACKET, AF_UNIX) use simple FIFO queues with configurable limits
- Driver-level DMA buffers are pre-allocated by each NIC driver during probe

### Traffic Control & QoS

**Files:** `src/net/pkt_sched.c`, `src/net/fq_codel.c`, `src/net/cake.c`, `src/net/sch_fq.c`, `src/net/sch_red.c`, `src/net/sch_tbf.c`, `src/include/pkt_sched.h`

This kernel implements a Linux-compatible qdisc (queueing discipline) framework with support for multiple schedulers, each attachable to a specific interface:

| Qdisc | File | Type | Algorithm |
|-------|------|------|-----------|
| **pfifo_fast** | `pkt_sched.c` | Classless | 3-band priority FIFO (ICMP→0, TCP→1, bulk→2) |
| **fq_codel** | `fq_codel.c` | Classless | Fair Queuing + Controlled Delay (per-flow queues + CoDel AQM) |
| **cake** | `cake.c` | Classless | Common Applications Kept Enhanced — bandwidth shaping + AQM + 8 TINs |
| **FQ** | `sch_fq.c` | Classless | Fair Queue with per-flow Deficit Round Robin + pacing |
| **RED** | `sch_red.c` | Classless | Random Early Detection — probabilistic AQM with ECN marking |
| **TBF** | `sch_tbf.c` | Classless | Token Bucket Filter — rate limiting with burst support |
| **HTB** | `pkt_sched.c` | Classful | Hierarchical Token Bucket — multi-level bandwidth hierarchy |

**Architecture:**

```
Application sends packet → net_link_send()
  → tc_get_qdisc(dev) → qdisc->enqueue()
    → qdisc->dequeue()  [driven by NIC transmit or watchdog timer]
      → netif_send(ifindex, data, len)
        → NIC driver transmit callback
```

Each qdisc implements the standard operations: `enqueue()`, `dequeue()`, `drop()`, `get_stats()`. Classful qdiscs (HTB) additionally support per-class statistics and hierarchical bandwidth partitioning.

**pfifo_fast** is the default qdisc (applied at init) — it provides simple priority queuing with 3 bands. Band 0 (highest) carries ICMP, band 1 carries TCP control/ACK packets, and band 2 carries bulk data. This prevents starvation of latency-sensitive traffic.

**fq_codel** applies fair queuing across flows (hash-based flow classification) with CoDel AQM to control latency under load. Each flow gets a separate FIFO queue; the CoDel algorithm drops packets from queues with excessive sojourn time (>5 ms target).

**CAKE** (`src/net/cake.c`) is a comprehensive shaper/AQM that combines bandwidth shaping, per-flow queuing, and ECN marking into a single qdisc. It classifies traffic into 8 TINs (tins) by DSCP marking and applies per-tin bandwidth limits, drop/mark thresholds, and priority levels.

**HTB** enables hierarchical bandwidth allocation — a root class distributes bandwidth among child classes according to configured rates and ceilings, supporting complex traffic shaping topologies (e.g., per-customer rate limits with burst allowance).

### Network Namespaces

**Files:** `src/net/net_ns.c`, `src/include/net_ns.h`

Network namespaces provide per-isolation-domain network state, modeled after Linux `CLONE_NEWNET`. Each namespace has its own:

```c
struct net_ns {
    int               id;                  /* namespace ID (0 = init) */
    char              name[32];            /* human-readable name */
    uint32_t          ip_addr;             /* per-ns IPv4 address */
    uint32_t          gateway;             /* per-ns default gateway */
    uint32_t          subnet_mask;         /* per-ns subnet mask */
    uint32_t          dns_server;          /* per-ns DNS resolver */
    uint8_t           mac[6];              /* per-ns MAC address */
    int               num_ifaces;          /* interfaces in this namespace */
    int               iface_ids[NET_NS_MAX_IFACES];
    struct rt_entry   rt_table[NET_NS_RT_MAX];   /* per-ns routing table */
    int               rt_num_entries;
    struct nf_rule    nf_rules[NET_NS_NF_MAX];    /* per-ns netfilter rules */
    int               nf_num_rules;
    int               in_use;
};
```

**API:**

- `net_ns_create(name)` — create a new empty namespace (up to `NET_NS_MAX` total)
- `net_ns_destroy(ns_id)` — tear down a namespace (cannot destroy init_ns)
- `net_ns_set_current(ns_id)` — switch the current thread's network namespace
- `net_ns_get_current()` — return the current namespace pointer
- `net_ns_add_iface(ns_id, ifindex)` — move an interface between namespaces
- `net_ns_remove_iface(ns_id, ifindex)` — detach an interface

**Isolation guarantees:**
- Each namespace has its own IP address, MAC, routing table, and netfilter rules
- Interfaces can be moved between namespaces via `net_ns_add_iface()` (removes from old, adds to new)
- The init namespace (ID 0) is created at boot and always present
- Namespace operations are guarded by `net_ns_lock` (spinlock)
- Used by the container runtime for per-container network isolation

**Integration with containers:**
```
container_create() → net_ns_create("c1")
  → net_ns_add_iface(ns1, veth_peer)  // veth pair half in container
  → configure IP, routes, and netfilter inside ns1
  → attach process to namespace via net_ns_set_current()
```

### Network Source File Layout

All networking source files live under `src/net/` (93 files organized by layer):

```
src/net/
├── Core packet processing
│   ├── net.c              — Link/network layer core, ARP, IP, ICMP, routing
│   ├── net_ext.c          — Extended network operations
│   └── netdevice.c        — Netdevice registration and dispatch
│
├── Transport protocols
│   ├── net_tcp.c          — TCP state machine, connection table, retransmit
│   ├── net_udp.c          — UDP datagram dispatch and bindings
│   ├── sctp.c / sctp_sm.c / sctp_tsn.c  — SCTP stream transport
│   ├── dccp.c             — DCCP datagram congestion control
│   └── mptcp.c / mptcp_sched.c  — Multipath TCP subflow management
│
├── TCP congestion control (8 pluggable algorithms)
│   ├── tcp_cc.c           — Congestion control framework (pluggable)
│   ├── tcp_newreno.c      — NewReno (default)
│   ├── tcp_cubic.c        — CUBIC (high-BDP)
│   ├── tcp_bbr.c / tcp_bbr2.c / tcp_bbr3.c — BBRv1/v2/v3 (model-based)
│   ├── tcp_bic.c          — BIC (binary increase)
│   ├── tcp_vegas.c        — Vegas (delay-based)
│   ├── tcp_westwood.c     — Westwood (bandwidth-estimation)
│   ├── tcp_hybla.c        — Hybla (satellite links)
│   └── tcp_illinois.c     — Illinois (delay-window hybrid)
│
├── Network layer (IPv4, IPv6)
│   ├── ipv4 fragments     — (in net.c via handle_ip_fragment)
│   ├── ipv6.c / ipv6_core.c       — IPv6 main processing
│   ├── ipv6_ndisc.c       — Neighbor Discovery (RFC 4861)
│   ├── ipv6_mld.c         — Multicast Listener Discovery
│   ├── ipv6_pmtu.c        — Path MTU Discovery
│   └── ipv6 addressing    — (in net_internal.h, ipv6_addr_table)
│
├── Socket layer
│   ├── socket.c / socket_ext.c    — BSD socket API dispatch
│   ├── af_unix.c           — AF_UNIX domain sockets
│   ├── af_packet.c         — AF_PACKET raw packet sockets
│   ├── netlink.c           — AF_NETLINK kernel-userspace IPC
│   ├── can.c               — AF_CAN SocketCAN
│   ├── vsock.c             — AF_VSOCK VM sockets
│   └── tipc.c              — AF_TIPC cluster messaging
│
├── Security & filtering
│   ├── netfilter.c / netfilter_hooks.c  — Packet filter hooks
│   ├── nf_tables.c         — nf_tables ruleset management
│   ├── conntrack.c / conntrack_helpers.c — Connection tracking
│   ├── ipsec.c             — IPsec ESP/AH (transport + tunnel)
│   ├── pfkey.c             — PF_KEYv2 SA management
│   ├── macsec.c            — IEEE 802.1AE MAC security
│   ├── wireguard.c / wg_netlink.c  — WireGuard VPN
│   └── ktls.c              — Kernel TLS offload
│
├── Traffic control & QoS
│   ├── pkt_sched.c         — Qdisc framework + pfifo_fast + HTB
│   ├── fq_codel.c          — Fair Queuing + CoDel AQM
│   ├── cake.c              — Common Applications Kept Enhanced
│   ├── sch_fq.c            — Fair Queue with pacing
│   ├── sch_red.c           — Random Early Detection
│   └── sch_tbf.c           — Token Bucket Filter
│
├── Tunneling & virtual interfaces
│   ├── gre.c               — GRE (RFC 2784)
│   ├── ipip.c              — IPIP (RFC 2003)
│   ├── vxlan.c             — VXLAN (RFC 7348)
│   ├── l2tp.c              — L2TPv3 (RFC 3931)
│   ├── pptp.c              — PPTP (RFC 2637)
│   ├── 6lowpan.c           — 6LoWPAN header compression
│   ├── tun.c               — TUN/TAP virtual interfaces
│   └── veth.c              — veth pair virtual Ethernet
│
├── Link layer
│   ├── bridge.c            — Ethernet bridge with STP
│   ├── stp.c               — Spanning Tree Protocol (802.1D)
│   ├── garp.c / mrp.c      — Generic/Multiple Registration Protocol
│   ├── lacp.c              — Link Aggregation Control Protocol (802.3ad)
│   ├── vlan.c              — VLAN 802.1Q tag/untag
│   ├── lldp.c              — Link Layer Discovery Protocol (802.1AB)
│   ├── ipoib.c             — IP over InfiniBand
│   └── bonding             — (in src/drivers/bonding.c)
│
├── Application protocols
│   ├── dhcp.c / dhcp6.c    — DHCPv4/v6 client
│   ├── dns_cache.c / dns_resolver.c / dns_server.c — DNS resolver + server
│   ├── httpd.c             — HTTP/1.1 server
│   ├── sshd.c              — SSH server (key exchange + channel)
│   ├── telnetd.c           — Telnet server
│   ├── smtp.c              — SMTP client
│   ├── ntp.c               — NTP client
│   ├── socks5.c            — SOCKS5 proxy client
│   └── tls.c / tls_aead.c / tls_handshake.c / tls_session.c / tls_x509.c — TLS 1.3
│
├── Infrastructure
│   ├── net_ns.c            — Network namespaces
│   ├── xdp.c               — eXpress Data Path
│   ├── rps.c               — Receive Packet Steering
│   ├── openvswitch.c       — Open vSwitch data path
│   └── ipvs.c              — IP Virtual Server (load balancer)
│
└── Headers (src/include/)
    ├── net.h               — Core types and protocol constants
    ├── net_internal.h      — Internal state (ARP cache, IPv6 table, etc.)
    ├── socket.h            — Socket types, SOL_*, SO_* constants
    ├── netdevice.h         — net_device structure and registration API
    ├── netfilter.h         — Netfilter hook API
    ├── pkt_sched.h         — Qdisc operations and statistics
    ├── conntrack.h         — Connection tracking structures
    ├── af_unix.h           — AF_UNIX socket types
    ├── net_ns.h            — Network namespace API
    └── tcp_cc.h            — Congestion control pluggable framework
```

### Packet Statistics & Monitoring

**Per-interface statistics** are tracked in `net_iface_stats[]` (defined in `net.c`) and exposed via `/proc/net/dev`:

| Counter | Description |
|---------|-------------|
| `rx_packets` / `tx_packets` | Total packets received/transmitted |
| `rx_bytes` / `tx_bytes` | Total bytes received/transmitted |
| `rx_errors` / `tx_errors` | Hardware/driver errors |
| `rx_dropped` / `tx_dropped` | Packets dropped (buffer full, netfilter reject) |
| `rx_overruns` | Ring buffer overflows |
| `multicast` | Multicast packets received |

**Network monitoring interfaces:**
- `/proc/net/tcp` — active TCP connections and state
- `/proc/net/udp` — UDP socket bindings
- `/proc/net/arp` — ARP cache entries
- `/proc/net/route` — IPv4 routing table
- `/proc/net/dev` — per-interface statistics
- `/proc/net/snmp` — IP/ICMP/TCP/UDP protocol statistics (MIB-compatible)
- `/proc/net/netfilter` — netfilter rules and counters
- `/proc/net/conntrack` — connection tracking table
- `/proc/net/netstat` — extended network statistics

### Interface Lifecycle

```
1. Driver probe → netif_register(dev)     [assigns ifindex, sets IFF_UP]
2. Address assignment (DHCP or static):
   - net_our_ip, net_subnet_mask, net_gateway configured
   - ARP cache initialized
3. Default route added to routing table
4. Interface becomes operational:
   - IFF_RUNNING set
   - net_poll() begins receiving packets
5. (Optional) Teardown:
   - netif_unregister(ifindex) — removes from table
   - Driver releases DMA buffers, MSI-X vectors
```

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
**Headers:** `src/include/module.h`, `src/include/module_elf.h`, `src/include/module_deps.h`, `src/include/module_signature.h`, `src/include/module_compress.h`, `src/include/module_autoload.h`

The kernel supports loadable modules as standalone `.ko` files. 226 modules compile from `obj-m` entries across the source tree. Each module is an ELF64 relocatable object (ET_REL) with its own `.text`, `.rodata`, `.data`, and `.bss` sections, plus meta-sections for symbol tables, relocation entries, signature data, and modinfo strings.

### Module Architecture

```text
┌─────────────────────────────────────────────────┐
│                  Userspace                      │
│  insmod / modprobe / rmmod / lsmod / modinfo   │
└──────────────────────┬──────────────────────────┘
                       │ syscall (module_init / module_delete / module_query)
                       ▼
┌─────────────────────────────────────────────────┐
│              Kernel Module Loader               │
│  ┌─────────────┐  ┌──────────┐  ┌───────────┐  │
│  │  ELF Parser  │  │ Relocator│  │ Symtable  │  │
│  │ module_elf.c │  │ (RELA)   │  │ resolver  │  │
│  └──────┬───────┘  └──────────┘  └─────┬─────┘  │
│         │                               │        │
│  ┌──────▼───────────────────────────────▼──────┐ │
│  │        64 MB Module Virtual Region          │ │
│  │  0xFFFF800100000000 — 0xFFFF800140000000    │ │
│  │  ┌─────┐ ┌────────┐ ┌──────┐ ┌────────┐   │ │
│  │  │.text│ │.rodata │ │.data │ │ .bss  │... │ │
│  │  │ (RX)│ │  (RO)  │ │ (RW) │ │ (RW)  │   │ │
│  │  └─────┘ └────────┘ └──────┘ └────────┘   │ │
│  └─────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────┘
```

### Module Data Structures

Each loaded module is represented by a `struct kernel_module` (defined in `src/include/module.h`), allocated in a fixed-size global table `g_modules[MODULE_MAX=16]`:

```c
struct kernel_module {
    char           name[32];           /* Module name (e.g. "e1000") */
    module_entry_t entry;              /* module_init() entry point */
    module_exit_t  exit_fn;            /* module_exit() cleanup */
    enum module_state state;           /* Current lifecycle state */
    uint64_t base_addr;                /* Virtual base in module region */
    uint64_t size;                     /* Total allocated size */
    struct module_section sections[16];/* Per-section tracking (vaddr/size/flags) */
    int num_sections;
    int refcount;                      /* Reference counter (get/put) */
    int module_id;                     /* Slot index in g_modules[] */
    struct module_dep deps[16];        /* Dependency list */
    int num_deps;
    uint32_t flags;                    /* Async probe, etc. */
    struct list_head params;           /* Module parameter list */
    int param_count;
};
```

The `struct module_section` tracks individual ELF sections within module memory:

```c
struct module_section {
    uint64_t vaddr;       /* Virtual address in module region */
    uint64_t size;        /* Size in bytes */
    uint32_t sh_flags;    /* ELF section flags (SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR) */
};
```

### Module State Machine

Modules progress through five states during their lifecycle:

```text
                  ┌──────────────────┐
                  │   MODULE_UNUSED   │  (slot free)
                  └────────┬─────────┘
                           │ module_load() called
                           ▼
                  ┌──────────────────┐
                  │  MODULE_LOADING   │  (ELF parse, relocate, verify)
                  └────────┬─────────┘
                           │ module_elf_finalize() succeeds
                           ▼
                  ┌──────────────────┐
                  │   MODULE_LIVE    │  (entry() called, module active)
                  └────────┬─────────┘
                           │ module_unload() called
                           ▼
                  ┌──────────────────┐
                  │ MODULE_UNLOADING  │  (exit_fn() called, refs drained)
                  └────────┬─────────┘
                           │ resources freed
                           ▼
                  ┌──────────────────┐
                  │   MODULE_DEAD    │  (slot reusable)
                  └──────────────────┘
```

If any loading step fails, the module transitions directly from `MODULE_LOADING` to `MODULE_ERROR`, and the allocated memory regions are freed.

### Module Loading Sequence (Detailed)

The full load path from `insmod`/`modprobe` to a live module consists of nine stages:

**Stage 1 — Verify signature (module_signature.c)**
The .ko file's `.module_sig` ELF section is extracted and verified. Two verification methods exist:
- **Raw RSA-2048/SHA-256:** The module's SHA-256 hash is computed and compared against the PKCS#1 v1.5 signature carried in `.module_sig`, using the kernel's embedded RSA public key (shared with the SSH host key via `rsa_key.h`).
- **PKCS#7 chain verification:** The `.module_sig` section may contain a PKCS#7 SignedData structure with a signer certificate whose SubjectKeyIdentifier must match a registered trusted key fingerprint. Up to 8 trusted keys can be registered.
- **Enforcement modes:** Mode 0 = warn-only (log but allow unsigned/invalid), Mode 1 = enforce (reject unsigned or invalid modules, default).

**Stage 2 — Validate ELF header (module_elf.c)**
The module ELF header is checked for:
- ELF magic number (`\x7fELF`)
- 64-bit class (ELFCLASS64)
- x86-64 machine type (EM_X86_64)
- Relocatable object type (ET_REL)
- Proper section header offset, string table indices, and file size bounds

**Stage 3 — Parse section headers**
All section headers are read from the ELF file. The section name string table (`.shstrtab`) is located and used to identify each section's type:
- `.text` → executable code (RX)
- `.rodata` → read-only data (RO)  
- `.data` → writable data (RW)
- `.bss` → zero-initialized data (RW)
- `.symtab` / `.strtab` → symbol table and string table
- `.rela.text`, `.rela.data`, `.rela.rodata` → RELA relocation entries
- `.modinfo` → module metadata (license, author, description, dependencies, aliases)
- `.module_sig` → RSA-2048/SHA-256 signature
- `.gnu.linkonce.this_module` → embedded `struct module` data

**Stage 4 — Parse symbol table**
The `.symtab` section is parsed to identify:
- **Defined (exported) symbols** — global symbols the module provides to the kernel
- **Undefined (imported) symbols** — references that must be resolved against the kernel's EXPORT_SYMBOL table
- **Local symbols** — module-internal references

**Stage 5 — Check blacklist and vermagic**
The module name is checked against the boot-time blacklist (`modprobe.blacklist=mod1,mod2` on the kernel command line). Blacklisted modules are rejected immediately.
The module's embedded vermagic string (kernel version + SMP/preempt/arch flags) is compared against the running kernel's `module_vermagic[]`. A mismatch taints the kernel (`TAINT_MODULE_VERMAGIC_MISMATCH`) and, depending on configuration, may reject the module.

**Stage 6 — Resolve dependencies (module_deps.c)**
Dependencies declared in `.modinfo` (`depends=mod1,mod2,...`) are resolved transitively:
1. For each dependency, check if the module is already loaded
2. If not, call `request_module()` to auto-load it
3. Recursively resolve the dependency's dependencies
4. A topological sort places dependencies before dependents in the load order
5. The `module_can_unload()` function checks for reverse dependencies before allowing unload

**Stage 7 — Allocate module memory region (M10)**
A block of the required size is allocated from the 64 MB module virtual region (`MODULES_VADDR` to `MODULES_END`, 0xFFFF800100000000 to 0xFFFF800140000000). The region allocator (`module_alloc_region()`) uses a simple first-fit algorithm within the pre-reserved virtual address space. Sections are mapped with appropriate permissions:
- `.text` → RX (PAGE_PRESENT | PAGE_USER)
- `.rodata` → RO (PAGE_PRESENT | PAGE_USER | PAGE_NX)
- `.data` / `.bss` → RW (PAGE_PRESENT | PAGE_USER | PAGE_RW | PAGE_NX)

**Stage 8 — Apply RELA relocations**
For each `.rela.*` section, the loader walks all RELA entries (64-bit relocation with addend, the standard x86-64 ELF format) and applies them:
- `R_X86_64_64` — 64-bit absolute address (direct S + A)
- `R_X86_64_PC32` — 32-bit PC-relative offset
- `R_X86_64_PLT32` — PLT-relative (resolved via GOT/PLT if needed)
- `R_X86_64_GOTPCREL` — GOT-relative (Global Offset Table reference)
- Undefined symbols trigger module load failure with a descriptive error

**Stage 9 — Call module_init() and transition to LIVE**
After all sections are loaded, permissions set, and relocations applied, the module's init function (`module_init(fn)`) is called. If successful, the module transitions from `MODULE_LOADING` to `MODULE_LIVE`. If init fails, the module enters `MODULE_ERROR` and resources are freed.

### Symbol Export System (EXPORT_SYMBOL)

The kernel maintains an exported symbol table (`export.c`, `src/include/export.h`) that modules resolve against during loading:

```c
struct kernel_symbol {
    uint64_t value;         /* Address of the symbol */
    const char *name;       /* Symbol name string */
};
```

Key characteristics:
- **50+ symbols exported** across core kernel subsystems (memory allocation, string functions, device I/O, timer, VFS operations, etc.)
- **Symbol table** — a sorted array of `struct kernel_symbol` entries, searched via binary search during resolution
- **EXPORT_SYMBOL_GPL variant** — restricts usage to GPL-licensed modules (checked via `.modinfo` license field)
- **Module-to-module symbol resolution** — one module's exported symbols become available to subsequently loaded modules
- **Lookup function:** `find_exported_symbol(name)` — iterates the kernel export table, returns the symbol value or NULL

### Module Parameter System

Modules can declare parameters that are configurable at load time via the kernel command line or `insmod`:

```c
module_param(name, type, perm);  // Declares a named parameter
```

Parameter types supported:
| Type | C type | Example |
|------|--------|---------|
| `PARAM_TYPE_INT` | `int` | `module_param(debug, int, 0644)` |
| `PARAM_TYPE_UINT` | `unsigned int` | `module_param(buf_size, uint, 0)` |
| `PARAM_TYPE_CHARP` | `char *` | `module_param(name, charp, 0)` |
| `PARAM_TYPE_STRING` | `char[N]` | Fixed-size buffer |
| `PARAM_TYPE_BOOL` | `bool` | Accepts 0/1, y/n, on/off |

Parameters are registered via the linked list `struct kernel_module.params` and exposed through `/sys/module/<name>/parameters/` for runtime inspection and modification (when permissions allow).

Boot-time module parameters can also be specified on the kernel command line:
```
e1000.debug=1 ext2.verbose_mount=1
```
These are parsed by `modules_init()` into a static cache (`g_cmdline_params[]`) and applied to modules as they load, before their init function runs.

### Module Autoload (request_module)

The kernel automatically loads modules when needed through `request_module()`:

**Trigger points:**
- **PCI device discovery:** `pci_autoprobe_work()` generates modalias strings (`pci:v0000VVVVd0000DDDDsv0000SSSS...`) and calls `request_module()` for each unknown device
- **USB device discovery:** Similar modalias-based loading for USB devices
- **Filesystem mount:** `vfs_mount()` calls `request_module("ext2")` when an unknown filesystem type is encountered
- **Socket creation:** `request_module("ipv6")` is called when an AF_INET6 socket is created
- **Network protocol:** Protocol-specific autoload for netfilter, tunnels, crypto modules

**Module loading flow in request_module():**
1. Check if the module is already loaded (by name)
2. Search the default module path: `/modules/<name>.ko` (or `/modules/<name>.ko.gz` for compressed)
3. Detect compression type (gzip or xz) via magic bytes
4. Decompress the module data if needed
5. Pass decompressed data to the standard ELF module loader
6. Trigger dependency resolution and auto-load missing dependencies

**Module aliasing:** Modules declare alias patterns in `.modinfo` via `MODULE_DEVICE_TABLE(pci, table)`. During autoload, the modalias string is matched against all registered module aliases (`module_alias.c`). The alias table supports wildcard patterns (e.g., `pci:v00008086d*` for all Intel devices).

### Module Compression

Modules can be stored compressed in the initramfs to reduce disk and memory footprint:

| Format | Magic bytes | Implementation |
|--------|-------------|----------------|
| **gzip** | `0x1f 0x8b` | Full DEFLATE inflator (RFC 1951/1952) — 1300+ lines of decompression code |
| **xz** | `0xfd 0x37 0x7a 0x58 0x5a 0x00` | Detection only (LZMA2 decompression stubbed for future implementation) |

Detection occurs via magic byte matching in `module_compress.c`. Compressed files use the `.ko.gz` or `.ko.xz` extension. The decompressed data is passed to the standard ELF loader.

### Module Signature Verification

All modules are verified cryptographically before loading:

```
module.ko → SHA-256 hash → RSA-2048 sign (PKCS#1 v1.5) → .module_sig section
kernel    → embed RSA public key → on load: re-hash + RSA verify
```

**Key management:**
- The kernel's RSA public key is embedded at build time from `rsa_key.h` (shared with SSH host key infrastructure)
- Additional trusted keys can be registered at runtime (up to `MAX_TRUSTED_KEYS=8`)
- PKCS#7 chain verification allows signing by X.509 certificates whose SubjectKeyIdentifier matches a registered trusted key

**Enforcement modes (configurable at boot):**
- `module.sig_enforce=0` — warn-only, log but allow unsigned/invalid modules
- `module.sig_enforce=1` — enforced, reject unsigned or invalid modules (default)

Unsigned modules or those with mismatched vermagic taint the kernel (`TAINT_MODULE_UNSIGNED`, `TAINT_MODULE_VERMAGIC_MISMATCH`).

### Userspace Tools

| Command | Source | Purpose |
|---------|--------|---------|
| `insmod.elf` | `userspace/bin/insmod.c` | Load a single .ko file via syscall |
| `modprobe.elf` | `userspace/bin/modprobe.c` | Load module + dependencies by name |
| `rmmod.elf` | `userspace/bin/rmmod.c` | Unload a module |
| `lsmod.elf` | `userspace/bin/lsmod.c` | List loaded modules (name, size, refcount, dependents) |
| `modinfo.elf` | `userspace/bin/modinfo.c` | Display module metadata (license, author, params, aliases, deps) |

### Initramfs Integration

- All 226 `.ko` files are bundled in the initramfs image at `/modules/`
- The init process (`/sbin/init`) reads `/etc/modules` and loads the listed modules at boot via `modprobe`
- The initramfs extraction (`initramfs_extract()`) unpacks the CPIO archive and populates the module directory before init starts
- Modules are stored uncompressed or gzip-compressed depending on the build configuration

### Build System

- Modules are compiled with the `-DMODULE` flag, which adds `#define MODULE 1` to the preprocessor, enabling module-specific code paths
- Each module is compiled from `obj-m` entries in the Makefile — e.g., `obj-m += e1000.o` produces `e1000.ko`
- `make modules` builds all `.ko` files without rebuilding the kernel
- `make all` (or just `make`) builds the kernel and all modules together
- The linker produces relocatable ET_REL objects (not ET_EXEC or ET_DYN), which the kernel loader handles via full RELA relocation
- Module build uses the same cross-compiler and flags as the kernel (`CC=x86_64-linux-gnu-gcc`)
- Modules are collected into `build/modules/` after compilation and bundled into the initramfs

## Container Runtime & Orchestrator

**Files:** `src/container/` (runtime.c, config.c, image.c, storage.c, network.c, state.c, ext.c, orch.c, checkpoint.c, scheduler_policy.c, service_proxy.c, seccomp_notify.c, security_scan.c, container_exec_enhanced.c)
**Headers:** `src/include/container.h`, `src/include/container_exec_enhanced.h`, `src/include/orch_api.h`, `src/include/orch_health.h`, `src/include/orch_hooks.h`

This kernel implements an OCI (Open Container Initiative) compatible container runtime entirely in-kernel, with a complementary HTTP-based orchestration API, pod abstraction, service discovery, and health checking.

### Architecture Overview

The container runtime follows a layered design:

```
OCI Runtime (runtime.c, config.c)
    ├── Storage (storage.c, image.c)
    │     Download/verify layers → unpack → overlay mount → rootfs
    ├── Networking (network.c)
    │     Create netns → veth pair → bridge attach → IPAM → firewall rules
    ├── Lifecycle (state.c, ext.c, checkpoint.c, container_exec_enhanced.c)
    │     Exec, attach, logs, pause/unpause, stats, checkpoint/restore
    └── Orchestration (orch.c, scheduler_policy.c, service_proxy.c)
          Pod/deployment management, health probes, service discovery,
          resource quotas, affinity/anti-affinity, seccomp notify
```

### Container ↔ Kernel Primitive Mapping

A running container is the composition of standard kernel isolation
primitives rather than a separate kernel object. `container_create()`
builds the on-disk hierarchy and mounts; `container_start()` spawns the
init process via `process_spawn()` with the configured namespace flags.

```
┌──────────────────────────────────────────────────────────────┐
│  Container (one descriptor in container_table[CONTAINER_MAX])│
└──────┬───────────────────────────────────────────────────────┘
       │ created from config.json (OCI bundle) by container_create()
┌──────▼───────────────────────────────────────────────────────┐
│  Rootfs (overlay union mount)                                │
│    lowerdir = image layers (read-only, content-addressed)    │
│    upperdir = container diff (writable)  workdir = work      │
│    mounted over /var/lib/containers/<id>/rootfs             │
└──────┬───────────────────────────────────────────────────────┘
       │ process_spawn() with ns_flags
┌──────▼───────────────────────────────────────────────────────┐
│  Namespace isolation (ns_flags)                              │
│    CLONE_NEWPID  → separate PID namespace (init = PID 1)     │
│    CLONE_NEWNS   → private mount namespace                   │
│    CLONE_NEWNET  → veth pair into container netns            │
│    CLONE_NEWIPC  → private IPC namespace                     │
└──────┬───────────────────────────────────────────────────────┘
       │ runtime bookkeeping
┌──────▼───────────────────────────────────────────────────────┐
│  Resource & policy enforcement                               │
│    cgroup v2  → CPU/memory/PID accounting under              │
│                 /sys/fs/cgroup/containers/<id>/              │
│    seccomp    → syscall filtering (seccomp_notify daemon)    │
│    rlimits    → CPU time, memory, open-file caps             │
└──────────────────────────────────────────────────────────────┘
```

Isolation comes from kernel `CLONE_NEW*` namespaces, resource control
from the cgroup v2 hierarchy, syscall filtering from seccomp, and
storage from overlay union mounts. The container manager itself only
orchestrates these existing kernel mechanisms — there is no dedicated
"container" scheduler; container processes run as ordinary tasks under
the global scheduler.

### Container Table & State Machine

A fixed-size global table (`container_table[CONTAINER_MAX]` with CONTAINER_MAX=64) holds all active container descriptors. Each slot is protected by a per-container spinlock; the global table lock guards table-wide operations (alloc, free, iteration).

**OCI runtime-spec state machine:**

```text
CREATING → CREATED → RUNNING → STOPPED → DELETED
                         ↓
                      PAUSED
```

State transitions are validated by `container_set_state()` and persist state to `/run/containers/<id>/state.json` on every change.

### Container Lifecycle

| Step | Function | Description |
|------|----------|-------------|
| 1 | `container_init()` | Create `/var/lib/containers/` and `/run/containers/` directories |
| 2 | `container_alloc()` | Allocate a slot in the global container table |
| 3 | `container_set_id()` | Generate a unique 16-char hex ID from (PID + tick + counter) |
| 4 | `container_create()` | Build OCI directory hierarchy, mount virtual filesystems (proc, sys, dev, tmpfs), transition to CREATED |
| 5 | `container_start()` | Parse config.json, spawn init process via `process_spawn()`, apply resource limits, transition to RUNNING |
| 6 | `container_stop()` | Send SIGTERM → wait timeout → SIGKILL, transition to STOPPED |
| 7 | `container_delete()` | Remove directories, free descriptor slot, transition to DELETED |

Extended lifecycle operations (ext.c): `container_exec()` (run process in container), `container_attach()` (stream I/O), `container_pause()`/`container_unpause()` (freeze/thaw processes), `container_wait()` (block until exit), `container_stats()` (cgroup resource usage), `container_top()` (list PIDs), `container_inspect()` (JSON metadata dump).

### Enhanced Exec (`container_exec_enhanced.c`)

Provides interactive process execution inside containers with:

- **PTY allocation** — pseudo-terminal master/slave pair for full terminal support
- **stdin/stdout/stderr channels** — pipe-based I/O multiplexing
- **Terminal resize** — SIGWINCH forwarding for dynamic terminal dimensions
- **Non-destructive detach** — process continues running after client disconnects

### Container Images (`image.c`, `storage.c`)

OCI image format support with content-addressable storage:

```text
Image Manifest (JSON)
    ├── Image Config (JSON) — Cmd, Entrypoint, Env, Volumes, ExposedPorts
    └── Layer Blobs (tar+gzip) — stacked via overlay mounts
        Layer N (top) ← writable container layer
        Layer N-1
        ...
        Layer 1 (base)
```

- **Pull/Push** — Docker Registry v2 API with auth (basic, bearer token)
- **Layer cache** — blobs stored at `/var/lib/containers/images/blobs/sha256/<digest>`
- **Overlay mount** — lowerdir=layers, upperdir=container diff, workdir=work
- **Tag management** — local repository index at `/var/lib/containers/images/repositories.json`
- **Prune** — garbage-collect unreferenced layers

### Container Networking (`network.c`)

Per-container network isolation using standard kernel networking primitives:

```text
Host netns                          Container netns
┌──────────────────────┐         ┌──────────────────────┐
│  bridge (docker0)    │         │  eth0 (veth peer)    │
│  veth-<id1> ─────────┼─────────┤  IP: 10.0.2.x/24    │
│  veth-<id2> ─────────┼──┐      │  Gateway: 10.0.2.1  │
│  IP: 10.0.2.1/24     │  │      └──────────────────────┘
│  iptables MASQUERADE  │  │      ┌──────────────────────┐
│  Port forwarding:     │  │      │ Container netns #2   │
│    host:8080→10.0.2.2│80│      │  eth0: 10.0.2.3/24  │
└──────────────────────┘  │      └──────────────────────┘
                          └─────── bridge isolates L2
                                  traffic between containers
```

1. **Network namespace** — each container gets an isolated netns anchored by a bind-mount file in `/var/run/netns/<id>`
2. **veth pair** — one end in container (eth0), one end in host (veth-`<id>`)
3. **Bridge** — veth endpoints attach to a Linux bridge for L2 connectivity
4. **IPAM** — static IP assignment or DHCP within the container subnet
5. **Port mapping** — netfilter DNAT rules for host-to-container port forwarding
6. **Firewall** — default: drop all inbound, allow all outbound; per-container rules tracked for cleanup

### Orchestration API (`orch.c`, `orch/`)

HTTP REST API server on port 8375 providing orchestration primitives:

**Pod abstraction** (`MAX_PODS=32`, `MAX_CONTAINERS_PER_POD=16`): A pod is a group of containers that share the same network namespace, IPC namespace, and PID namespace. Containers within a pod communicate via localhost and can share volumes.

**Service abstraction**: Services provide stable network endpoints to pods:
- ClusterIP — virtual IP within the cluster network
- NodePort — expose on host port across all nodes
- LoadBalancer — external load balancer integration

**Health checking** (`orch_health.h`): Configurable liveness and readiness probes:
- Exec probes — run command inside container, check exit code 0
- HTTP probes — HTTP GET to container-ip:port, expect 2xx/3xx
- Configurable: initial delay, period, timeout, success/failure thresholds

**Lifecycle hooks** (`orch_hooks.h`): PostStart and PreStop hooks with exec and HTTP support. Best-effort execution: hook failure is logged but does not block container lifecycle.

**Scheduler policies** (`scheduler_policy.c`): Resource quotas, limit ranges, pod priority/preemption, inter-pod affinity/anti-affinity, taints and tolerations.

**Service proxy** (`service_proxy.c`): Layer-4 load balancing with iptables and userspace modes, DNS-based service discovery, ConfigMap and Secret volume mounting.

### Security & Isolation

- **seccomp notify** (`seccomp_notify.c`) — user-space policy daemon for syscall interception via seccomp
- **Security scanning** (`security_scan.c`) — in-kernel CVE matching against image layer package manifests
- **Checkpoint/restore** (`checkpoint.c`) — CRIU-like container state freeze and restore for live migration
- **Namespace isolation** — each container gets its own PID, mount, network, and IPC namespaces (via CLONE_NEW* flags)

### Cross-References

- **Network Namespaces** — see the Network Namespaces section earlier in this document for the low-level netns API
- **Kernel Modules** — 226 kernel modules provide drivers that containers can use (e.g., filesystem modules for storage drivers)
- **Seccomp** — `src/kernel/seccomp.c` provides the BPF-based syscall filtering used by container security policies
- **IPC subsystem** — `src/ipc/` is used for signal delivery and process synchronisation within containers
- **VFS layer** — overlay filesystem (`src/kernel/overlay.c`) enables union-mount container rootfs

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

### Orchestration Workflow

Cluster orchestration coordinates the Raft consensus engine and the
gossip membership protocol to place and manage workloads:

```
Node Join / Membership
    node start → clusterd runs gossip (SWIM-style membership)
    existing nodes detect newcomer via suspicion/infection dissemination
    node reported healthy → added to member list, replicated via Raft log

Leader Election (Raft)
    Follower → (election timeout) → Candidate
    Candidate → (majority of votes) → Leader
    Leader owns: log replication, KV store writes, orchestration decisions
    All writes go through the Raft log; followers apply entries in order

Deploy a Workload
    orchctl / REST → leader
    leader appends "deploy pod <id>" to Raft log → replicates to followers
    leader selects a target node (resource-aware placement)
    kernel hooks: IPVS load-balancer + conntrack NAT configured via netlink

Service Exposure
    Service (ClusterIP/NodePort/LoadBalancer) mapped to selectors
    IPVS VIP + real servers synced to kernel over netlink
    Conntrack tracks flows for NAT and policies
```

The state machine in `clusterd.c` is explicit:
`RAFT_FOLLOWER → RAFT_CANDIDATE → RAFT_LEADER` (then back to
`FOLLOWER` on timeout/term change). Because all orchestration writes are
durably replicated through the Raft log before clusterd acts on them,
and gossip provides an eventually-consistent membership view, the
control plane tolerates individual node failure without a single point
of coordination.

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

## Userspace Subsystem

The userspace layer runs atop the kernel and comprises several major components:

```
┌─────────────────────────────────────────────────────────┐
│                    Userspace                            │
│  ┌──────────┐  ┌──────┐  ┌─────┐  ┌──────┐  ┌──────┐  │
│  │  Init    │  │ Shell│  │ GUI │  │ Doom │  │ DOS  │  │
│  │ (init.c) │  │(sh.c)│  │     │  │      │  │ Emu  │  │
│  └────┬─────┘  └──────┘  └─────┘  └──────┘  └──────┘  │
│       │                                                 │
│  ┌────┴─────────────────────────────────────────────┐   │
│  │            libc (userspace/libc/)                 │   │
│  │  unistd.h, stdio.h, string.h, stdlib.h,          │   │
│  │  pthread.h, math.h, signal.h, fcntl.h, etc.      │   │
│  └──────────────────────────────────────────────────┘   │
│  ┌──────────────────────────────────────────────────┐   │
│  │   Kernel Modules (226 .ko files in initramfs)     │   │
│  │   Loaded by init via modprobe at boot             │   │
│  └──────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

### Init (`userspace/init/init.c`)
The first userspace process (PID 1). Responsible for:
- Mounting the real root filesystem
- Loading kernel modules from `/modules/` in initramfs
- Launching the shell and GUI on the console
- Handling orphaned child processes (reaping zombies)
- System shutdown/reboot coordination

### Shell (`userspace/bin/sh.c`)
A feature-rich shell with 356+ built-in commands. Features:
- Command parsing with pipes, redirection, and background jobs
- Variable expansion (`${var:-default}`, `${var:+alt}`, `${#var}`)
- Shell arrays indexed from 0
- Tab completion and persistent command history
- Command table (`cmd_table.inc`) auto-generated from registrations
- Job control with foreground/background process groups
- Builtins: eval, read, type, dirs/pushd/popd, cd, export, alias, exec, test, etc.

### GUI (`userspace/gui/`)

A graphical desktop environment providing a complete windowing system, widget toolkit, and drawing library for the OS. It runs as a userspace process (or kernel task) and renders into the VGA framebuffer via `vga_put_pixel()`.

#### Architecture Overview

The GUI framework is structured around six subsystems:

| Subsystem | Files | Description |
|-----------|-------|-------------|
| Core context & window manager | `gui.c`, `gui.h` | Global GUI state, window list, focus management, event dispatch, rendering pipeline |
| Drawing primitives | `gui_draw.c`, `gui_draw.h` | 50+ rasterization primitives — lines, circles, ellipses, beziers, polygons, gradients, filled shapes |
| Widget toolkit | `gui_widgets.c`, `gui_widgets.h` | Concrete widget types (button, textbox, label, textedit, tabview, tooltip, notification, filebrowser, taskbar) |
| Demo applications | `gui_apps.c`, `gui_apps.h` | 80+ runnable demo apps (Mandelbrot, calculator, paint, clock, minesweeper, snake, tetris, sorting viz, etc.) |
| Desktop shell | `gui_shell.c`, `gui_shell.h` | Desktop shell with app launcher, taskbar, file browser, and window drag support |
| Kernel GUI task | `gui_task.c` | Background kernel task variant of the desktop shell |

#### Core Architecture (`gui.c`)

**1. Global GUI Context (`g_gui_ctx`)**
A single static `struct gui_context` holds all display state:
- Doubly-linked list of all windows (inserted at head, rendered back-to-front)
- Pointer to the currently focused window (receives keyboard events)
- Mouse position (`mouse_x`, `mouse_y`) and button state (`mouse_buttons`)
- Current cursor shape (`gui_cursor_t`)
- Theme colors (title bar, window background, button background)
- Initialization flag

```c
struct gui_context {
    gui_window_t *windows;
    gui_window_t *focused_window;
    int32_t mouse_x, mouse_y;
    int mouse_buttons;
    int initialized;
    gui_cursor_t cursor;
    gui_color_t theme_title_bg;
    gui_color_t theme_window_bg;
    gui_color_t theme_button_bg;
};
```

**2. Window Management**
Windows are the top-level containers in the display. Each `gui_window_t` stores:
- Position & size (`x, y, w, h`) and a saved rectangle for minimize/restore
- Title string (up to 64 characters)
- Background color and visibility flag
- State: `NORMAL`, `MINIMIZED`, or `MAXIMIZED`
- Minimum dimensions (`min_w`, `min_h`)
- A singly-linked list of child widgets (`struct gui_widget`)
- Pointer to the focused child widget
- Doubly-linked list pointers for z-order traversal

Key operations:
- `gui_window_create()` — allocates and initialises a new window
- `gui_add_window()` / `gui_remove_window()` — manage the window list
- `gui_window_bring_to_front()` — reorders the z-stack
- `gui_window_minimize()` / `gui_window_maximize()` / `gui_window_restore()` — state transitions
- `gui_window_close()` — closes and destroys a window
- `gui_window_titlebar_at()` / `gui_window_resize_handle_at()` — hit-testing for window chrome

**3. Widget System**
Every widget is a self-drawing, event-handling rectangle with polymorphic dispatch:

```c
struct gui_widget {
    gui_rect_t rect;
    gui_color_t bg, fg;
    int visible;
    int enabled;
    uint32_t flags;
    struct gui_widget *next;
    void *data;                          /* type-specific data pointer */
    gui_widget_draw_fn draw;             /* called each frame to render */
    gui_widget_event_fn on_event;        /* called on input events */
    gui_widget_destroy_fn destroy;       /* called to clean up */
};
```

Concrete widget types each store their specific data in a struct referenced via the generic `data` pointer:

| Widget | Data struct | Description |
|--------|-------------|-------------|
| `gui_button_create()` | `gui_button_data_t` | Clickable with `on_click` callback, label, auto-sizing |
| `gui_textbox_create()` | `gui_textbox_data_t` | Single-line text input, cursor, max length |
| `gui_label_create()` | `gui_label_data_t` | Static text display |
| `gui_textedit_create()` | `gui_textedit_data_t` | Multi-line text editor with scrolling (2048 chars) |
| `gui_tabview_create()` | `gui_tabview_data_t` | Tabbed container with linked list of `gui_tab_t` |
| `gui_tooltip_create()` | `gui_tooltip_data_t` | Small popup label with configurable delay |
| `gui_notification_create()` | `gui_notification_data_t` | Color-bar toast with configurable lifetime |
| `gui_filebrowser_create()` | `gui_filebrowser_t` | Directory listing with navigation and on-select callback |
| `gui_taskbar_create()` | `gui_taskbar_t` | System taskbar with application launcher buttons |

**4. Drawing Primitives (`gui_draw.c`)**

The rendering layer provides 50+ primitives targeting a pixel-addressable framebuffer:

| Category | Primitives | Algorithm |
|----------|------------|-----------|
| Lines | `draw_line`, `draw_dashed_line`, `draw_thick_line`, `draw_arrow_line` | Bresenham (integer arithmetic, no FP) |
| Circles & Ellipses | `draw_circle`, `draw_circle_filled`, `draw_ellipse`, `draw_ellipse_filled` | Midpoint algorithm |
| Arcs & Pies | `draw_arc`, `draw_pie` | Angular sweep with sin/cos LUT |
| Beziers | `draw_bezier_cubic`, `draw_bezier_quad` | De Casteljau evaluation |
| Polygons | `draw_polyline`, `draw_polygon`, `draw_polygon_filled` | Scan-line active-edge fill |
| Triangles | `draw_triangle`, `draw_triangle_filled` | Scan-line with barycentric interpolation |
| Rounded Rects | `draw_rounded_rect`, `draw_rounded_rect_filled` | Corner arc blending |
| Gradients | `draw_gradient_v`, `draw_gradient_h`, `draw_gradient_radial` | Per-pixel color lerp |
| Images | `draw_image_raw`, `draw_checkerboard` | Pixel buffer copy |
| Extended | `draw_star`, `draw_progress_bar`, `draw_3d_frame`, `draw_hex_grid`, `draw_sine_wave`, `draw_gauge`, `draw_led`, `draw_heart`, `draw_spiral`, and more | Specialized rasterization |

**5. Bitmap Font**
A built-in 5×7 pixel monochrome font (`font5x7[]`) provides 47 glyphs:
- A–Z, a–z, 0–9, space, period, comma, dash, slash, parentheses, colon, underscore, plus, equals
- Each glyph is a 7-byte bitmap with 5 bits per row
- Characters are rendered at 10×14 pixels (2× scaling) via `render_glyph()`
- `font_char_index()` maps any ASCII character to its font index (returns space for unsupported)
- Clipping is applied against the window's clip rectangle

**6. Rendering Pipeline (`gui_render_frame()`)**

Called periodically (~30 FPS) to produce a complete frame:
1. Clear the VGA framebuffer to black
2. Traverse windows from back to front (tail → head of the list)
3. For each visible window:
   a. Fill the window rectangle with its background colour
   b. Draw the title bar (with [close] [minimize] [maximize] buttons) if a title exists
   c. Draw the border — white if focused, dark gray if unfocused
   d. Draw resize handle indicators in the bottom-right corner
   e. Iterate and draw all child widgets via their `draw` function pointers
4. Overdraw the mouse cursor on top of everything (8 cursor shapes)
5. Call `vga_refresh_console()` to commit the frame

**7. Input Event Handling (`gui_handle_event()`)**
Events (mouse down/up/move/drag, keyboard char) are dispatched to the focused window's focused widget. Mouse coordinates are validated against the widget's bounding rectangle before dispatch to prevent off-screen or negative coordinates from triggering unintended actions.

**8. Main Loop (`gui_run_loop()`)**
The event-driven GUI loop:
- Polls the keyboard for character input (ESCAPE to exit)
- Polls the mouse for position and button changes
- Dispatches input events to the focused widget
- Triggers a full frame redraw every ~33 ms or on input events
- Calls `scheduler_yield()` each iteration to cooperate with other tasks

#### GUI Applications (`gui_apps.c`)

Over 80 demo applications demonstrate the framework's capabilities:

| Category | Apps |
|----------|------|
| Drawing | draw, colors, gradient, shapes, checker, typography, flood_fill |
| Math/Nature | mandelbrot, julia, lorenz, snowflake, biorhythm, complex |
| Games | minesweeper, snake, tetris, pong, bouncing_ball |
| Visualization | sort_viz, sort_compare, chart_bar, chart_line, chart_pie, wave, noise, heatmap, fractal_tree, sierpinski, moire, tunnel, metaballs, rotozoom, kaleidoscope, starfield, fire, plasma, particles, wave_interference, boids, voronoi, fireworks, fluid, softbody |
| Demos | lights, terrain, bezier_demo, text_editor, cube_3d, pendulum, fourier, wave_eq, reaction_diff, cellular, cellular2, maze_gen, pathfind, bintree, color_wheel, dither, edge, spirograph, ascii_art, audio_viz, memory_map, pong, tiling, clock_alarm, text_editor, convolution, buddha |
| Clock | analog_clock, digital_clock, clock_dual, stopwatch |
| Utilities | calc, rgb_mixer, paint, info, screensaver, biorhythm, solar, turing, gravity, eyes |

#### Desktop Shell (`gui_shell.c` / `gui_task.c`)

The desktop shell provides the full graphical environment:
- **Window drag** — click-and-drag title bars to move windows
- **Taskbar** — dark bar at screen bottom (y=750, h=18) with application launcher buttons and a status bar showing IP address and system uptime
- **File browser** — `gui_filebrowser_t` widget with directory navigation and on-select callbacks
- **App launcher** — two pages of launcher buttons organized via macros; each macro generates a `launch_<app>()` callback

#### Dependencies

```
gui.c → gui.h → gui_draw.h
  ↓        ↓
vga.h (framebuffer put_pixel, clear, refresh)
string.h, stdlib.h, stdio.h (libc)
```

Drawing primitives access the framebuffer without locking (caller must ensure mutual exclusion). Widget data structures use internal kmalloc/kfree; callers should not access widget data fields directly after creation.

### Doom (`userspace/kmods/doom/`)
A port of the classic game DOOM, running as a userspace process with:
- Full software renderer
- Sound effects via PC speaker or AC97
- Keyboard input handling
- Optimized for the kernel's framebuffer API

### DOS Emulator (`userspace/kmods/dosbox/`)
An x86 emulator for running legacy DOS programs:
- 8086 CPU emulation with real-mode segmentation
- DOS system call (INT 21h) emulation
- VGA text/graphics mode emulation
- Basic sound (PC speaker, AdLib) support

### libc (`userspace/libc/`)
Standard C library implementation providing:
- POSIX syscall wrappers (open, read, write, close, etc.)
- stdio (printf, scanf, fopen, fread, fwrite)
- string/memory functions (strcpy, memcpy, memset)
- stdlib functions (malloc, free, atoi, strtol)
- pthread API subset (mutex, thread creation, TLS)
- math library (sin, cos, sqrt, etc.)

## Directory Structure Overview

```
/ (repo root)
├── ARCHITECTURE.md          — this file
├── Makefile                 — top-level build system
├── linker.ld                — kernel linker script
├── src/                     — kernel source (~317K lines)
│   ├── kernel/              — core kernel (init, syscalls, process, scheduler)
│   ├── memory/              — PMM, VMM, heap, slab allocators
│   ├── drivers/             — device drivers (PCI, ATA, AHCI, NVMe, e1000, etc.)
│   ├── fs/                  — filesystem implementations and VFS
│   ├── net/                 — networking stack (TCP, UDP, IPv4, IPv6, sockets)
│   ├── ipc/                 — inter-process communication (pipes, shm, semaphores)
│   ├── process/             — signal handling, scheduler details
│   ├── lib/                 — in-kernel library (string, printf, AES, SHA, CRC)
│   ├── shell/               — built-in kernel shell
│   ├── test/                — in-kernel test framework
│   ├── container/           — container runtime, network, image
│   ├── orch/                — orchestrator (manifest, RBAC)
│   ├── boot/                — boot assembly, UEFI GOP
│   ├── power/               — suspend/resume
│   └── include/             — kernel headers (453+ header files)
├── userspace/               — userspace programs
│   ├── init/                — init process (PID 1)
│   ├── bin/                 — shell and utility binaries
│   ├── gui/                 — graphical desktop environment
│   ├── kmods/               — userspace module components (doom, dosbox)
│   ├── libc/                — C standard library + headers
│   └── lib/                 — userspace libraries
├── tests/                   — host-side and e2e tests
│   ├── host_libc/           — libc tests compiled against host glibc
│   └── e2e.sh               — QEMU-based end-to-end smoke test
├── build/                   — build output (kernel.bin, disk.img, .ko files)
├── .github/workflows/       — CI workflow definitions
└── .hermes/                 — Hermes agent configuration (local only)
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

## Recent Improvements

### Kernel Page Table Isolation (KPTI)

Implemented KPTI with PCID/INVPCID support for Meltdown mitigation. The kernel
maintains separate page tables for kernel and userspace: a "kernel mode" PML4
with all kernel+user mappings, and a "user mode" PML4 with only minimal kernel
entries (interrupt gates, syscall entry). Context switches between kernel and
user mode switch CR3 via PCID-tagged entries to avoid TLB flushing overhead.
Key files: `src/kernel/kpti.c`, `src/include/kpti.h`.

### Modular Kernel Architecture

Transitioned from a purely monolithic build to a modular kernel with 226
loadable kernel modules (`.ko` files). The module loader supports ELF64
relocation, GOT/PLT handling, EXPORT_SYMBOL symbol resolution, RSA-2048/SHA-256
signature verification, dependency tracking, and automatic module loading via
`request_module()`. Modules are compressed (xz/gzip) in initramfs and
decompressed on load. Module region layout: 64MB at `0xFFFF800100000000`
divided into RX (code), RO (rodata), and RW (data) sub-regions.

### E1000 NIC Driver

Added full Intel PRO/1000 network driver with MSI-X multi-queue, RSS (Receive
Side Scaling) for flow distribution across CPUs, hardware interrupt moderation
(ITR), and NAPI polling. Supports PCIe capability detection, multi-queue
transmit/receive with descriptor ring management, and hardware filtering.
File: `src/drivers/e1000.c` (with `e1000.h` header).

### PCIe ECAM Configuration

Upgraded PCI configuration access from legacy I/O port cycles (0xCF8/0xCFC) to
Enhanced Configuration Access Mechanism (ECAM) for PCIe. ECAM maps the entire
PCIe configuration space into memory-mapped I/O, enabling access to extended
PCIe capabilities (AER, DPC, PTM, SR-IOV). The ECAM memory region is
discovered via ACPI MCFG table. File: `src/drivers/pci.c`.

### 500+ System Calls

Expanded the system call table from ~120 to 512 entries. New syscalls include:
- Linux-compatible `openat`, `readv`, `writev`, `splice`, `sendfile`,
  `fallocate`, `copy_file_range`, `renameat2`
- Modern Linux syscalls: `pidfd_open`, `pidfd_send_signal`, `clone3`,
  `faccessat2`, `fsopen`, `fsconfig`, `fsmount`, `fspick`
- eBPF: `bpf()` syscall with BPF_PROG_LOAD, BPF_MAP_CREATE, etc.
- io_uring: `io_uring_setup`, `io_uring_enter`, `io_uring_register`
- Namespace: `setns`, `unshare`, `open_tree`, `move_mount`
- Security: `landlock_create_ruleset`, `landlock_add_rule`, `landlock_restrict_self`,
  `pkey_alloc`, `pkey_free`, `pkey_mprotect`, `mseal`

Syscall dispatch is now table-driven with per-syscall argument validation.
File: `src/kernel/syscall.c`.

### Test Infrastructure

Three-tier testing framework:
1. **In-kernel tests** (`src/test/test.c`) — 200+ tests running in QEMU,
   covering scheduler (fork/exec/exit/wait), VM (map/unmap/COW), PMM
   (alloc/free/refcount), slab, IPC, VFS, TCP/UDP, device probing, syscalls
2. **KUnit** (`src/test/kunit_tests.c`) — In-kernel unit testing framework
   with test case/suite/assertion API for subsystem-level tests
3. **Host-side tests** (`tests/host_libc/`) — kernel libc functions compiled
   and run on Linux host against glibc baseline (string, printf, stdlib,
   bitops, CRC, SHA-256, AES)
4. **E2E QEMU smoke test** (`tests/e2e.sh`) — boots kernel, validates boot
   sequence, shell commands, networking, filesystem operations via serial
5. **UBSan:** Kernel boots with CONFIG_UBSAN for automatic undefined behavior
   detection

### Performance Optimizations

- **Per-CPU data structures:** page caches, slab caches, runqueues minimize
  cross-CPU traffic
- **Lock contention:** MCS optimistic spinning, RCU for read-mostly paths,
  per-CPU locking
- **Memory bandwidth:** Huge pages (2MB/1GB) for kernel and userspace reduce
  TLB misses
- **Interrupt mitigation:** MSI/MSI-X per-queue, interrupt moderation (e1000
  ITR), softirq coalescing, NAPI polling
- **I/O efficiency:** I/O schedulers (deadline/CFQ), block layer merging,
  readahead, page cache, io_uring async I/O
- **Network performance:** RPS/RFS flow steering, XDP fast path, multi-queue RSS

### Build System Hardening

- `-Werror` in default CFLAGS — zero compiler warnings
- `-fstack-clash-protection` — stack clash attack mitigation
- `-z relro -z now` — full RELRO, GOT read-only after relocation
- `-fstack-protector-strong` — stack canary for functions with local buffers
- `-fstackleak` — kernel stack erasure on syscall exit
- Static analysis via `cppcheck` target
- `krealloc_array`/`kmalloc_array`/`kcalloc_array` wrappers — overflow-checked
  allocation for multiply-based sizes
- Stack canary per-task for kernel stack overflow detection

## Syscall Table

**File:** `src/kernel/syscall.c`, `src/include/syscall.h`

The kernel uses a **dual-numbering scheme** for system calls: native `SYS_*` constants and Linux-compatible `__NR_*` constants.

### Numbering Scheme

| Constant Type | Range | Purpose |
|---|---|---|
| `SYS_*` | 0–800+ | Primary kernel-internal native syscall numbers |
| `__NR_*` | 0–334 | Linux x86-64 ABI compatibility layer |

### Native Syscall Number Ranges (`SYS_*`)

| Range | Category | Description |
|---|---|---|
| 0–13 | Core ABI | read, write, open, close, exit, getpid, kill, brk, stat, mkdir, unlink, time, yield, uptime |
| 78 | Linux compat | getdents64 (Linux slot 78) |
| 100–137 | Extended FS/Net | ATA, AHCI, VFS operations, network status, PCI/USB list, DNS, ping, ARP |
| 138–146 | User/Session | find, add, delete, login, logout, session management |
| 147–149 | Hardware/Audio | RTC get time, speaker beep, ACPI shutdown |
| 150–154 | I/O & Memory | mouse, serial, CMOS, PMM stats |
| 155–161 | Specialized | ELF exec, script exec, FAT mount, kernel exec, module insert |
| 162–165 | Shell-core | history, readline, variables, exec |
| 166–167 | Display | VGA color, framebuffer info |
| 168 | Compiler | CC compile |
| 169–178 | Tmux isolation | keyboard, VGA, shell tab-complete, terminal, etc. |
| 179–182 | Heap | malloc, free, realloc, calloc |
| 183–189 | TCP server | listen, accept, send, recv, close, connect |
| 190–193 | Mutex | init, lock, unlock, destroy |
| 194–197 | Semaphore | init, wait, post, destroy |
| 198–200 | UDP server | listen, recv, unlisten |
| 201–203 | FS extended | symlink, readlink, lstat |
| 204–206 | Working dir | chdir, getcwd, setpriority |
| 207–210 | Shared mem IPC | shmget, shmat, shmdt, shmfree |
| 211 | Fork | fork |
| 212 | Connection list | net_connlist |
| 213 | Signal | signal handler registration |
| 214–215 | File ops | lseek, truncate |
| 216 | Raw Ethernet | raw ethernet send |
| 217–218 | FD-based I/O | fd_read, fd_write |
| 219–278 | Job control | priority, scheduling, process groups |
| 231–234 | Process/thread | clone, execve, gettid, tkill |
| 235–237 | Memory mapping | mmap, munmap, mprotect |
| 238–239 | CPU affinity | sched_setaffinity, sched_getaffinity |
| 240–242 | FD manipulation | dup, dup2, fcntl |
| 243 | I/O multiplexing | select |
| 244–245 | Per-process timers | setitimer, getitimer |
| 246 | Sleep | nanosleep |
| 247–248 | System config | sysconf, uname |
| 249–270 | FS/dir/process | pipe, ppid, alarm, access, chroot, etc. |
| 272–287 | Modern Linux compat | prlimit64, futex, poll, ppoll, pselect6, etc. |
| 288–294 | \*at family | openat, mkdirat, fstatat, unlinkat, renameat, linkat, symlinkat |
| 296–314 | Memory management | mlock, madvise, fallocate, membarrier, mremap, msync, etc. |
| 303–306 | Event/timer fds | timerfd_create, timerfd_settime, signalfd |
| 307–312 | Data transfer | splice, tee, sync, syncfs, vmsplice |
| 313–316 | Process mgmt | setsid, getsid, sigaltstack, rt_sigprocmask |
| 317–328 | BSD Socket API | socket, bind, listen, accept, connect, sendto, recvfrom, getsockname, getpeername, socketpair, shutdown, setsockopt, getsockopt |
| 329–332 | epoll | epoll_create1, epoll_ctl, epoll_wait, epoll_pwait |
| 333–340 | POSIX Clocks & Timers | clock_gettime, clock_settime, timer_create, timer_settime, timer_gettime, timer_getoverrun, timer_delete, clock_getres |
| 341–345 | Modern FD ops | dup3, pipe2, mkdtemp, utimensat, futimens |
| 346–349 | FS & system info | statfs, fstatfs, getrusage, sysinfo |
| 350–355 | Credentials & sched | getuid, geteuid, getgid, getegid, setresuid, setresgid |
| 356–359 | POSIX Message Queues | mq_open, mq_unlink, mq_timedsend, mq_timedreceive |
| 360–362 | CPU info, preadv/pwritev | sched_getcpu, preadv, pwritev |
| 363–364 | Signal wait | sigwaitinfo, sigtimedwait |
| 365 | memfd | memfd_create |
| 366–369 | Module syscalls | module_init, module_finit, module_delete, module_query |
| 370–374 | Misc | mremap, readahead, fadvise64, membarrier, pivot_root |
| 375–376 | Signal-safe I/O | pselect6, ppoll |
| 377–378 | chroot & copy | chroot, copy_file_range |
| 379–380 | File handles | name_to_handle_at, open_by_handle_at |
| 381–383 | inotify | inotify_init1, inotify_add_watch, inotify_rm_watch |
| 384 | userfaultfd | userfaultfd |
| 385–386 | Positional I/O | pread64, pwrite64 |
| 387 | vmsplice | vmsplice |
| 388 | clock_nanosleep | clock_nanosleep |
| 389–393 | NUMA memory policy | mbind, set_mempolicy, get_mempolicy, migrate_pages, move_pages |
| 394–396 | Namespace/rseq | unshare, setns, rseq |
| 397–399 | Extended sched | sched_setattr, sched_getattr, kcov |
| 400 | remap_file_pages | remap_file_pages |
| 401–407 | Credentials | setuid, seteuid, setgid, getgroups, setgroups, etc. |
| 424–428 | pidfd/io_uring/semctl | pidfd operations, io_uring, semctl |
| 434–438 | pidfd | pidfd_open, pidfd_send_signal, close_range, pidfd_getfd |
| 442 | mount_setattr | mount_setattr |
| 450–460 | D123: Signal & process | rt_sigaction, rt_sigpending, rt_sigtimedwait, wait4, waitid |
| 500–502 | Swap & mseal | swapon, swapoff, mseal |
| 503 | seccomp | seccomp (syscall filtering) |
| 504–507 | Framebuffer | put_pixel, blit, clear, refresh |
| 508–510 | Keyboard state | keyboard state queries |
| 511–513 | Legacy stubs | module/sysctl (return -ENOSYS) |
| 550–552 | Threading | thread_create, thread_join, thread_exit |
| 555–556 | ioprio | ioprio_set, ioprio_get |
| 570–571 | Capabilities | capget, capset |
| 572 | fdatasync | fdatasync |
| 573–575 | Robust list & shmctl | robust_list, set_robust_list, shmctl |
| 576 | pkey_mprotect | pkey_mprotect |
| 577–578 | securebits | set_securebits, get_securebits |
| 777 | posix_spawn | posix_spawn |
| 778 | kexec | kexec_load |
| 800 | msync | msync |

### Linux x86-64 Compat Syscall Numbers (`__NR_*`)

The kernel also implements the standard Linux x86-64 syscall ABI (numbers 0–334), dispatched via `syscall_linux_dispatch()` in `syscall.c`. These include the canonical Linux numbers for:

- **Standard I/O:** `__NR_read (0)`, `__NR_write (1)`, `__NR_open (2)`, `__NR_close (3)`, `__NR_stat (4)`, `__NR_fstat (5)`, `__NR_lstat (6)`, `__NR_poll (7)`, `__NR_lseek (8)`, `__NR_mmap (9)`, `__NR_mprotect (10)`, `__NR_munmap (11)`, `__NR_brk (12)`
- **Process:** `__NR_fork (57)`, `__NR_vfork (58)`, `__NR_execve (59)`, `__NR_exit (60)`, `__NR_wait4 (61)`, `__NR_clone (56)`, `__NR_getpid (39)`, `__NR_gettid (186)`
- **Signals:** `__NR_rt_sigaction (13)`, `__NR_rt_sigprocmask (14)`, `__NR_rt_sigreturn (15)`, `__NR_kill (62)`, `__NR_tkill (200)`, `__NR_sigaltstack (131)`
- **Time:** `__NR_nanosleep (35)`, `__NR_clock_gettime (228)`, `__NR_gettimeofday (96)`, `__NR_time (201)`
- **Network:** `__NR_socket (41)`, `__NR_connect (42)`, `__NR_accept (43)`, `__NR_sendto (44)`, `__NR_recvfrom (45)`, `__NR_sendmsg (46)`, `__NR_recvmsg (47)`, `__NR_shutdown (48)`, `__NR_bind (49)`, `__NR_listen (50)`, `__NR_getsockname (51)`, `__NR_getpeername (52)`, `__NR_setsockopt (54)`, `__NR_getsockopt (55)`
- **Filesystem:** `__NR_mkdir (83)`, `__NR_rmdir (84)`, `__NR_unlink (87)`, `__NR_link (86)`, `__NR_rename (82)`, `__NR_chdir (80)`, `__NR_getcwd (79)`, `__NR_mount (165)`, `__NR_umount2 (166)`, `__NR_openat (257)`, `__NR_mkdirat (258)`, `__NR_newfstatat (262)`, `__NR_unlinkat (263)`, `__NR_renameat (264)`, `__NR_linkat (265)`, `__NR_symlinkat (266)`, `__NR_readlinkat (267)`
- **Memory:** `__NR_mlock (149)`, `__NR_munlock (150)`, `__NR_mlockall (151)`, `__NR_munlockall (152)`, `__NR_mincore (218)`, `__NR_madvise (219)`, `__NR_remap_file_pages (219)`, `__NR_mbind (235)`, `__NR_set_mempolicy (236)`, `__NR_get_mempolicy (237)`
- **Modern Linux:** `__NR_eventfd2 (290)`, `__NR_epoll_create1 (291)`, `__NR_dup3 (292)`, `__NR_pipe2 (293)`, `__NR_inotify_init1 (294)`, `__NR_preadv (295)`, `__NR_pwritev (296)`, `__NR_sendmmsg (307)`, `__NR_getrandom (318)`, `__NR_memfd_create (319)`, `__NR_kexec_file_load (320)`, `__NR_pkey_alloc (330)`, `__NR_pkey_free (331)`, `__NR_pkey_mprotect (332)`

### Dispatch Flow

```
Userspace: SYSCALL instruction
     │
     ▼
  [MSR_LSTAR entry point]
     │
     ├─ KPTI trampoline (if CONFIG_KPTI) — switch to kernel page tables
     │
     ▼
  syscall_entry_full (syscall_asm.asm)
     │   Save all registers (push GPRs, save RSP, store in pt_regs)
     │   Switch to per-CPU kernel stack
     │   Stash 6th argument (via pselect6 ABI trick)
     ▼
  syscall_dispatch(num, a1, a2, a3, a4, a5)
     │
     ├─ 1. Seccomp filter evaluation (seccomp_evaluate_syscall)
     │       → ALLOW: continue
     │       → KILL: exit with SIGSYS
     │       → TRAP: notify tracer
     │       → ERRNO: return -errno
     │
     ├─ 2. Audit record generation (if audit enabled)
     │
     ├─ 3. KCOV trace recording
     │
     ├─ 4. Pointer validation (uaccess — check userspace addresses
     │      for syscalls that take pointers)
     │
     ├─ 5. Stackleak (erase kernel stack after syscall)
     │
     └─ 6. syscall_dispatch_internal(num, ...)
              │
              ├─ Native SYS_* dispatch (switch statement, ~200+ cases)
              │   Returns -ENOSYS for unrecognized numbers.
              │
              └─ Custom syscall registration (syscall_register/syscall_unregister)
                  Modules can register handlers in custom_syscall_table[256]
                  Checked first by syscall_table_lookup()
```

### Custom Syscall Registration

Kernel modules can register custom syscall handlers at runtime:

```c
int syscall_register(int nr, void *handler);
int syscall_unregister(int nr);
```

- **Table:** `custom_syscall_table[256]` — shared read-mostly array
- **Lookup:** checked in `syscall_table_lookup()` before the built-in switch dispatch
- **Constraints:** maximum 256 custom entries; registration fails with `-EBUSY` if slot is taken, `-ENOSPC` if nr ≥ 256

### Calling Convention

All syscalls follow the standard x86-64 syscall ABI:

| Register | Role |
|---|---|
| RAX | Syscall number |
| RDI | Argument 1 |
| RSI | Argument 2 |
| RDX | Argument 3 |
| R10 | Argument 4 |
| R8  | Argument 5 |
| R9  | Argument 6 |
| RCX | (clobbered — saved by SYSCALL instruction) |
| R11 | (clobbered — saved by SYSCALL instruction) |
| RAX (return) | Return value (negative = -errno on error) |

### Key Syscall Source Files

| File | Purpose |
|---|---|
| `src/kernel/syscall.c` | Main dispatch logic, syscall_dispatch(), syscall_dispatch_internal() |
| `src/kernel/syscall_asm.asm` | Assembly entry/exit trampolines (syscall_entry_full) |
| `src/kernel/syscall_linux.c` | Linux compat dispatch (__NR_* numbers) |
| `src/include/syscall.h` | All SYS_* and __NR_* constant definitions |
| `src/kernel/seccomp.c` | Seccomp-BPF filter evaluation on syscall entry |
