# OS Architecture

A 64-bit x86 hobby kernel (~47,000 lines of C + ASM). Boots via Multiboot1, runs on QEMU, and provides a Unix-like shell over telnet with filesystem, networking, process management, SMP, GUI, and more.

> **Architecture diagram:** [docs/architecture-diagram.html](docs/architecture-diagram.html) — interactive dark-themed SVG (open in browser).

---

## 1. Boot Flow

```
Multiboot1 (GRUB/QEMU -kernel)
    │  load ELF at 0x100000
    ▼
_start (boot.asm, 32-bit)
    │  verify magic 0x2BADB002
    │  set up identity-mapped page tables (2 MB huge pages, 1 GB)
    │  PML4[0] → identity map, PML4[256] → same PDPT (high half)
    │  enable PAE → enable long mode → enable paging
    │  load 64-bit GDT, far-jump to 64-bit code
    ▼
long_mode_entry (boot.asm, 64-bit)
    │  reload segments, clear BSS, set 16 KB bootstrap stack
    │  call kernel_main(multiboot_magic, multiboot_info_ptr)
    ▼
kernel_main() (kernel.c)
    │  strict linear init of every subsystem
    │  serial → VGA → GDT → PIC → IDT → fault → PMM → VMM →
    │  cpu_security_init (SMEP/SMAP/NXE/UMIP) → heap → framebuffer →
    │  process → scheduler → SMP/APIC → timer → drivers →
    │  syscall → production_subsystems (socket,epoll,mq,timers) →
    │  VFS → pipes → SHM → blockdev → ATA → AHCI → FAT32 → FS →
    │  service_mgr → PCI → Intel GPU → USB/MSC → users →
    │  virtio-net → virtio-blk → AC97 → e1000 → network →
    │  dhcp → telnetd → httpd → processes → sti
    ▼
Idle loop (PID 0)
    └─ hlt in a loop, wakes only on interrupts
```

### Init Sequence in `kernel_main()`

| # | Call | Purpose |
|---|------|---------|
| 1 | `serial_init()` | COM1 debug output (38400 baud) |
| 2 | `vga_init()` | 80×25 text mode |
| 3 | `gdt_init()` | 7-entry GDT + TSS (also for SMP APs) |
| 4 | `pic_init()` | Remap IRQs to INT 32–47 (fallback until APIC) |
| 5 | `idt_init()` | 32 exception + 16 IRQ handlers |
| 6 | `fault_init()` | Register page fault handler with guard-page detection |
| 7 | `pmm_init()` | Bitmap PMM from Multiboot memory map |
| 8 | `vmm_init()` | 4-level page table walker |
| 9 | `cpu_security_init()` | SMEP, SMAP, NXE, UMIP enable |
| 10 | `heap_init()` | Kernel heap (12 MB limit) |
| 11 | `vga_try_init_framebuffer()` | Multiboot framebuffer (1024×768, if available) |
| 12 | `process_init()` | Process table; idle = PID 0 |
| 13 | `scheduler_init()` | Ready queue (round-robin, 4 priority levels) |
| 14 | `smp_init_bsp()` | Per-CPU data for BSP |
| 15 | `apic_init_local()` | Local APIC (replaces PIC) |
| 16 | `smp_boot_aps()` | SIPI bringup of application processors |
| 17 | `timer_init()` | PIT at 100 Hz → APIC timer |
| 18 | `keyboard_init()` | PS/2 keyboard on IRQ1 |
| 19 | `rtc_init()` | CMOS RTC on IRQ8 |
| 20 | `mouse_init()` | PS/2 mouse on IRQ12 |
| 21 | `speaker_init()` | PC speaker via PIT channel 2 |
| 22 | `acpi_init()` | RSDP → RSDT → FADT; PM1a port |
| 23 | `syscall_init()` | MSR_LSTAR for SYSCALL/SYSRET |
| 24 | `production_subsystems_init()` | Socket, epoll, POSIX timers, mq |
| 25 | `vfs_init()` | VFS mount table |
| 26 | `pipe_init()` | 16 kernel pipes |
| 27 | `shm_init()` | Named shared memory segments |
| 28 | `blockdev_init()` | Block device registry |
| 29 | `ata_init()` | IDE PIO disk |
| 30 | `ahci_init()` | AHCI SATA disk |
| 31 | `fat32_mount()` / `vfs_mount()` | FAT32 on /mnt (if present) |
| 32 | `fs_init()` | SMFS superblock + inodes |
| 33 | `service_init()` | Service manager + FS directory tree |
| 34 | `pci_init()` | PCI bus enumeration |
| 35 | `intel_gpu_init()` | Intel integrated GPU detection |
| 36 | `usb_init()` / `usb_msc_init()` | USB EHCI + mass storage |
| 37 | `users_init()` | Multiuser database (root/guest) |
| 38 | `virtio_net_init()` | virtio-net NIC |
| 39 | `virtio_blk_init()` | virtio-blk storage |
| 40 | `ac97_init()` | AC97 audio |
| 41 | `e1000_init()` | Intel e1000 NIC (if present) |
| 42 | `net_init()` | TCP/IP stack |
| 43 | `net_dhcp_discover()` | DHCP |
| 44 | `service_register/start` | telnetd, httpd |
| 45 | `elf_exec()` autodetect | Userspace init binary |
| 46 | `process_create()` | shell, netd, gui, httpd tasks |
| 47 | `sti()` | Interrupts on → idle loop |

