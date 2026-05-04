# OS — A 64-bit x86 Hobby Kernel

[![CI](https://github.com/rusik69/os/actions/workflows/ci.yml/badge.svg)](https://github.com/rusik69/os/actions/workflows/ci.yml)

A minimal operating system kernel written from scratch in C and x86-64 assembly.
Boots via Multiboot1, runs on QEMU, and provides a Unix-like shell accessible
over a telnet connection with filesystem, networking, process management, and more.

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
```

## Features

- **64-bit long mode** with Multiboot1 boot, identity-mapped first 1 GB
- **Preemptive multitasking** — round-robin scheduler with 50 ms time slices
- **Kernel/User separation (Ring 3)** — per-process page tables, SYSCALL/SYSRET, TSS RSP0
- **Background processes & job control** — `&` operator, `jobs`, `fg`, `wait`
- **Blocking sleep** — timer-based process wakeup, no busy-wait
- **Zombie reaping** — automatic cleanup of terminated processes
- **TCP/IP networking** — DHCP, ARP, ICMP, TCP, UDP, DNS
- **Telnet server** on port 23 — remote shell access
- **Simple filesystem (SMFS)** — on an IDE disk with directories and files
- **VFS layer** with mount table
- **IPC via pipes** — 16 blocking circular-buffer pipes
- **Signal delivery** — SIGKILL, SIGTERM, SIGSTOP/SIGCONT, user signals
- **ELF loader** — load and run static 64-bit ELF binaries in ring 3
- **C compiler** — single-pass recursive descent, outputs native x86-64 ELF64 binaries
- **Terminal multiplexer (tmux)** — split panes, Ctrl-B prefix key bindings
- **Shell** — 60+ built-in commands, command history, tab completion, pipes, redirection
- **Drivers** — VGA text mode, PS/2 keyboard & mouse, PIT timer, RTC,
  serial (COM1), ATA/IDE disk, PCI bus, Intel e1000 NIC, PC speaker, ACPI
- **CI** — GitHub Actions with unit tests and full E2E test suite

## Project Structure

```
os/
├── Makefile              Build system
├── linker.ld             Linker script (load at 0x100000)
├── src/
│   ├── boot/
│   │   └── boot.asm      Multiboot1 entry, 32→64 bit transition, paging
│   ├── kernel/
│   │   ├── kernel.c       Main init sequence
│   │   ├── gdt.c          Global Descriptor Table + TSS
│   │   ├── gdt_asm.asm    GDT/TSS load
│   │   ├── idt.c          Interrupt Descriptor Table
│   │   ├── idt_asm.asm    ISR/IRQ stubs
│   │   ├── syscall.c      SYSCALL/SYSRET interface
│   │   ├── syscall_asm.asm Syscall entry point
│   │   ├── vfs.c          Virtual filesystem layer
│   │   └── elf.c          ELF64 binary loader
│   ├── drivers/
│   │   ├── vga.c          80×25 text mode console
│   │   ├── pic.c          8259 PIC (IRQ remapping)
│   │   ├── timer.c        8254 PIT at 100 Hz
│   │   ├── keyboard.c     PS/2 keyboard (scancode set 1)
│   │   ├── serial.c       COM1 at 38400 baud
│   │   ├── ata.c          IDE disk (PIO, LBA28)
│   │   ├── pci.c          PCI bus enumeration
│   │   ├── e1000.c        Intel 82540EM NIC (polled MMIO)
│   │   ├── rtc.c          CMOS real-time clock
│   │   ├── mouse.c        PS/2 mouse (3-byte packets)
│   │   ├── speaker.c      PC speaker (PIT channel 2)
│   │   └── acpi.c         RSDP/RSDT/FADT, PM shutdown
│   ├── memory/
│   │   ├── pmm.c          Physical memory manager (bitmap)
│   │   ├── vmm.c          Virtual memory manager (4-level paging)
│   │   └── heap.c         Kernel heap (first-fit, 12 MB max)
│   ├── process/
│   │   ├── process.c      Process table, creation, exit
│   │   ├── scheduler.c    Round-robin scheduler
│   │   ├── switch.asm      Context switch (cli-protected)
│   │   └── signal.c       POSIX-like signal delivery
│   ├── fs/
│   │   └── fs.c           SMFS filesystem implementation
│   ├── net/
│   │   ├── net.c          Core networking (ETH/ARP/IP/ICMP, poll loop)
│   │   ├── net_tcp.c      TCP state machine, connect, listen, send
│   │   ├── net_udp.c      UDP, DHCP, DNS, HTTP client
│   │   └── telnetd.c      Telnet server (port 23)
│   ├── ipc/
│   │   └── pipe.c         Inter-process pipes
│   ├── shell/
│   │   ├── shell.c        Shell core (input, dispatch, history, background)
│   │   ├── editor.c       Text editor (vi-like)
│   │   ├── script.c       Script runner with variables
│   │   └── cmds/          One file per command (cmd_*.c)
│   ├── compiler/
│   │   ├── cc_lex.c       Lexer (tokenizer)
│   │   ├── cc_parse.c     Parser + code generator
│   │   └── cc_elf.c       ELF64 binary output
│   ├── lib/
│   │   ├── string.c       String/memory utilities
│   │   └── printf.c       kprintf with output hook
│   ├── test/
│   │   └── test.c         In-kernel test suite (95 tests)
│   └── include/           Header files
├── tests/
│   ├── e2e.sh             E2E test harness (QEMU boot + telnet)
│   ├── e2e.py             E2E test suite (110+ tests via telnet)
│   └── run_tests.sh       In-kernel test runner
└── .github/workflows/
    └── ci.yml             GitHub Actions CI pipeline
```

---

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
every subsystem in a strict order:

| # | Call | Purpose |
|---|------|---------|
| 1 | `serial_init()` | COM1 at 38400 baud (debug output) |
| 2 | `vga_init()` | 80×25 text mode, cursor enabled |
| 3 | `gdt_init()` | 7-entry GDT with TSS |
| 4 | `pic_init()` | Remap IRQs to INT 32–47 |
| 5 | `idt_init()` | 32 exception + 16 IRQ handlers |
| 6 | `pmm_init(mboot)` | Bitmap allocator from Multiboot memory map |
| 7 | `vmm_init()` | 4-level page table walker |
| 8 | `heap_init()` | Kernel heap (12 MB limit) |
| 9 | `process_init()` | Process table; idle process = PID 0 |
| 10 | `scheduler_init()` | Ready queue (round-robin) |
| 11 | `timer_init()` | PIT at 100 Hz; preemption every 5 ticks |
| 12 | `keyboard_init()` | PS/2 keyboard on IRQ1 |
| 13 | `rtc_init()` | CMOS RTC on IRQ8 |
| 14 | `mouse_init()` | PS/2 mouse on IRQ12 |
| 15 | `speaker_init()` | PC speaker via PIT channel 2 |
| 16 | `acpi_init()` | RSDP → RSDT → FADT; PM1a port |
| 17 | `syscall_init()` | MSR_LSTAR for SYSCALL/SYSRET |
| 18 | `vfs_init()` | Mount SMFS at "/" |
| 19 | `pipe_init()` | 16 kernel pipes |
| 20 | `ata_init()` | IDE PIO disk detection |
| 21 | `fs_init()` | Load SMFS superblock + inodes |
| 22 | `pci_init()` | PCI bus enumeration |
| 23 | `e1000_init()` | Intel e1000 NIC (if present) |
| 24 | `net_init()` | TCP/IP stack |
| 25 | `net_dhcp_discover()` | Obtain IP via DHCP |
| 26 | `telnetd_init()` | Telnet server on port 23 |
| 27 | Create processes | `task_a`, `task_b`, `shell`, `netd` |
| 28 | `sti()` | Enable interrupts |
| 29 | Idle loop | `hlt` in a loop |

After initialization the boot thread becomes the **idle process** (PID 0)
which executes `hlt` in a loop, only waking on interrupts.

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

---

## Memory Management

### Physical Memory Manager (PMM)

Bitmap-based frame allocator. Each bit represents one 4 KB physical frame.

| Constant | Value |
|----------|-------|
| Frame size | 4 KB |
| Max frames | 262144 (1 GB) |
| Bitmap size | 32 KB |
| Allocation | First-fit scan |

The PMM parses the Multiboot memory map at boot to determine which frames
are free. Kernel image frames and the heap region are marked as reserved.

**API:**
- `pmm_alloc_frame()` → physical address (or 0 on failure)
- `pmm_free_frame(addr)` — release a frame
- `pmm_reserve_frames(start, size)` — mark a range as used
- `pmm_get_total_frames()` / `pmm_get_used_frames()` — statistics

### Virtual Memory Manager (VMM)

Standard x86-64 **4-level page tables** (PML4 → PDPT → PD → PT).
The boot code pre-creates identity-mapped 2 MB huge pages for the first
1 GB. The VMM can add 4 KB mappings on demand.

**Page table index extraction:**

| Level | Bits | Entries |
|-------|------|---------|
| PML4 | 39–47 | 512 |
| PDPT | 30–38 | 512 |
| PD | 21–29 | 512 |
| PT | 12–20 | 512 |

**API:**
- `vmm_map_page(virt, phys, flags)` — create mapping; allocates intermediate tables
- `vmm_unmap_page(virt)` — remove mapping, invalidate TLB
- `vmm_get_physaddr(virt)` — translate virtual to physical (handles 2 MB entries)

### Kernel Heap

First-fit allocator with block splitting and free-block coalescing.
Lives in the identity-mapped region immediately after the kernel image.

| Parameter | Value |
|-----------|-------|
| Start | `_kernel_end` aligned to 4 KB |
| Max size | 12 MB |
| Initial size | 16 KB (4 pages) |
| Alignment | 16 bytes |
| Block overhead | 24 bytes (`size` + `free` + `next`) |

**API:**
- `kmalloc(size)` — allocate; splits oversized blocks
- `kfree(ptr)` — deallocate; coalesces with next free block

---

## Process Management

### Process Model

Up to **64 processes** in a flat table. Each process has a 32 KB kernel
stack allocated from the heap.

```c
struct process {
    uint32_t           pid;
    enum process_state state;     // UNUSED, READY, RUNNING, BLOCKED, ZOMBIE
    uint64_t           kernel_stack;
    uint64_t           stack_top;
    struct cpu_context *context;  // Saved registers on stack
    struct process     *next;     // Ready-queue link
    const char         *name;
    uint32_t           pending_signals;
    signal_handler_t   sig_handlers[32];
    int                is_user;        // 1 = ring 3 process
    uint64_t           user_entry;     // Ring 3 entry point
    uint64_t           user_rsp;       // Ring 3 stack pointer
    uint64_t          *pml4;           // Per-process page table
    uint32_t           parent_pid;     // Parent PID
    int                exit_code;      // Exit status
    uint64_t           sleep_until;    // Timer wakeup tick
    int                is_background;  // Launched with &
};
```

**States:**
- `READY` — in the scheduler's ready queue
- `RUNNING` — currently executing (one at a time)
- `BLOCKED` — waiting (e.g., SIGSTOP, pipe)
- `ZOMBIE` — terminated, awaiting cleanup

### Scheduler

**Algorithm:** Round-robin with preemption.

The PIT timer fires at 100 Hz. Every **5 ticks (50 ms)**, the timer handler
calls `schedule()`, which picks the next READY process from the head of
the queue, puts the current process at the tail, and performs a context
switch.

Processes can also yield voluntarily via `scheduler_yield()`.

### Context Switch

Implemented in [src/process/switch.asm](src/process/switch.asm):

```
context_switch(old_ctx_ptr, new_ctx):
    push rbp, rbx, r12-r15        save callee-saved registers
    *old_ctx_ptr = rsp             save current stack pointer
    rsp = new_ctx                  load new stack pointer
    pop r15-r12, rbx, rbp          restore registers
    ret                            jump to saved return address
```

**Interrupt safety:** The caller (`schedule()`) wraps the call in
`cli` / `sti` to prevent a timer interrupt from re-entering
`schedule()` mid-switch, which would corrupt the partially-restored
register frame.

**New process startup:** When a newly created process is scheduled for
the first time, `context_switch` returns to `process_entry_trampoline`
(in switch.asm), which executes `sti` to re-enable interrupts and then
jumps to the real entry function via `jmp r15`.

### Signals

POSIX-like signals with a pending bitmask per process:

| Signal | # | Default Action |
|--------|---|----------------|
| SIGKILL | 9 | Terminate (cannot be caught) |
| SIGTERM | 15 | Terminate |
| SIGSTOP | 19 | Block process |
| SIGCONT | 18 | Resume blocked process |
| SIGUSR1 | 10 | Terminate |
| SIGUSR2 | 12 | Terminate |

Custom handlers can be registered with `signal_register(sig, handler)`.
Signal delivery happens in `signal_check()`, called by the scheduler
right before each context switch.

---

## Drivers

### VGA Text Console

80×25 text mode at `0xB8000`. Each cell is 2 bytes:
`character | (bg << 12) | (fg << 8)`.

Supports 16-color palette, hardware cursor, scrolling, tabs, and
backspace. `kprintf()` output goes to both VGA and serial simultaneously
(unless redirected by the telnet output hook).

### 8259 PIC

Dual cascaded PICs remapped so IRQ 0–7 → INT 32–39 and
IRQ 8–15 → INT 40–47 (avoiding conflict with CPU exceptions 0–31).

### PIT Timer

Channel 0 of the Intel 8254 at **100 Hz** (divisor 11932). IRQ0 (INT 32)
increments a 64-bit tick counter and triggers preemptive scheduling
every 5 ticks.

### PS/2 Keyboard

Scancode set 1 on port `0x60`. IRQ1 (INT 33). A 256-byte ring buffer
stores keypresses. Tracks Shift, Ctrl, and CapsLock modifiers.
Arrow keys mapped to codes `0x80–0x83`.

### PS/2 Mouse

3-byte PS/2 mouse packets via IRQ12 (INT 44). Tracks X/Y position
(clamped to 80×25 screen) and 3-button state (left, middle, right).

### Serial Port

COM1 (`0x3F8`) at **38400 baud**, 8N1 format with FIFO enabled.
Used for kernel debug output and test harness communication.

### ATA/IDE Disk

Primary IDE bus at ports `0x1F0–0x1F7`. **PIO mode** with LBA28 addressing.
512-byte sectors. Supports IDENTIFY, READ, WRITE, and FLUSH commands.

The disk image (`build/disk.img`) is a 16 MB raw file created by `dd`.

### PCI Bus

Type 1 PCI configuration space access (ports `0xCF8`/`0xCFC`).
Enumerates bus 0–255, slot 0–31 to find devices. Used primarily to
locate the e1000 NIC.

### Intel e1000 NIC

Intel 82540EM (Vendor `0x8086`, Device `0x100E`) — the default NIC
emulated by QEMU. Memory-mapped I/O via BAR0.

| Resource | Count | Size |
|----------|-------|------|
| RX descriptors | 32 | 2048 B buffer each |
| TX descriptors | 32 | — |

**Poll-based** operation: no interrupts. The `netd` task calls `net_poll()`
in a loop, which calls `e1000_receive()` to check the RX ring.

### RTC

CMOS real-time clock accessed through ports `0x70`/`0x71`. Reads are
done in a consistency loop (re-read if update-in-progress flag is set).
Time stored in BCD or binary format depending on register B.

### PC Speaker

PIT channel 2 generates square waves at a given frequency.
Gate bit 0–1 of port `0x61` controls the speaker output.

### ACPI

Searches for the RSDP signature in the EBDA (`0x80000–0x9FFFF`) and
BIOS area (`0xE0000–0xFFFFF`). Traverses RSDP → RSDT → FADT to find
the PM1a control port. `acpi_shutdown()` writes `SLP_TYPa | SLP_EN`
to initiate a clean power-off.

---

## GDT, IDT, and Syscalls

### Global Descriptor Table

7 entries (null + kernel code/data + user code/data + TSS low/high):

| Selector | Segment | DPL | Notes |
|----------|---------|-----|-------|
| 0x00 | Null | — | Required |
| 0x08 | Kernel Code | 0 | 64-bit, executable |
| 0x10 | Kernel Data | 0 | Writable |
| 0x18 | User Code | 3 | 64-bit, executable |
| 0x20 | User Data | 3 | Writable |
| 0x28 | TSS Low | 0 | 64-bit TSS descriptor |
| 0x30 | TSS High | 0 | Upper 32 bits of TSS base |

The **TSS** provides `RSP0` for ring 3 → ring 0 transitions.

### Interrupt Descriptor Table

256 entries. Exceptions (0–31) use interrupt gates (ring 0, present,
type `0x8E`). Hardware IRQs (32–47) are mapped from the PIC.
Unhandled exceptions print register state to the console and halt.

Custom handlers can be registered via `idt_register_handler(vector, fn)`.

### Syscall Interface

Uses the `SYSCALL` / `SYSRET` mechanism (AMD64 fast syscall):

| MSR | Purpose |
|-----|---------|
| `IA32_EFER` | Enable SCE (syscall enable) bit |
| `IA32_LSTAR` | Entry point address |
| `IA32_SFMASK` | Clears IF during syscall |
| `IA32_STAR` | Kernel/user CS selectors |

**Calling convention:** `rax` = syscall number, `rdi`/`rsi`/`rdx`/`r10`/`r8`/`r9` = arguments.

| # | Name | Description |
|---|------|-------------|
| 0 | `SYS_READ` | Read from fd |
| 1 | `SYS_WRITE` | Write to fd (→ VGA) |
| 2 | `SYS_OPEN` | Open file |
| 3 | `SYS_CLOSE` | Close fd |
| 4 | `SYS_EXIT` | Terminate process |
| 5 | `SYS_GETPID` | Get process ID |
| 6 | `SYS_KILL` | Send signal |
| 7 | `SYS_BRK` | Adjust heap break |
| 8 | `SYS_STAT` | File metadata |
| 9 | `SYS_MKDIR` | Create directory |
| 10 | `SYS_UNLINK` | Remove file |
| 11 | `SYS_TIME` | Get time |
| 12 | `SYS_YIELD` | Yield CPU |
| 13 | `SYS_UPTIME` | Get uptime ticks |

---

## Filesystem — SMFS

**SMFS** (Simple Micro File System) is a custom on-disk format stored on
the ATA disk image.

### Disk Layout

| Sector(s) | Content |
|-----------|---------|
| 0 | Superblock (512 B) |
| 1–33 | Inode table (128 inodes × 64 B = 8192 B, 16 sectors) |
| 34+ | Data blocks (allocated on demand) |

### Superblock

```c
struct fs_super {           // 512 bytes total
    uint32_t magic;         // 0x53464D53 ("SMFS")
    uint32_t num_inodes;    // 128
    uint32_t num_data_blocks;
    uint32_t next_free_block;  // next sector to allocate
    uint8_t  padding[496];
};
```

### Inode

```c
struct fs_inode {           // 64 bytes, packed
    uint8_t  type;          // 0=free, 1=file, 2=directory
    uint8_t  reserved;
    uint16_t parent;        // parent inode index
    uint32_t size;          // file size in bytes
    uint32_t blocks[8];     // up to 8 sector numbers
    char     name[28];      // null-terminated filename
};
```

### Limits

| Limit | Value |
|-------|-------|
| Max files/dirs | 128 |
| Max file size | 4096 B (8 blocks × 512 B) |
| Block size | 512 B (= ATA sector) |
| Max filename | 27 chars |
| Max nesting | Arbitrary (parent pointer chain) |

### API

- `fs_format()` — wipe and create root directory (inode 0)
- `fs_create(path, type)` — create file or directory
- `fs_write_file(path, data, size)` — write (allocates blocks)
- `fs_read_file(path, buf, max, &out_size)` — read
- `fs_delete(path)` — remove file or directory
- `fs_stat(path, &size, &type)` — metadata
- `fs_list(path)` — list directory contents

### VFS Layer

A thin abstraction with a mount table (max 4 mounts). Each mount maps a
path prefix to a `struct vfs_ops` with `read`, `write`, `stat`, `create`,
`unlink`, and `readdir` function pointers. SMFS is mounted at `/` on
boot.

---

## Networking

### TCP/IP Stack

A minimal but functional network stack split across three files:
- [src/net/net.c](src/net/net.c) — Core networking, Ethernet, ARP, IPv4, ICMP, poll loop
- [src/net/net_tcp.c](src/net/net_tcp.c) — TCP state machine, connect, listen, send/receive
- [src/net/net_udp.c](src/net/net_udp.c) — UDP, DHCP client, DNS resolver, HTTP client

All packet processing is polled (no NIC interrupts).

### Protocol Support

| Layer | Protocol | Notes |
|-------|----------|-------|
| L2 | Ethernet | 6-byte MAC, EtherType dispatch |
| L2 | ARP | 16-entry cache, request/reply |
| L3 | IPv4 | Checksums, TTL; no fragmentation |
| L3 | ICMP | Echo request/reply (ping) |
| L4 | TCP | 3-way handshake, data, FIN; 8 connections max |
| L4 | UDP | Send/receive with port binding |
| App | DHCP | Client discover/offer/request/ack |
| App | DNS | UDP stub resolver |

### TCP Implementation

| Parameter | Value |
|-----------|-------|
| Max connections | 8 |
| RX buffer per conn | 4096 B |
| Max segment (send) | 1400 B |
| Window size | 8192 B |
| Retransmit handling | Duplicate detection + partial trim |

**States:** CLOSED → SYN_SENT/SYN_RECEIVED → ESTABLISHED → FIN_WAIT/CLOSE_WAIT → CLOSED

The TCP implementation handles SLIRP (QEMU user-mode networking)
retransmissions by:
1. Dropping entirely duplicate segments (`seq + len ≤ expected`)
2. Dropping future/gap segments (`seq > expected`)
3. Trimming the already-received prefix from partial retransmits

### Network Configuration

When using QEMU user-mode networking (`-netdev user`):

| Parameter | Value |
|-----------|-------|
| Default IP | 10.0.2.15 |
| Gateway | 10.0.2.2 |
| Subnet mask | 255.255.255.0 |
| DNS server | From DHCP |

With `vmnet-shared`: IP assigned via real DHCP.

### Telnet Server

The telnet daemon listens on **port 23** and supports up to 8 concurrent
sessions. Each session has:

- 256-byte command input buffer
- 4096-byte output buffer
- IAC negotiation (ECHO, SUPPRESS_GA, LINEMODE)

**Output redirection:** During command execution, `kprintf()` is
temporarily redirected from VGA+serial to the telnet session's output
buffer via a global hook (`kprintf_set_hook`). A `processing` flag
prevents re-entrant command execution from TCP retransmits.

### The `netd` Task

A dedicated kernel process runs the network polling loop:
```c
for (;;) { net_poll(); scheduler_yield(); }
```
`net_poll()` calls `e1000_receive()` to dequeue packets from the NIC's
RX descriptor ring, then dispatches by EtherType (ARP or IP → ICMP/TCP/UDP).

---

## Shell

### Built-in Commands

| Command | Description |
|---------|-------------|
| `help` | List all commands |
| `echo [text]` | Print text |
| `clear` | Clear screen |
| `meminfo` | Physical memory statistics |
| `ps` | Process table |
| `uptime` | Time since boot |
| `date` | Current RTC date/time |
| `cpuinfo` | CPU vendor and brand |
| `history` | Recent commands |
| `kill <pid> [sig]` | Send signal to process |
| `color <fg> [bg]` | Set console color (0–15) |
| `hexdump <addr> [len]` | Dump memory in hex |
| `mouse` | Show mouse position & buttons |
| `beep <freq> <dur>` | Play a tone |
| `play <notes...>` | Play musical notes (e.g., C4 E4 G4) |
| `ls [path]` | List directory |
| `cat <file>` | Print file contents |
| `write <file> <text>` | Write text to file |
| `touch <file>` | Create empty file |
| `mkdir <dir>` | Create directory |
| `rm <path>` | Remove file or directory |
| `stat <path>` | File/directory metadata |
| `format` | Format filesystem |
| `edit <file>` | Open text editor |
| `exec <elf>` | Run ELF binary |
| `run <script>` | Execute shell script |
| `ifconfig` | Show network config |
| `ping <ip>` | ICMP echo |
| `dns <hostname>` | DNS lookup |
| `curl <url>` | HTTP GET |
| `udpsend <ip> <port> <data>` | Send UDP datagram |
| `reboot` | Reboot system |
| `wc <file>` | Count lines, words, bytes |
| `head <file> [n]` | Show first n lines (default 10) |
| `tail <file> [n]` | Show last n lines (default 10) |
| `cp <src> <dst>` | Copy a file |
| `mv <src> <dst>` | Move/rename a file |
| `grep <pattern> <file>` | Search for pattern in file |
| `df` | Show disk usage |
| `free` | Show memory usage |
| `whoami` | Print current user |
| `hostname` | Print hostname |
| `env` | Print environment variables |
| `xxd <file>` | Hex dump of a file |
| `sleep <seconds>` | Pause for n seconds |
| `seq <start> <end>` | Print number sequence |
| `arp` | Show ARP table |
| `route` | Show routing table |
| `uname` | Print system information |
| `lspci` | List PCI devices |
| `dmesg` | Print kernel log |
| `shutdown` | ACPI power off |
| `cc <file> [out]` | Compile C source to ELF64 binary |
| `sort <file>` | Sort file lines alphabetically |
| `find <pattern>` | Search for files matching wildcard pattern |
| `calc <expr>` | Arithmetic calculator (+, -, *, /, %, parens) |
| `uniq <file>` | Remove adjacent duplicate lines |
| `tr <from> <to> <file>` | Translate characters (supports ranges) |
| `tmux` | Terminal multiplexer (split panes, Ctrl-B prefix) |
| `jobs` | List background processes |
| `fg <pid>` | Bring background process to foreground |
| `wait <pid>` | Wait for process to finish |
| `exit` | Disconnect telnet session |

### Text Editor

A minimal full-screen editor (128 lines × 80 chars) with:
- Arrow key navigation and scrolling
- Ctrl-S to save, Ctrl-Q to quit, Ctrl-G to go to line
- Reads/writes via VFS

### Script Execution

Scripts are text files with one command per line. The runner supports up
to 16 simple `$variables` and a max script size of 4096 bytes.

---

## IPC — Pipes

16 kernel pipes, each with a 4096-byte circular buffer.

```c
int id = pipe_create();
pipe_write(id, data, len);    // blocks if full
pipe_read(id, buf, len);      // blocks if empty
pipe_close_write(id);         // EOF for readers when empty
pipe_close_read(id);          // broken pipe for writers
```

Blocking is cooperative (spin on `scheduler_yield()`).

---

## ELF Loader

Loads **static 64-bit ELF** binaries (ET_EXEC or ET_DYN, x86-64).

1. Read ELF file from VFS into a 64 KB buffer
2. Validate header (magic, class, machine)
3. Copy PT_LOAD segments to their `p_vaddr` addresses
4. Zero BSS regions (`p_memsz > p_filesz`)
5. Create a kernel process with the ELF entry point

No dynamic linking, no relocations, no ASLR.

---

## Ring 3 — User Mode

Processes can run in **ring 3** (user mode) with full hardware isolation:

- **Per-process page tables** — each user process gets its own PML4; kernel
  half (entries 256–511) is shared, user half is private
- **SYSCALL/SYSRET** — fast system call entry via MSR_LSTAR; kernel RSP
  loaded from `syscall_kernel_rsp` on entry
- **TSS RSP0** — set on every context switch so interrupts in user mode
  switch to the correct kernel stack
- **iretq transition** — new user processes start via `user_entry_trampoline`
  which builds an iretq frame (SS=0x23, CS=0x1B, RFLAGS=0x202)

User-mode ELF binaries are loaded at `0x400000` with a 64 KB user stack
below `0x7FFFFFFFE000`.

---

## C Compiler

A built-in single-pass recursive descent C compiler (`cc` command):

- **Lexer** — tokenizes C source with support for `#define` preprocessor macros
- **Parser** — recursive descent for expressions, statements, functions
- **Code generator** — emits raw x86-64 machine code directly (no assembler)
- **Output** — ELF64 executable loaded at `0x400000`

**Supported features:**
- Functions, local/global variables, arrays, structs, unions
- Pointers, pointer arithmetic, `->` and `.` operators
- Control flow: `if`/`else`, `while`, `for`, `do-while`, `switch`, `goto`
- Operators: arithmetic, comparison, logical, bitwise, compound assignment
- `sizeof`, type casts, function pointers, string literals
- Syscall interface for I/O (`sys_write`, `sys_exit`)

**Usage:** `cc source.c [output]` — compiles and writes ELF to filesystem,
then `exec output` to run it.

---

## Terminal Multiplexer (tmux)

A built-in terminal multiplexer with split-pane support:

- **Ctrl-B** prefix key (like GNU tmux)
- **Ctrl-B %** — vertical split
- **Ctrl-B "** — horizontal split
- **Ctrl-B ←/→/↑/↓** — navigate between panes
- **Ctrl-B x** — close current pane
- **Ctrl-B q** — quit tmux and return to shell

Each pane runs an independent shell instance with its own command buffer.

---

## Job Control & Background Processes

The shell supports background execution and job management:

- **`command &`** — launch command in a background kernel thread; shell
  returns immediately with `[PID] command`
- **`jobs`** — list all background processes with PID, state, and name
- **`fg <pid>`** — bring a background process to the foreground (wait for it)
- **`wait <pid>`** — block until a specific process exits

**Implementation:**
- Background commands run as separate kernel threads via `process_create()`
- The `is_background` flag marks processes launched with `&`
- Blocking sleep uses `process_sleep_ticks()` — the timer ISR wakes processes
  when their `sleep_until` tick is reached (no busy-wait)
- Zombie processes are automatically reaped every second by the timer ISR
- `process_waitpid()` blocks the caller until the target becomes ZOMBIE,
  then frees its kernel stack and process slot

---

## Testing

### In-Kernel Tests (95 tests)

Built with `make test` (adds `-DTEST_MODE`). A dedicated process runs
all test groups at boot, outputs `[PASS]`/`[FAIL]` to serial, and calls
`acpi_shutdown()`:

- **String tests** — strlen, strcmp, strcpy, memcpy, memset, strncmp, etc.
- **Memory tests** — PMM frame alloc/free, heap alloc/free/coalesce
- **Timer tests** — tick counter advancement
- **RTC tests** — time reading sanity
- **Process tests** — creation, PID assignment
- **Scheduler tests** — round-robin yield
- **Filesystem tests** — format, create, write, read, delete, stat
- **VFS tests** — mount/read/write through VFS layer
- **Pipe tests** — create, write, read, close, EOF
- **Speaker tests** — tone generation
- **Mouse tests** — position queries
- **Signal tests** — send/receive, SIGKILL termination
- **Network tests** — IP config, ARP, DHCP, TCP handshake
- **UDP tests** — port binding

### E2E Tests (110+ tests)

Built with `make e2e`. Boots the kernel in QEMU with user-mode networking
and drives every shell command over a telnet connection:

**Test runner** ([tests/e2e.sh](tests/e2e.sh)):
1. Pick a free TCP port (12323–12328)
2. Start QEMU with `-netdev user,hostfwd=tcp::PORT-:23`
3. Wait for "Telnet server on port 23" in serial log (up to 90 s)
4. Sleep 3 s for scheduler warm-up
5. Run [tests/e2e.py](tests/e2e.py) with `E2E_PORT` set

**Python test client** ([tests/e2e.py](tests/e2e.py)):
- Minimal telnet client (strips IAC sequences)
- `send_cmd(cmd)` → sends command, waits for `os> ` prompt, returns output
- Validates output with substring checks and regex patterns

**Test groups:** help, echo, meminfo, ps, uptime, date, cpuinfo, history,
color, hexdump, mouse, ifconfig, beep, play, udpsend, ping, dns, kill,
filesystem (format, ls, mkdir, touch, write, cat, stat, rm), run/script,
wc, head, tail, cp, mv, grep, df, free, whoami, hostname, env, xxd,
sleep, seq, arp, route, uname, lspci, dmesg, sort, find, calc, uniq,
tr, cc (compiler), pipes, redirection, background processes, jobs, fg,
wait, enhanced ps, error cases, exit.

### CI — GitHub Actions

Every push and pull request triggers the full test suite on Ubuntu:

1. Install cross-compiler (`x86_64-linux-gnu-gcc`), NASM, and QEMU
2. Build the kernel
3. Run in-kernel unit tests (95 assertions)
4. Run E2E tests over telnet (110+ assertions)

---

## Build System

### Toolchain

| Tool | Purpose |
|------|---------|
| `x86_64-elf-gcc` | Cross-compiler (C11, freestanding) |
| `nasm` | Assembler (ELF64 output) |
| `x86_64-elf-ld` | Linker |
| `x86_64-elf-objcopy` | Convert ELF64 → ELF32 for Multiboot |
| `qemu-system-x86_64` | Emulator |
| `python3` | E2E test runner |

Install on macOS: `brew install x86_64-elf-gcc nasm qemu`

Install on Ubuntu/CI: `apt install gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu nasm qemu-system-x86`

The Makefile supports `CC`, `LD`, and `OBJCOPY` overrides for CI:
```bash
make CC=x86_64-linux-gnu-gcc LD=x86_64-linux-gnu-ld OBJCOPY=x86_64-linux-gnu-objcopy
```

### Compiler Flags

```
-std=c11 -ffreestanding -mno-red-zone -mno-mmx -mno-sse -mno-sse2
-fno-stack-protector -nostdlib -nostdinc -fno-builtin
-Wall -Wextra -Isrc/include -mcmodel=large -g
```

Key flags:
- `-mno-red-zone` — required for kernel code (interrupts corrupt the red zone)
- `-mcmodel=large` — allows code/data at any 64-bit address
- `-ffreestanding` — no hosted C library assumptions
- `-mno-sse*` — avoids SSE instructions (kernel doesn't save SSE state)

### Linker Script

The [linker.ld](linker.ld) places the kernel at physical address
**0x100000** with 4 KB-aligned sections:

```
0x100000  .multiboot2    Multiboot header
          .boot          Bootstrap page tables, GDT, stack
          .text          Kernel code
          .rodata        Read-only data
          .data          Initialized data
          .bss           Zero-initialized data
          _kernel_end    Heap starts here (page-aligned)
```

### Make Targets

| Target | Description |
|--------|-------------|
| `make` | Build kernel (`build/kernel.bin`) |
| `make run` | Run in QEMU with VGA + vmnet networking |
| `make test` | Build and run in-kernel tests |
| `make e2e` | Build and run E2E telnet tests |
| `make debug` | Run QEMU with GDB stub (`-s -S`) |
| `make clean` | Remove build artifacts |
| `make deps` | Install toolchain via Homebrew |

---

## Key Design Decisions

1. **Identity-mapped memory** — the first 1 GB is mapped 1:1 (virtual =
   physical) using 2 MB huge pages. This simplifies driver access (VGA,
   MMIO) and heap management at the cost of not having separate address
   spaces.

2. **Cooperative + preemptive scheduling** — processes can yield
   voluntarily, but the timer forces a switch every 50 ms to prevent
   starvation.

3. **CLI-protected context switch** — interrupts are disabled around the
   register save/restore in `context_switch` to prevent a timer interrupt
   from re-entering `schedule()` mid-switch and corrupting the register
   frame.

4. **Poll-based NIC** — the e1000 driver disables interrupts and instead
   the `netd` task polls `e1000_receive()` each iteration.  This trades
   latency for simplicity (no interrupt-driven RX path or bottom-half
   processing).

5. **Global kprintf hook** — telnet output redirection uses a single
   global function pointer in `kprintf`. Only the `netd` task runs shell
   commands, so there's no concurrent access.

6. **Static buffers in TX path** — `send_tcp`, `send_ip`, and `send_eth`
   use `static` local buffers to avoid overflowing kernel stacks (the
   chained TX path would put ~4.5 KB on stack otherwise).

7. **TCP retransmit handling** — since QEMU's SLIRP backend aggressively
   retransmits, the kernel's TCP stack detects and trims duplicate/partial
   segments to prevent commands from executing twice.
# os
