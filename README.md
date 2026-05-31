# OS — A 64-bit x86 Hobby Kernel

[![CI](https://github.com/rusik69/os/actions/workflows/ci.yml/badge.svg)](https://github.com/rusik69/os/actions/workflows/ci.yml)

A hobby operating system kernel written from scratch in C and x86-64 assembly.
Boots via Multiboot1, runs on QEMU, and provides a Unix-like shell accessible
over a telnet connection with filesystem, networking, process management, SMP,
GUI, and more.

- **Source lines:** ~47,000 (C + ASM + headers)
- **Files:** 226 C sources, 72 headers, 1 assembly boot
- **Syscalls:** 274+ (419 switch cases)
- **Shell commands:** 137
- **In-kernel tests:** 30+ | **E2E tests:** 113 groups
- **CI jobs:** 8 (build, strict, static analysis, unit tests, E2E, smoke tests, libc)

## Quick Start

```bash
# Install toolchain (macOS)
make deps

# Build kernel
make

# Run in QEMU (with VGA console + e1000 NIC)
make run

# Run in-kernel unit tests
make test

# Run E2E tests over telnet
make e2e

# Run with virtio-net NIC (QEMU)
make run-virtio
```

Set `E2E_EXTERNAL_DNS=1` to enable external hostname ping in E2E (off by default in CI).

## Key Docs

- **[ARCHITECTURE.md](ARCHITECTURE.md)** — boot flow, init sequence, memory layout, subsystems, build system
- **[TODO.md](TODO.md)** — Process isolation roadmap (SMEP/SMAP done, identity map removal pending)
- **[IMPROVEMENTS.md](IMPROVEMENTS.md)** — Feature history and remaining improvement opportunities
- **[DEBUGGING.md](DEBUGGING.md)** — GDB/QEMU debug reference

## Security

If running a self-hosted GitHub Actions runner, see
[scripts/runner-hardening/](scripts/runner-hardening/) for network isolation,
filesystem protection, and process hardening scripts.

## Features

### Memory Protection
- **SMEP** — Supervisor Mode Execution Prevention (CR4.SMEP)
- **SMAP** — Supervisor Mode Access Prevention (CR4.SMAP, AC flag gating)
- **NXE** — No-Execute bit on all user mappings
- **UMIP** — User Mode Instruction Prevention (blocks SGDT/SIDT/SLDT/SMSW/STR)
- **Kernel stack guard pages** — unmapped page below each kernel stack
- **User stack guard pages** — unmapped page below each ring 3 stack
- **NX page enforcement** — ELF segments get appropriate X/W/R per `p_flags`

### Process Management
- **Preemptive multitasking** — round-robin with 4 priority levels
- **SMP support** — AP bringup via SIPI, local+IO APIC, work stealing, CPU affinity
- **Kernel/User separation (Ring 3)** — per-process page tables, SYSCALL/SYSRET, TSS RSP0, fork()
- **Background processes & job control** — `&` operator, `jobs`, `fg`, `wait`
- **Blocking sleep** — timer-based process wakeup, no busy-wait
- **Zombie reaping** — automatic cleanup of terminated processes
- **Signals** — SIGKILL, SIGTERM, SIGSTOP/SIGCONT, SIGUSR1/2, custom handlers
- **Wait queues, RW-locks, completions** — kernel synchronization primitives
- **Per-process syscall capabilities** — bitmask-based syscall whitelist

### Syscalls (274+, 60+ production-grade calls)
- **File I/O:** open, read, write, close, lseek, pread64/pwrite64, readv/writev, dup/dup2/dup3, fcntl, fsync, stat, fstat
- **Directory ops:** mkdir, rmdir, getdents64, chdir, getcwd
- **Filesystem ops:** mount, umount, chmod, chown, rename, link, symlink, readlink, mknod, access, utimensat, futimens
- ***at family:** openat, mkdirat, renameat, linkat, symlinkat, readlinkat, fchownat, fstatat, unlinkat
- **Networking:** socket, bind, listen, accept, connect, sendmsg/recvmsg, setsockopt/getsockopt, getsockname/getpeername, socketpair
- **I/O multiplexing:** epoll_create1, epoll_ctl, epoll_wait, epoll_pwait
- **POSIX timers:** timer_create, timer_settime, timer_gettime, timer_getoverrun, timer_delete, clock_gettime/getres/settime, getitimer/setitimer
- **Message queues:** mq_open, mq_send, mq_receive, mq_unlink
- **Process control:** fork, execve, exit, getpid, getppid, waitpid, nanosleep, alarm, pause, getrusage
- **Signals:** sigprocmask, sigaltstack, kill, tkill, signal
- **Memory:** mmap, munmap, mprotect, brk, madvise
- **File descriptor ops:** pipe/pipe2, select/pselect, poll, splice/vmsplice/tee
- **Timers:** timerfd_create, timerfd_gettime, timerfd_settime
- **Signal:** signalfd
- **System:** sysinfo, uname, sysconf, sched_getparam/sched_setparam, sched_setaffinity, getrandom, reboot, personality, setsid, gettid
- **UID/GID:** getuid, geteuid, getgid, getegid, getresuid/setresuid, getresgid/setresgid
- **Hostname:** sethostname, gethostname
- **Misc:** umask, mkdtemp, sysinfo, syslog

### Networking
- **TCP/IP stack** — ARP, ICMP, IP, TCP, UDP, DNS
- **DHCP** — auto-configuration on boot
- **Telnet server** on port 23 — remote shell access
- **HTTP server** — document root at `/tmp/www`
- **Socket API** — BSD-compatible socket/sendmsg/recvmsg interface
- **epoll** — scalable I/O event notification
- **IP fragmentation** — TX fragmentation for large payloads

### Filesystems
- **SMFS** — Simple custom filesystem on IDE disk (16 MB), free-block bitmap
- **FAT12/FAT16/FAT32** — auto-detected read/write via `fat mount`; VFS mount at `/mnt`
- **procfs** — /proc filesystem (cpu, meminfo, uptime, mounts, version, processes)
- **devfs** — /dev device filesystem
- **VFS layer** — mount table supporting multiple filesystem types

### Storage
- **ATA/IDE** — PIO mode, LBA28
- **AHCI/SATA** — native SATA controller support
- **virtio-blk** — paravirtualized block device
- **USB mass storage** — EHCI + BOT protocol
- **Block device abstraction** — unified sector I/O layer

### IPC
- **Pipes** — 16 blocking circular-buffer pipes
- **Shared memory** — named segments shared between processes
- **Mutexes & semaphores** — kernel synchronization primitives

### Shell (~137 built-in commands)
- Command history, tab completion, pipes, redirection, background (`&`)
- Shell variables (`$VAR` expansion), aliases
- Script runner with looping and conditionals
- vi-like text editor
- Terminal multiplexer (tmux) — split panes, Ctrl-B prefix

### Drivers
- VGA text mode + framebuffer (1024×768 RGB rendering)
- PS/2 keyboard & mouse
- PIT timer + RTC
- Serial (COM1, 38400 baud)
- PCI bus enumeration
- e1000 (Intel 82540EM) and virtio-net NICs
- ATA/AHCI, virtio-blk, USB MSC
- AC97 audio
- PC speaker
- ACPI (power management, shutdown)
- Intel GPU detection

### Other Subsystems
- **ELF64 loader** — loads static 64-bit ELF binaries in ring 3 with NX enforcement
- **C compiler** — single-pass recursive descent, outputs native x86-64 ELF64 binaries
- **x86-16 DOS emulator** — runs .COM and MZ executables (INT 21h/10h/16h)
- **Doom-like raycast engine** — `doom` shell command (WASD + mouse)
- **GUI framework** — window system, widgets, GUI terminal, mouse cursor
- **Multiuser auth** — password-backed users (root/guest), home directories
- **Unix permissions** — owner/group/other rwx bits

### Testing & CI
- **8 CI jobs** — build, build-strict (Werror), cppcheck, unit tests, E2E, virtio-net smoke, USB+FAT smoke, libc-test
- **ccache** caching across all jobs
- **Parallel builds** `-j$(nproc)` everywhere
- **Release automation** — GitHub Release on `v*` tag push
- **isa-debug-exit** — reliable test VM shutdown

## Multiuser Notes

- Default accounts: `root/root` and `guest/guest`.
- Successful `login` sets shell variables `HOME` and `PWD` to the user's home path.
- File ownership and permissions are enforced by uid/gid + rwx mode bits.

## Toolchain Notes

- Build mode uses strict C17 (`-std=c17`) in freestanding kernel context.
- Makefile auto-selects cross tools on macOS; override with `CC=x86_64-linux-gnu-gcc` on Linux.
- `ccache` auto-detected for faster rebuilds.

## Project Structure

```
os/
├── Makefile              Build system (parallel, ccache)
├── linker.ld             Linker script (load at 0x100000)
├── src/
│   ├── boot/             Multiboot1 entry, 32→64 bit
│   ├── kernel/           Core: kernel_main, GDT, IDT, syscall (274+), VFS, ELF, SMP, APIC
│   ├── drivers/          VGA, PIC, timer, keyboard, serial, ATA, AHCI, e1000, virtio, USB, AC97, ACPI...
│   ├── memory/           PMM, VMM, heap
│   ├── process/          Process table, scheduler, context switch, signals, users
│   ├── fs/               SMFS, FAT32, procfs, devfs
│   ├── net/              TCP/IP, UDP, socket API, telnetd, httpd
│   ├── ipc/              Pipe, SHM, mutex, semaphore
│   ├── production/       Socket/epoll/mq/POSIX timer subsystem stubs
│   ├── shell/            137 commands, editor, script runner, tmux
│   ├── compiler/         C compiler → native ELF64
│   ├── gui/              Window system, widgets, terminal pane, taskbar
│   ├── doom/             Raycast engine (doom shell command)
│   ├── dos/              x86-16 DOS emulator
│   ├── lib/              string, printf, stdlib, libc shim
│   ├── test/             In-kernel test suite
│   └── include/          72 header files
├── tests/                E2E, unit test runners, host libc tests
├── docs/                 Architecture diagram (SVG)
└── .github/workflows/    CI pipeline (9 jobs)
```

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full subsystem map and init sequence.

## Architecture

### Boot Process

The kernel boots via the **Multiboot1** protocol (magic `0x1BADB002`).
The bootloader (GRUB or QEMU `-kernel`) loads the ELF binary at physical
address **0x100000** (1 MB).

**boot.asm** performs the 32-bit → 64-bit transition:

1. Verify Multiboot magic in EAX
2. Set up identity-mapped page tables using **2 MB huge pages** for the
   first 1 GB of physical memory (PML4 → PDPT → PD)
3. Enable **PAE** (CR4 bit 5)
4. Enable **long mode** (MSR `IA32_EFER` bit 8)
5. Enable **paging** (CR0 bit 31)
6. Load a minimal 64-bit GDT and far-jump to 64-bit code
7. Clear BSS, set up a 16 KB bootstrap stack
8. Call `kernel_main(multiboot_magic, multiboot_info_ptr)`

### Kernel Initialization Sequence

`kernel_main()` in [src/kernel/kernel.c](src/kernel/kernel.c) initializes
every subsystem in a strict 47-step order. See [ARCHITECTURE.md](ARCHITECTURE.md) for
the complete sequence.

### Memory Layout

```
Physical Memory Map
───────────────────────────────────────
0x000000 - 0x0FFFFF   Reserved (real mode, BIOS, VGA)
0x0B8000 - 0x0B8FA0   VGA text buffer (identity-mapped)
0x100000 - kernel_end  Kernel image (.text, .rodata, .data, .bss)
kernel_end → +12MB     Kernel heap (identity-mapped, on demand)
1MB frames above       Free physical frames (managed by PMM)
0xFEBC0000             E1000 MMIO BAR (typical QEMU address)
───────────────────────────────────────
Virtual = Physical for the first 1 GB (identity map via 2 MB huge pages)
```

### Memory Management

#### Physical Memory Manager (PMM)

Bitmap-based frame allocator. Each bit represents one 4 KB physical frame.

| Constant | Value |
|----------|-------|
| Frame size | 4 KB |
| Max frames | 262144 (1 GB) |
| Bitmap size | 32 KB |
| Allocation | First-fit scan |

**API:**
- `pmm_alloc_frame()` → physical address (or 0 on failure)
- `pmm_free_frame(addr)` — release a frame
- `pmm_unref_frame(addr)` — release COW-shared frame (refcount aware)
- `pmm_reserve_frames(start, size)` — mark a range as used
- `pmm_get_total_frames()` / `pmm_get_used_frames()` — statistics

#### Virtual Memory Manager (VMM)

Standard x86-64 **4-level page tables** (PML4 → PDPT → PD → PT).
The boot code pre-creates identity-mapped 2 MB huge pages for the first
1 GB. The VMM can add 4 KB mappings on demand.

Supports hugepage splitting: automatically splits 2 MB pages into 4 KB
page tables when smaller mappings are needed.

**Security integration:**
- NXE bit set on user page table entries (no-execute for data pages)
- SMEP prevents kernel execution of user pages
- SMAP requires AC flag clearance for kernel user-memory access

**API:**
- `vmm_map_page(virt, phys, flags)` — create mapping; allocates intermediate tables
- `vmm_unmap_page(virt)` — remove mapping, invalidate TLB
- `vmm_get_physaddr(virt)` — translate virtual to physical (handles 2 MB entries)

#### Kernel Heap

First-fit allocator with block splitting and free-block coalescing.
Lives in the identity-mapped region immediately after the kernel image.

| Parameter | Value |
|-----------|-------|
| Start | `_kernel_end` aligned to 4 KB |
| Max size | 12 MB |
| Initial size | 16 KB (4 pages) |
| Alignment | 16 bytes |
| Block overhead | 32 bytes (`size` + `free` + `next` + `prev`) |

**API:**
- `kmalloc(size)` — allocate; splits oversized blocks
- `kfree(ptr)` — deallocate; coalesces with next free block
- `heap_get_total()` / `heap_get_used()` / `heap_get_free()` — heap statistics

### Process Management

#### Process Model

Up to **256 processes** in a flat table. Each process has a 128 KB kernel
stack with a guard page for overflow detection.

```c
struct process {
    uint32_t           pid;
    enum process_state state;     // UNUSED, READY, RUNNING, BLOCKED, ZOMBIE
    uint64_t           kernel_stack;
    uint64_t           stack_top;
    struct cpu_context *context;  // Saved registers on stack
    struct process     *next;     // Ready-queue link
    const char         *name;
    /* Signal state */
    uint32_t           pending_signals;
    uint32_t           sig_mask;
    signal_handler_t   sig_handlers[32];
    /* Ring 3 support */
    int                is_user;        // 1 = ring 3 process
    uint64_t           user_entry;     // Ring 3 entry point
    uint64_t           user_rsp;       // Ring 3 stack pointer
    uint64_t          *pml4;           // Per-process page table
    /* Multitasking */
    uint32_t           parent_pid;
    uint32_t           pgid;
    uint32_t           sid;
    int                exit_code;
    uint64_t           sleep_until;
    int                is_background;
    int                is_suspended;
    uint8_t            priority;       // 0=high .. 3=low
    uint64_t           syscall_caps[4];
    char               cwd[64];
    uint32_t           wait_for_pid;
    uint16_t           ticks_remaining;
    uint64_t           last_run_tick;
    struct process_fd  fd_table[16];   // Per-process file descriptors
};
```

**States:**
- `READY` — in the scheduler's ready queue
- `RUNNING` — currently executing (one at a time)
- `BLOCKED` — waiting (e.g., SIGSTOP, pipe)
- `ZOMBIE` — terminated, awaiting cleanup

#### Scheduler

**Algorithm:** Round-robin with 4 priority levels and work stealing.

The PIT timer (or APIC timer) fires at 100 Hz. Every **5 ticks (50 ms)**,
the timer handler calls `schedule()`, which picks the next READY process
from the head of the highest-priority non-empty queue.

The scheduler supports **work stealing**: idle CPUs steal tasks from busy
CPU run queues when their own queues are empty.

Processes can also yield voluntarily via `scheduler_yield()`.

#### Context Switch

Implemented in [src/process/switch.asm](src/process/switch.asm):

```
context_switch(old_ctx_ptr, new_ctx):
    push rbp, rbx, r12-r15        save callee-saved registers
    *old_ctx_ptr = rsp             save current stack pointer
    rsp = new_ctx                  load new stack pointer
    pop r15-r12, rbx, rbp          restore registers
    ret                            jump to saved return address
```

#### Signals

POSIX-like signals with a pending bitmask per process:

| Signal | # | Default Action |
|--------|---|----------------|
| SIGKILL | 9 | Terminate (cannot be caught) |
| SIGTERM | 15 | Terminate |
| SIGSTOP | 19 | Block process |
| SIGCONT | 18 | Resume blocked process |
| SIGUSR1 | 10 | Terminate |
| SIGUSR2 | 12 | Terminate |

### Drivers

#### SMP & APIC
- **Local APIC** replaces legacy PIC for interrupt delivery
- **I/O APIC** routes IRQs to any CPU
- **SIPI bringup** of application processors
- **Per-CPU data** areas for cross-CPU isolation

#### VGA Text Console
80×25 text mode at `0xB8000`. Supports 16-color palette, hardware cursor,
scrolling, tabs, backspace. `kprintf()` output goes to both VGA and serial
simultaneously. Framebuffer console (1024×768 RGB) when available via
Multiboot.

#### 8259 PIC / Local APIC
Legacy dual cascaded PICs remap IRQ 0–7 → INT 32–39, IRQ 8–15 → INT 40–47.
The Local APIC replaces this after initialization.

#### PIT Timer
Channel 0 of the Intel 8254 at **100 Hz** (divisor 11932). IRQ0 (INT 32)
increments a 64-bit tick counter and triggers preemptive scheduling.

#### PS/2 Keyboard
Scancode set 1 on port `0x60`. IRQ1 (INT 33). A 256-byte ring buffer
stores keypresses. Arrow keys mapped to codes `0x80–0x83`.

#### PS/2 Mouse
3-byte PS/2 mouse packets via IRQ12 (INT 44). Tracks X/Y position
and 3-button state (left, middle, right).

#### Serial Port
COM1 (`0x3F8`) at **38400 baud**, 8N1 format with FIFO enabled.
Used for kernel debug output and test harness communication.

#### ATA/IDE Disk
Primary IDE bus at ports `0x1F0–0x1F7`. **PIO mode** with LBA28 addressing.
512-byte sectors. 16 MB disk image (`build/disk.img`).

#### PCI Bus
Type 1 PCI configuration space access (ports `0xCF8`/`0xCFC`).
Enumerates bus 0–255, slot 0–31 to find devices.

#### Intel e1000 NIC