---

## 2. Memory Layout

### Physical Memory Map

```
0x000000 – 0x0FFFFF   Reserved (real mode, BIOS, VGA)
0x0B8000 – 0x0B8FA0   VGA text buffer (identity-mapped)
0x100000 – _kernel_end Kernel image (.text, .rodata, .data, .bss)
_kernel_end → +12 MB  Kernel heap (identity-mapped, on-demand growth)
+12 MB → 1 GB         Free physical frames (managed by PMM bitmap)
0xFEBC0000            e1000 MMIO BAR (typical QEMU address)
```

First 1 GB is identity-mapped via 2 MB huge pages. The same PDPT is referenced from both PML4[0] (low) and PML4[256] (high half at `0xFFFF800000000000`), so kernel code linked at high VMA can access physical memory directly via the identity map.

### Virtual Memory (4-Level Paging)

| Level | Bits | Entries | Page Size |
|-------|------|---------|-----------|
| PML4 | 39–47 | 512 | — |
| PDPT | 30–38 | 512 | 1 GB |
| PD | 21–29 | 512 | 2 MB |
| PT | 12–20 | 512 | 4 KB |

- Boot code pre-creates identity-mapped 2 MB pages for the first 1 GB
- VMM can add 4 KB mappings on demand (`vmm_map_page`, `vmm_unmap_page`)
- Per-process page tables for ring 3 isolation (with SMEP/SMAP enforcement)
- **NX bit** enforced: user `.data`/`.bss`/`.rodata` segments are non-executable
- **Guard pages** unmapped below kernel stacks and user stacks for overflow detection

### Kernel Heap

- **Location:** Immediately after `_kernel_end`, in identity-mapped region
- **Max size:** 12 MB, **Initial:** 16 KB (4 pages)
- **Algorithm:** First-fit with block splitting and coalescing
- **API:** `kmalloc(size)`, `kfree(ptr)` — 16-byte aligned, 32-byte block overhead

---

## 3. Subsystem Dependency Graph

```
shell ────────────────────────────────────────────────┐
  │  commands (137 cmd_*.c files)                      │
  ├─ editor, script, shell_vars                        │
  ├─ pipe ────── ipc (mutex, semaphore, shm) ──────────┤
  ├─ fat32 ────────────────────────────────────────────┤
  ├─ fs (SMFS, procfs, devfs) ── vfs (mount table) ────┤
  │       └─ ata / ahci / virtio_blk / usb_msc ───────┤
  │              └─ blockdev (sector I/O abstraction)   │
  ├─ net ────────── e1000 / virtio_net ────────────────┤
  │    ├─ net_tcp (TCP state machine)                  │
  │    ├─ net_udp (UDP, DHCP, DNS, HTTP client)        │
  │    ├─ telnetd (port 23)                            │
  │    └─ httpd (port 80, /tmp/www)                    │
  ├─ process ──── scheduler ──── timer ────────────────┤
  │    ├─ signal                                        │
  │    ├─ users (multiuser auth)                       │
  │    └─ syscall (SYSCALL/SYSRET, ring 3, 274 calls)  │
  ├─ elf (ELF64 loader → ring 3) ──────────────────────┤
  ├─ compiler (C compiler → native ELF64) ─────────────┤
  ├─ doom (raycast engine, framebuffer) ───────────────┤
  ├─ gui (windows, widgets, taskbar) ──────────────────┤
  ├─ dos (x86-16 emulator) ────────────────────────────┤
  └─ production (socket, epoll, POSIX timers, mq) ─────┘
           │
           ▼
       kernel (GDT, IDT, fault, service, SMP, APIC)
           │
           ▼
       memory (pmm → vmm → heap; cpu_security_init)
           │
           ▼
       drivers (vga, pic, timer, keyboard, serial, pci, ...)
           │
           ▼
       boot.asm (Multiboot → long mode)
```

**Dependency direction:** bottom → top. Each layer depends only on layers below it for initialization in `kernel_main()`, but subsystems are coupled through function calls at runtime.

---

## 4. Key Design Decisions

### `-mcmodel=large`

The kernel is linked at a high-half VMA (`0xFFFF800000000000 + text_base`). The `large` code model is required because the kernel image is smaller than 2 GB total but some symbols (e.g., page tables in `.boot` section at low physical addresses) are outside the 2 GB signed reach of the small/medium code model. With `large`, all code/data references use absolute 64-bit addresses instead of RIP-relative.

### SMFS (Simple Filesystem)

A custom flat filesystem designed for simplicity and minimal code. Uses a free-block bitmap for allocation, fixed inode table, and directory entries. No journaling, no complex b-trees. Lives on a 16 MB IDE disk image (`build/disk.img`). FAT12/16/32 support is additionally available via `fat mount` (read/write, VFS-mounted at `/mnt`).

### Ring 3 Separation

User processes run at Ring 3 with per-process page tables. The kernel uses:

- **SMEP** — prevents Ring 0 from jumping to Ring 3 code pages (ret2usr mitigation)
- **SMAP** — prevents Ring 0 from accidentally dereferencing user pointers (AC flag gating)
- **SYSCALL/SYSRET** (MSR_LSTAR) for fast ring transitions
- **TSS RSP0** per-process kernel stack for interrupt entry from ring 3
- **fork()** support: full page table + process struct copy
- **Per-process syscall capabilities** (bitmask limiting which syscalls a user process can call)
- **ELF loader** loads static 64-bit ELF binaries into ring 3 with NX page enforcement
- **User stack guard pages** — unmapped page below each ring 3 stack for overflow detection
- **Kernel stack guard pages** — unmapped page below each kernel stack

### SMP Support

The kernel supports SMP via:
- **Local APIC** replaces the legacy PIC for interrupt delivery
- **I/O APIC** for IRQ routing to all CPUs
- **SIPI bringup** of application processors (APs)
- **Per-CPU data** via `smp_get_cpu_id()` / per-CPU areas
- **Work stealing** — idle CPUs steal tasks from busy CPU run queues
- **CPU affinity** — `sched_setaffinity` for pinning processes to CPUs

### Polled Networking (No Interrupts)

The e1000 and virtio-net NICs operate in polled mode. A dedicated `netd` kernel task calls `net_poll()` in a tight loop, which checks RX descriptor rings. This avoids interrupt complexity at the cost of CPU usage during network activity.

### Identity-Mapped First 1 GB

The boot page tables map physical 0–1 GB to both low virtual (PML4[0]) and high half (PML4[256]). This means:
- Kernel code at high VMA can dereference physical addresses directly (e.g., VGA buffer at `0xB8000`)
- Device MMIO regions are accessible without special remapping
- The PMM bitmap can manage frames using physical addresses directly

### Production Subsystems

Socket, epoll, POSIX timers, and message queues are initialized as a batch after the basic kernel subsystems:

- **Socket API** — `socket()`, `bind()`, `listen()`, `accept()`, `connect()`, `sendmsg()`/`recvmsg()`, setsockopt/getsockopt
- **epoll** — `epoll_create1()`, `epoll_ctl()`, `epoll_wait()`/`epoll_pwait()`
- **POSIX timers** — `timer_create()`, `timer_settime()`, `timer_gettime()`, `timer_getoverrun()`, `timer_delete()`
- **Message queues** — `mq_open()`, `mq_send()`, `mq_receive()`, `mq_unlink()`

---

## 5. Build System

### Toolchain

| Tool | Default | Override |
|------|---------|----------|
| CC | `x86_64-elf-gcc` | env `CC` (e.g., `x86_64-linux-gnu-gcc` on CI) |
| AS | `nasm` | — |
| LD | `x86_64-elf-ld` | env `LD` |
| OBJCOPY | `x86_64-elf-objcopy` | env `OBJCOPY` |

`ccache` is auto-detected and wrapped around `$(CC)` for faster rebuilds. Parallel builds with `-j$(nproc)` are the default.

### Flags

- `-std=c17 -ffreestanding -nostdlib -nostdinc -fno-builtin`
- `-mno-red-zone -mno-mmx -mno-sse -mno-sse2` (no SSE in kernel)
- `-mcmodel=large` (see section 4)
- `-O2 -Wall -Wextra`
- Linker: `-T linker.ld -z max-page-size=0x1000 -z noexecstack`

### Makefile Targets

| Target | Description |
|--------|-------------|
| `all` (default) | Build `build/kernel.bin` (parallel, auto-dep tracking) |
| `run` | Boot in QEMU (VGA + e1000, macOS vmnet-shared) |
| `run-virtio` | Boot in QEMU with virtio-net NIC |
| `test` | Build test kernel + run in-kernel unit tests |
| `test-kernel` | Build test kernel binary (separate `build_test/` dir) |
| `e2e` | Build normal kernel + run E2E tests via telnet |
| `debug` | Boot with `-s -S` for GDB remote debugging |
| `clean` | Remove `build/` and `build_test/` |
| `format` | Run clang-format on all `.c`/`.h` files |
| `lint` | Run cppcheck static analysis |
| `deps` | Install toolchain (macOS Homebrew) |
| `doom-test` | Verify framebuffer renders non-black pixels |
| `e2e-port-N` | E2E with explicit telnet port |
| `test-serial` | Headless QEMU with serial on TCP `:4444` |

### Output

- `build/kernel.elf` → `build/kernel.bin` (normal kernel)
- `build_test/kernel.elf` → `build_test/kernel.bin` (test kernel, compiled with `-DTEST_MODE`)
- `build/disk.img` (16 MB raw disk image for SMFS)

---

## 6. Test Infrastructure

### In-Kernel Unit Tests

- **30+ tests** in `src/test/test.c`
- Built as `test-kernel` target (separate `build_test/` directory, `-DTEST_MODE` flag)
- Runs as a dedicated kernel process: exercises PMM, heap, timer, process, scheduler, VFS, pipes, signals, ATA, SMFS, networking, FAT32, SHM, mutex, semaphore, VMM, guard pages, ELF edge cases, etc.
- Outputs `[PASS]`/`[FAIL]` lines to serial console
- Test runner: `tests/run_tests.sh` launches QEMU headless (`-nographic`, `isa-debug-exit`), captures serial output, parses results
- Exits cleanly via `acpi_shutdown()` or `isa-debug-exit` (exit 33 = PASS, 16 = FAIL)
- CI: GitHub Actions `test` job (needs `build`), timeout 15 min, `SKIP_DISK_TESTS=1` for TCG speed

### E2E Tests (Telnet-Driven)

- **113 test groups** in `tests/e2e.py` (~2000 lines of Python)
- Connects to the kernel's telnet daemon (port 23) forwarded through QEMU user-mode networking
- Exercises every shell command: filesystem ops, networking, process control, pipes, permissions, etc.
- Test runner: `tests/e2e.sh` boots QEMU, waits for "Services started" marker, then runs `e2e.py`
- CI: GitHub Actions `e2e` job, timeout 15 min, `E2E_EXTERNAL_DNS=0` in CI

### CI Pipeline (`.github/workflows/ci.yml`)

| Job | Description |
|-----|-------------|
| `build` | Baseline build (`-Wall -Wextra`, ccache, parallel) |
| `build-strict` | Strict build with `-Werror` |
| `static-analysis` | cppcheck (informational) |
| `test` | In-kernel unit tests |
| `e2e` | Full E2E test suite over telnet |
| `virtio-net-smoke` | Quick smoke: QEMU + virtio-net, check serial for markers |
| `usb-fat-smoke` | Quick smoke: USB EHCI + FAT32 (main/master only) |
| `libc-test` | Host-side libc unit tests (ubuntu-latest) |
| `release` | GitHub Release on tag push (needs build, test, e2e) |

### Additional Tests

- **Host-side libc tests:** `tests/host_libc/` — compiles and runs libc helpers natively on the host
- **Doom framebuffer test:** `tests/doom_fb.sh` — verifies framebuffer pixel output
- **Serial test:** `test-serial` target — headless QEMU on TCP port 4444 for manual inspection

---

## 7. Code Organization

```
os/
├── Makefile                   Build system (parallel, ccache, -j$(nproc))
├── linker.ld                  Linker script (VMA = LMA + 0xFFFF800000000000)
│
├── src/
│   ├── boot/
│   │   └── boot.asm           Multiboot1 header, 32→64 bit transition, paging
│   │
│   ├── kernel/                Core kernel
│   │   ├── kernel.c           Main init sequence (kernel_main, 47 steps)
│   │   ├── gdt.c / gdt_asm.asm    GDT + TSS (BSP + AP trampoline)
│   │   ├── idt.c / idt_asm.asm    IDT, ISR/IRQ stubs
│   │   ├── syscall.c / syscall_asm.asm  SYSCALL/SYSRET (274 syscalls, 419 cases)
│   │   ├── vfs.c              Virtual filesystem layer (mount table)
│   │   ├── elf.c              ELF64 binary loader (NX enforcement)
│   │   ├── fault.c            CPU exception handlers (guard page detection)
│   │   ├── service.c          Service manager (start/stop services)
│   │   ├── smp.c              SMP boot (AP bringup via SIPI)
│   │   ├── apic.c             Local APIC + I/O APIC
│   │   └── cpu.c              CPU feature detection (SMEP/SMAP/NXE/UMIP)
│   │
│   ├── drivers/               Hardware drivers
│   │   ├── vga.c              80×25 text mode, framebuffer (1024×768)
│   │   ├── pic.c              8259 PIC (legacy, for pre-APIC init)
│   │   ├── timer.c            8254 PIT (100 Hz) → APIC timer
│   │   ├── keyboard.c         PS/2 keyboard (scancode set 1)
│   │   ├── serial.c           COM1 (38400 baud)
│   │   ├── ata.c              IDE PIO (LBA28)
│   │   ├── ahci.c             AHCI/SATA
│   │   ├── blockdev.c         Block device abstraction
│   │   ├── pci.c              PCI bus enumeration
│   │   ├── e1000.c            Intel 82540EM NIC (polled MMIO)
│   │   ├── virtio_net.c       virtio-net NIC
│   │   ├── virtio_blk.c       virtio-blk storage
│   │   ├── usb_ehci.c         USB EHCI controller
│   │   ├── usb_msc.c          USB mass storage (BOT)
│   │   ├── ac97.c             AC97 audio
│   │   ├── intel_gpu.c        Intel GPU detection
│   │   ├── rtc.c              CMOS RTC
│   │   ├── mouse.c            PS/2 mouse
│   │   ├── speaker.c          PC speaker (PIT channel 2)
│   │   └── acpi.c             ACPI (RSDP/RSDT/FADT, PM shutdown)
│   │
│   ├── memory/                Memory management
│   │   ├── pmm.c              Bitmap-based physical frame allocator
│   │   ├── vmm.c              4-level page table manager (hugepage split)
│   │   └── heap.c             First-fit kernel heap (12 MB max)
│   │
│   ├── process/               Process management
│   │   ├── process.c          Process table, fork, caps
│   │   ├── scheduler.c        Round-robin scheduler (4 levels, work stealing)
│   │   ├── switch.asm         Context switch (cli-protected)
│   │   ├── signal.c           POSIX-like signal delivery
│   │   └── users.c            User/group database, passwords
│   │
│   ├── fs/                    Filesystems
│   │   ├── fs.c               SMFS implementation
│   │   ├── fat32.c            FAT12/16/32 read/write driver
│   │   ├── procfs.c           /proc virtual filesystem
│   │   └── devfs.c            /dev virtual filesystem
│   │
│   ├── net/                   Networking stack
│   │   ├── net.c              Core: ETH/ARP/IP/ICMP, poll loop
│   │   ├── net_tcp.c          TCP state machine (connect, listen, send)
│   │   ├── net_udp.c          UDP, DHCP, DNS, HTTP client
│   │   ├── telnetd.c          Telnet server (port 23)
│   │   ├── httpd.c            HTTP server (port 80, /tmp/www)
│   │   └── socket.c           BSD socket layer (syscall backend)
│   │
│   ├── ipc/                   Inter-process communication
│   │   ├── pipe.c             Blocking circular-buffer pipes (16 max)
│   │   ├── shm.c              Named shared memory segments
│   │   ├── mutex.c            Kernel mutexes
│   │   └── semaphore.c        Counting semaphores
│   │
│   ├── production/            High-level subsystem stubs
│   │   └── production.c       Production subsystems init (socket, epoll, mq, timers)
│   │
│   ├── shell/                 Interactive shell
│   │   ├── shell.c            Core: input, dispatch, history, background
│   │   ├── shell_cmd_table.c  Command dispatch table
│   │   ├── shell_vars.c       Variable expansion ($VAR)
│   │   ├── editor.c           vi-like text editor
│   │   ├── script.c           Script runner
│   │   └── cmds/              137 cmd_*.c files (one per command)
│   │
│   ├── compiler/              C compiler (runs inside the OS)
│   │   ├── cc_lex.c           Lexer / tokenizer
│   │   ├── cc_parse.c         Parser + code generator
│   │   ├── cc_elf.c           ELF64 binary output
│   │   ├── cc_link.c          Linker support
│   │   └── cc_obj.c           Object file support
│   │
│   ├── gui/                   GUI framework
│   │   ├── gui.c / gui.h           Window management, widgets, rendering
│   │   ├── gui_widgets.c / .h      FileBrowser, Taskbar
│   │   ├── gui_shell.c / .h        Terminal emulator pane
│   │   └── gui_task.c              Desktop kernel process
│   │
│   ├── doom/                  Doom-like raycast engine
│   │   ├── doom.h                 Types and constants
│   │   └── doom_*.c (12 files)    Combat, doors, raycast, render, etc.
│   │
│   ├── dos/                   x86-16 DOS emulator
│   │   ├── dos_emu.c              Emulator core
│   │   ├── dos_ints.c             INT 10h/16h emulation
│   │   ├── dos_int21.c            INT 21h DOS API
│   │   └── dos_load.c             .COM / MZ loader
│   │
│   ├── lib/                   Kernel utility library
│   │   ├── string.c           memcpy, memset, strlen, strcmp, etc.
│   │   ├── printf.c           kprintf (VGA + serial output hook, -Wformat)
│   │   ├── stdlib.c           atoi, itoa, rand, etc.
│   │   └── libc.c             Libc shim for userspace ELF programs
│   │
│   ├── test/
│   │   └── test.c             In-kernel test suite (~30+ tests)
│   │
│   └── include/               Header files (72 .h files)
│       ├── types.h            Base types + macros
│       ├── libc.h             Userspace libc header
│       ├── syscall.h          274 syscall numbers
│       ├── shell.h, shell_cmds.h
│       ├── socket.h           BSD socket types
│       ├── epoll.h            epoll types
│       ├── timer.h            POSIX timer types
│       └── ...                One header per subsystem
│
├── tests/                     Test infrastructure
│   ├── e2e.sh                 E2E harness (launches QEMU, waits for boot)
│   ├── e2e.py                 113 test groups via telnet (~2000 lines)
│   ├── run_tests.sh           In-kernel test runner (serial capture)
│   ├── host_libc/             Host-side libc tests
│   └── doom_fb.sh             Framebuffer validation
│
├── docs/
│   └── architecture-diagram.html  Interactive dark-themed SVG
│
├── .github/workflows/
│   └── ci.yml                 GitHub Actions CI (9 jobs incl. release)
│
├── README.md                  Project overview and quick start
├── ARCHITECTURE.md            This file
├── TODO.md                    Process isolation roadmap
├── IMPROVEMENTS.md            Feature tracking
├── DEBUGGING.md               GDB/QEMU debug reference
└── linker.ld                  Linker script
```


## Architecture Diagram

An interactive SVG architecture diagram is available at
[docs/architecture-diagram.html](docs/architecture-diagram.html).
Open it in any browser to explore the system layout.
