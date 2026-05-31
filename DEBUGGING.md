# OS Kernel Debugging Guide

A practical reference for debugging this 64-bit x86 hobby kernel under QEMU.
The project lives at `/home/ubuntu/os/` (repo root).

---

## 1. Build Targets

| Target | Use |
|--------|-----|
| `make` | Build the kernel (parallel via `-j$(nproc)`, ccache, auto-dep tracking) |
| `make debug` | Build + launch QEMU with GDB stub on port 1234 (paused at cold boot) |
| `make run` | Normal QEMU boot with serial stdio and VGA |
| `make run-virtio` | QEMU boot with virtio-net instead of e1000 |
| `make test-serial` | Headless test-kernel boot with serial on TCP `:4444` |
| `make test` | Build test kernel + run automated in-kernel test suite |
| `make e2e` | Build normal kernel + run E2E tests via telnet |
| `make lint` | Run cppcheck static analysis |
| `make doom-test` | Verify framebuffer renders non-black pixels |
| `make clean` | Remove build artifacts |

> The CI pipeline has 9 additional targets (build-strict, static-analysis, virtio-net-smoke, usb-fat-smoke, libc-test, release) — see `.github/workflows/ci.yml`.

### The `make debug` target (from Makefile)

```makefile
debug: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
    sudo qemu-system-x86_64 -kernel $(BUILDDIR)/kernel.bin -m 256M -serial stdio -vga std -s -S \
        -drive file=$(BUILDDIR)/disk.img,format=raw,if=ide \
        -netdev vmnet-shared,id=net0 -device e1000,netdev=net0
```

**Flags explained:**
- `-s -S`: shorthand for `-gdb tcp::1234` (listen on 1234) and `-S` (freeze CPU until GDB continues)
- `-m 256M`: 256 MB RAM
- `-serial stdio`: serial output (COM1) on the terminal
- `-vga std`: standard VGA (cirrus would need different BAR handling)
- `-drive file=...`: IDE disk image (16 MB, formatted with SMFS or FAT32)

**Note:** on macOS the target calls `sudo qemu-system-x86_64` for vmnet bridging.
On Linux you may need `sudo` for the `vmnet-shared` netdev, or use `make run-virtio` instead
(user-mode networking, no root required).

---

## 2. GDB Setup

### Step-by-step

```bash
# Terminal 1: Launch QEMU in debug mode (pauses at pre-boot)
cd /home/ubuntu/os
make debug
```

```bash
# Terminal 2: Connect GDB
cd /home/ubuntu/os
x86_64-elf-gdb build/kernel.elf    # or just 'gdb' if your distro supports it
```

Inside GDB:

```
(gdb) target remote :1234
```

Or, if you sourced `.gdbinit` (see section 7):

```
(gdb) connect
```

### What to expect

- QEMU will be frozen at the first instruction (before the BIOS even starts).
- The kernel has **full debug symbols** (`-g` is always in CFLAGS).
- The kernel is compiled `-mcmodel=large` and linked at `0xFFFF800000000000`
  (KERNEL_VMA_OFFSET). GDB handles this automatically from the ELF file.
- Low physical memory (< 1 GB) is identity-mapped by boot code, so physical
  addresses and virtual addresses for low memory are the same (no PHYSTOVIRT
  adjustment needed for low addresses).

### Toolchain

The kernel uses a cross-compiler toolchain:
- **Compiler:** `x86_64-elf-gcc` (or `x86_64-linux-gnu-gcc` on some distros)
- **Linker:** `x86_64-elf-ld`
- **Debugger:** `x86_64-elf-gdb`
- **Assembler:** `nasm` (for `.asm` source files)

On macOS, install via `make deps` (uses Homebrew).
On Linux, install the `gcc-x86-64-elf` or `gcc-x86_64-linux-gnu` package.

---

## 3. GDB Command Examples

### Breakpoints

```gdb
# Functions in kernel code (easy — full symbols)
break kernel_main
break page_fault_handler
break timer_handler
break schedule
break scheduler_tick
break fault_init
break idt_init
break gdt_init
break process_create
break vmm_init
break vmm_map_page
break serial_putchar
break kmalloc
break cpu_security_init
break smp_init_bsp
break smp_boot_aps
break apic_init_local
break production_subsystems_init
break sys_socket
break sys_epoll_create1
break sys_mq_open
break net_poll
break syscall_handler

# File + line number
break kernel.c:72        # kernel_main
break fault.c:23         # page_fault_handler
break timer.c:13         # timer_handler
break scheduler.c:106    # schedule()
break process.c:176      # process_create

# In assembly files (NASM)
# Use the label name directly — NASM exports them as symbols
break long_mode_entry    # boot.asm
break _start             # boot.asm

# Conditional breakpoint (e.g., only catch PID 5's page fault)
break page_fault_handler if process_get_current()->pid == 5

# Hardware breakpoint (for I/O memory or MMIO regions)
hbreak *0xFFFF800000109000   # kernel text physical (rarely needed)
```

### Watchpoints (data breakpoints)

```gdb
# Stop when a variable changes
watch ticks              # timer tick counter
watch current_process    # scheduler's current process pointer
watch kernel_pml4        # VMM's kernel page table pointer

# Hardware watchpoint for specific address (no aliasing)
awatch *0xB8000          # stop on read or write to VGA text buffer

# Conditional watch
watch ticks if ticks > 1000
```

### Stepping and Navigation

```gdb
# Step one instruction (into)
stepi        # or just 's' (defined in .gdbinit)

# Step one instruction (over)
nexti        # or just 'n'

# Continue execution
continue     # or just 'c'

# Finish current function (run until return)
finish       # or just 'f'

# Run until a specific address
until *0xFFFF800000001000
```

### Register Inspection

```gdb
# All registers at once (defined in .gdbinit)
regs

# Individual registers
info registers
info registers rax rbx rcx rdx rsi rdi
print $rip
print $cr2
print $cr3
print $rsp
print $eflags

# FPU / SIMD (if applicable — kernel disables SSE/MMX)
info float
info all-registers
```

### Memory Dumps

```gdb
# Hex dump helper (defined in .gdbinit)
hexdump &idt 64           # first 64 bytes of IDT
hexdump &gdt 128          # GDT entries
hexdump &process_table 512  # process table start

# x command (raw)
x/32gx &idt               # 32 quadwords from IDT
x/10i $rip                # disassemble 10 instructions at current RIP
x/16xb $cr2               # 16 bytes at fault address (CR2)

# Examine stack
x/32gx $rsp               # stack contents (64-bit words)
backtrace                 # stack backtrace (works with DWARF)
bt full                   # backtrace with local variables
```

### Useful Inspection Commands

```gdb
# Print data structures
print process_table
print *process_table        # first process entry
print *process_get_current()
print *current_process

# List source
list kernel_main
list page_fault_handler

# Disassemble
disassemble kernel_main
disassemble schedule
disassemble $rip, $rip+64

# Symbol info
info functions             # all functions
info functions ^sched      # scheduler functions
info variables ^ticks      # timer-related globals
info address kernel_main   # show address of a symbol

# Find what address a symbol is at (useful for physical vs virtual)
print &kernel_main
print &page_fault_handler
print &idt
print &process_table
```

---

## 4. Serial Console

The kernel prints boot diagnostics and shell output over COM1 (serial port at `0x3F8`).

### Normal boot (`make run`)

```bash
make run
# serial goes to the terminal via -serial stdio
```

### Headless test boot with TCP serial

```bash
# Terminal 1: boot QEMU, serial on TCP port 4444
make test-serial

# Terminal 2: connect to serial console
nc localhost 4444
# or
telnet localhost 4444
```

The `test-serial` target runs with `-display none -vga none -serial tcp::4444,server,nowait`.
This is ideal for automated testing or when you don't need VGA.

### Serial source code reference

The serial driver (`src/drivers/serial.c`) writes to COM1 (I/O port `0x3F8`):
- `serial_init()` — configures 38400 baud, 8N1, FIFO enabled
- `serial_putchar(char c)` — blocking write with timeout
- `serial_write(const char *str)` — writes a string (converts `\n` to `\r\n`)
- `serial_readable()` / `serial_getchar()` — polling reads

The kernel calls `serial_init()` first thing in `kernel_main()`, so serial output
is available from the very first `kprintf()` call.

---

## 5. QEMU Monitor

QEMU has a built-in monitor for inspecting VM state. It is **invaluable** for
kernel debugging.

### Accessing the Monitor

| Method | How |
|--------|-----|
| GUI hotkey | <kbd>Ctrl+Alt+Shift+2</kbd> (then <kbd>Ctrl+Alt+Shift+1</kbd> back to VGA) |
| stdio mode | Add `-monitor stdio` to the QEMU command line |
| TCP mode | Add `-monitor tcp::5555,server,nowait` then `nc localhost 5555` |

For the `debug` target, you can extend it:
```bash
qemu-system-x86_64 -kernel build/kernel.bin -m 256M -s -S \
    -serial stdio -monitor stdio -vga std \
    -drive file=build/disk.img,format=raw,if=ide
```
Now the QEMU monitor and serial share the same terminal — prefix with
<kbd>Ctrl+A c</kbd> to toggle between console and monitor (in `-nographic` mode),
or use separate terminals with TCP serial/monitor.

### Essential Monitor Commands

```monitor
# CPU state
info registers          # all registers (much faster than GDB for bulk)
info cpus               # list virtual CPUs
info local-apic         # local APIC state
info ioapic             # I/O APIC state

# Memory
info mem                # current page tables summary (virtual -> physical)
info pg                 # page table walk (verbose)
info mtree              # memory hierarchy (address spaces, regions)

# Interrupts
info irq                # interrupt routing
info pic                # legacy PIC state (if using PIC mode)
info lapic              # local APIC state (if using APIC)

# Devices
info pci                # PCI device tree
info block              # block devices
info network            # network devices
info usb                # USB devices (if any)

# Debug & reset
system_reset            # reset the VM (useful for restarting debug session)
system_powerdown        # shutdown
stop                    # pause execution (if not in -S mode)
cont                    # continue execution
gdbserver               # toggle GDB stub on/off
gdbserver port:1234     # change GDB stub port

# DMA
info tlb                # TLB state
info numa               # NUMA topology
info roms               # loaded ROMs

# Tracing
trace-event NAME on     # enable trace event at runtime
trace-event NAME off    # disable trace event
info trace-events       # list available trace events
```

---

## 6. Common Kernel Debug Scenarios

### 6.1 Guard Page Fault (Kernel Stack Overflow)

**Symptom:** Unmapped page fault in kernel mode at an address just below a kernel stack region.

**What it means:** A kernel thread's stack overflowed past its guard page. This usually indicates a massive stack allocation or deep recursion in kernel code.

**Debugging:**
1. Check CR2 — it should be within the guard page range for a process kernel stack
2. Check which process was running (`process_get_current()->pid`)
3. Examine the kernel stack pointer — it'll be below the guard page boundary:
   ```gdb
   print $rsp
   print process_get_current()->kernel_stack
   ```
4. The guard page is at `kernel_stack - 0x1000` (4 KB below). If CR2 equals this, it's a confirmed stack overflow.

**Typical causes:**
- Large stack-allocated buffers in a kernel function
- Deep recursion in VFS or net code
- Interrupt nesting exhausting the remaining stack

### 6.2 SMEP / SMAP Faults

**Symptom:** Page fault with error code bit 0 (protection violation) when kernel code tries to execute a user page (SMEP) or access user data without clearing AC (SMAP).

**SMEP detection:**
- Page fault error code: present=1 (bit 0=1), user=0 (bit 2=0 because the faulting source is ring 0), write=0
- RIP is in kernel code (`0xFFFF8000...`)
- The faulting address (CR2) is a user-mode page

**SMAP detection:**
- `kprintf` may show spurious garbage or the fault address is in user range
- Fault happens on kernel data access to user memory
- Check the AC flag state: the kernel must execute `stac` before accessing user memory and `clac` after

**Fix:** Add `stac`/`clac` around any kernel code that dereferences user pointers, or use the `copy_from_user()` / `copy_to_user()` wrappers.

### 6.3 SMP / APIC Debugging

**Symptom:** APs don't come online, or cross-CPU crashes.

**SIPI bringup — what to check:**
1. Is the Local APIC initialized (`apic_init_local()`)?
2. Check the BSP's APIC ID:
   ```gdb
   print smp_get_cpu_id()
   print smp_bsp_lapic_id
   ```
3. Break on the AP trampoline entry:
   ```gdb
   break ap_trampoline_start   # in smp.asm or boot.asm
   ```
4. Verify IPI delivery for TLB shootdowns/work stealing:
   ```gdb
   break smp_send_ipi
   ```

**APIC vs PIC mode:**
- The kernel initializes the legacy PIC first, then switches to APIC mode
- If APIC init fails, inter-CPU interrupts (IPIs) don't work
- All SMP operations depend on Local APIC being functional

### 6.4 Production Subsystem Debugging

**Socket/epoll/POSIX timer/message queue faults:**

1. Check that `production_subsystems_init()` completed during boot (look for "[OK] Production subsystems initialized" in serial output)

2. **Socket layer:**
   ```gdb
   break sys_socket
   break sys_bind
   break sys_listen
   ```

3. **epoll:**
   ```gdb
   break sys_epoll_create1
   break epoll_add_fd
   ```

4. **POSIX timers:**
   ```gdb
   break sys_timer_create
   break timer_signal_handler
   ```

5. **Message queues:**
   ```gdb
   break sys_mq_open
   break mq_send_wake
   ```

### 6.5 Triple Fault / Hang at Boot

**Symptom:** QEMU window shows "Guest has not initialized the display (yet)" or
QEMU silently resets. The kernel never reaches `kernel_main` or hangs shortly after.

**What to check:**

1. **IDT** — Are exception handlers registered? If ISR 8 (Double Fault) fires and
   no IDT entry exists, the CPU tries an unhandled #DF → #DF again → **Triple Fault**.
   ```gdb
   # Check IDT entries 0-31 are populated
   dump_idt               # from .gdbinit
   # Or manually:
   print idt[0]
   print idt[8]           # Double Fault
   print idt[13]          # General Protection Fault
   print idt[14]          # Page Fault
   ```

2. **GDT** — Is the GDT loaded correctly? Wrong segment selectors cause #GP.
   ```gdb
   # Examine GDT (assuming gdt is in .data)
   print gdt[0]           # Null descriptor
   print gdt[1]           # Kernel code (0x08)
   print gdt[2]           # Kernel data (0x10)
   print gdt[3]           # User code (0x18)
   print gdt[4]           # User data (0x20)
   print gdt[5]           # TSS low
   ```

3. **Page tables** — Is the PML4 set up correctly? The kernel is linked at
   `0xFFFF800000000000`, so PML4[256] must point to a valid PDPT.
   ```gdb
   # Walk page tables
   info registers cr3     # PML4 physical address (low memory → identity mapped)
   x/4gx $cr3             # first 4 PML4 entries
   x/4gx (PML4_addr + 256*8)  # PML4[256] (high-half kernel mapping)
   ```

4. **Triple fault from QEMU perspective:**
   ```bash
   # Reboot the kernel with triple-fault logging
   qemu-system-x86_64 -kernel build/kernel.bin ... -d cpu_reset -no-reboot
   ```
   The `-no-reboot` flag stops QEMU on triple fault instead of resetting,
   and `-d cpu_reset` prints the CPU state at the fault.

5. **Boot hang:** The kernel's first actions are `serial_init()` then
   `vga_init()` then `kprintf`. If serial works but VGA doesn't, try
   `-vga none` and watch serial output.

### 6.6 Page Fault

**Symptom:** The kernel prints `*** KERNEL PAGE FAULT ***` and halts, or a user
process receives SIGSEGV.

**Kernel-mode page fault output** (from `page_fault_handler` in `fault.c`):
```
*** KERNEL PAGE FAULT ***
CR2=0x12345678  error=0x7
RIP=0xFFFF800000109ABC  RSP=0xFFFF8000001FF000  CS=0x08
```

**Error code bits:**
| Bit | Meaning |
|-----|---------|
| 0 | `1` = protection fault (present page), `0` = not-present |
| 1 | `1` = write, `0` = read |
| 2 | `1` = user mode, `0` = kernel mode |
| 3 | `1` = reserved bit violation |
| 4 | `1` = instruction fetch |

**Debugging workflow:**

1. **Get the fault address from CR2:**
   ```gdb
   info registers cr2
   print $cr2
   ```

2. **Find what was executing:**
   ```gdb
   print $rip
   x/10i $rip             # disassemble around fault
   list *$rip             # show source line (if DWARF info available)
   ```

3. **Examine the faulting address:**
   ```gdb
   # Is it a valid address? Walk the page tables.
   info registers cr3
   # PML4 physical is in CR3. Since low 1GB is identity-mapped, use CR3 as address.
   hexdump $cr3 32        # first 4 PML4 entries

   # For user-mode fault: check which process
   print process_get_current()
   print *process_get_current()
   ```

4. **Null pointer dereference:** If `CR2` is near `0x0` or `0xFFFF800000000000`,
   a pointer was not initialized or a struct member was accessed via NULL.

5. **User page fault (SIGSEGV):** Check process state:
   ```gdb
   print *process_get_current()
   # or
   print process_table[0]
   print process_table[1]
   ```

### 6.7 Process Stuck / No Scheduling

**Symptom:** The kernel boots but no process runs, or one process hogs the CPU.

**What to check:**

1. **Is the timer IRQ firing?** Without timer interrupts, the scheduler stays
   with the first/current process forever.
   ```gdb
   print ticks            # timer tick count (volatile)
   # If ticks stays 0, timer_init() wasn't called or IRQ routing is wrong.
   ```

2. **Scheduler queues:**
   ```gdb
   # Scheduler uses a priority-queue array (4 levels)
   # queue_head[0..3] and queue_tail[0..3] are static in scheduler.c
   # They're not exported. Use debug symbols:
   print scheduler_enabled
   print queue_head
   print queue_tail
   ```

3. **Process table scan:**
   ```gdb
   # process_table is static in process.c, 256 entries max
   print process_table[0]
   print process_table[1]
   # ... check state == PROCESS_READY (2) or PROCESS_RUNNING (3)
   ```

4. **Is the `schedule()` function being called?**
   ```gdb
   break scheduler_tick
   break schedule
   # If schedule() is never hit, the timer IRQ (IRQ0 → vector 32) is broken.
   ```

5. **IRQ routing:** Check that the PIC (or APIC) is configured:
   ```gdb
   # Look at PIC state via QEMU monitor (much easier)
   # In QEMU monitor: info pic
   ```

6. **Current process state:**
   ```gdb
   # The current_process variable is static. We can check via process_get_current().
   print process_get_current()
   print *(struct process*)process_get_current()
   ```

7. **If all processes are blocked:**
   ```gdb
   # Check if scheduler_wake_sleepers is waking them
   # Check sleep_until vs timer_get_ticks()
   break scheduler_wake_sleepers
   ```

### 6.8 Memory Corruption

**Symptom:** Random crashes, corrupted strings, page faults on valid pointers,
or data mysteriously changing.

**Debugging workflow:**

1. **GDB watchpoints** — detect the exact instruction that modifies memory:
   ```gdb
   # Watch a specific variable
   watch my_global_var

   # Watch a buffer
   watch *(char[64]*)buffer_ptr

   # Watch a 4-byte integer at an address
   watch *(uint32_t*)0xFFFF800000123456

   # Hardware-assisted watchpoint (stops before the write instead of after)
   hwatch *(uint64_t*)addr
   ```

2. **Watchpoint with scope:**
   ```gdb
   # Watch only in a specific function (watch local)
   break kernel_main
   run
   watch my_local
   ```

3. **Watch process table corruption:**
   ```gdb
   watch process_table
   # Or watch a specific field
   watch process_table[0].state
   ```

4. **Detect buffer overflow (watch sentinel values):**
   Place a known pattern (`0xDEADBEEF`) after your buffer, then set a watchpoint.

5. **Use QEMU monitor for physical memory corruption:**
   ```monitor
   # In QEMU monitor (not GDB):
   info registers
   xp /10gx 0x100000       # physical memory at kernel base
   ```

6. **Stack corruption:**
   ```gdb
   # Examine the stack canary or pattern
   x/16gx $rsp
   # Check RSP is within bounds
   print $rsp
   ```

7. **Heap corruption:**
   ```gdb
   # If heap has magic/guard values, check them
   # kmalloc uses a heap allocator — check its metadata
   ```

---

## 7. GDB .gdbinit

The `.gdbinit` file lives at `/home/ubuntu/os/.gdbinit` and is auto-loaded when
GDB starts from the repo root.

GDB must be configured to allow `.gdbinit` in the current directory (this is
off by default for security). Add this to `~/.gdbinit`:

```
set auto-load safe-path /
```

Or run GDB with `-x .gdbinit`:

```bash
x86_64-elf-gdb build/kernel.elf -x .gdbinit
```

### Commands defined in `.gdbinit`

| Command | Purpose |
|---------|---------|
| `connect` | Connect to QEMU GDB stub at `:1234` + reload symbols |
| `restart` | Disconnect and reconnect (after QEMU `system_reset`) |
| `regs` | Pretty-print all CPU registers (GP, CR, segment) |
| `hexdump ADDR [COUNT]` | Hex + ASCII memory dump (default 64 bytes, max 1 KB) |
| `walk_page ADDR` | Walk 4-level page tables for a virtual address (work in progress) |
| `proc PID` | Show process information by PID |
| `proc current` | Show current (running) process |
| `dump_idt` | Dump IDT entries 0-31 (CPU exception vectors) |

### KASLR / Address Offset Awareness

The kernel is currently **not** KASLR-enabled — all sections are linked at
`KERNEL_VMA_OFFSET = 0xFFFF800000000000` (see `linker.ld` and `types.h`).
GDB resolves symbols automatically from `kernel.elf` (which has full DWARF
debug info). No manual offset adjustment is needed.

If KASLR is added later, update the `$KERNEL_VMA` variable in `.gdbinit`:

```
(gdb) set $KERNEL_VMA = 0xFFFF800000000000   # actual base after KASLR
(gdb) add-symbol-file build/kernel.elf $KERNEL_VMA
```

---

## 8. Debug Symbols

### Compiler flags

The `-g` flag is **always present** in CFLAGS (from `Makefile`):

```makefile
CFLAGS = -std=c17 -ffreestanding -mno-red-zone ... -g -O2 ...
```

Note that `-O2` optimization is enabled — variables may be optimized out,
and stepping may jump around. If you need fully unoptimized debugging,
build with `CFLAGS=-O0 -g`:

```bash
make CFLAGS="-O0 -g"
```

### Build artifacts and symbol layout

```
build/
├── kernel.bin              # Flat binary loaded by QEMU -kernel
├── kernel.elf              # ELF64 with full DWARF debug info (use this in GDB)
├── boot/
│   ├── boot.o              # NASM assembly (boot.asm)
│   └── ...
├── kernel/
│   ├── kernel.o
│   ├── gdt.o
│   ├── idt.o
│   ├── fault.o
│   ├── syscall.o
│   └── ...
├── drivers/
│   ├── vga.o
│   ├── pic.o
│   ├── timer.o
│   ├── serial.o
│   ├── e1000.o
│   └── ...
├── memory/
│   ├── pmm.o
│   ├── vmm.o
│   └── heap.o
├── process/
│   ├── process.o
│   ├── scheduler.o
│   └── switch.o
├── shell/
│   └── shell.o
├── net/
│   ├── net.o
│   ├── net_tcp.o
│   └── ...
├── fs/
│   ├── fs.o
│   ├── fat32.o
│   └── ...
├── ipc/
│   ├── shm.o
│   └── pipe.o
├── lib/
│   ├── string.o
│   ├── printf.o
│   └── stdlib.o
└── disk.img                # 16 MB IDE disk image
```

### Key symbols to know

| Symbol | File | Description |
|--------|------|-------------|
| `_start` | `boot.asm` | Entry point (32-bit) |
| `long_mode_entry` | `boot.asm` | 64-bit entry after paging enabled |
| `kernel_main` | `kernel.c` | Main C entry point |
| `page_fault_handler` | `fault.c` | ISR 14 handler |
| `timer_handler` | `timer.c` | PIT IRQ0 handler (100 Hz) |
| `schedule` | `scheduler.c` | Context switch entry |
| `scheduler_tick` | `scheduler.c` | Called each timer tick |
| `process_create` | `process.c` | Create a kernel-thread process |
| `process_create_user` | `process.c` | Create a user-space process |
| `process_get_current` | `process.c` | Return current process pointer |
| `process_get_by_pid` | `process.c` | Look up process by PID |
| `process_table` | `process.c` | Static array of all process slots |
| `idt` | `idt.c` | IDT entries (256 entries) |
| `gdt` (via `gdt.c`) | `gdt.c` | GDT entries (7 entries) |
| `kernel_pml4` | `vmm.c` | Kernel page table PML4 pointer |
| `ticks` | `timer.c` | Global tick counter (100 Hz) |
| `current_process` | `process.c` | Current process pointer |

### Finding symbols in the kernel

```bash
# List all global symbols
nm build/kernel.elf | sort

# Count symbols by type
nm build/kernel.elf | grep ' T ' | wc -l    # text symbols
nm build/kernel.elf | grep ' D ' | wc -l    # data symbols

# Check DWARF debug info
objdump -h build/kernel.elf | grep debug
readelf -S build/kernel.elf | grep debug
```

---

## 9. QEMU Options for Debugging

### Logging and Tracing

```bash
# Interrupt tracing (see every interrupt/exception)
-d int

# CPU reset info (dump state on triple fault / reset)
-d cpu_reset

# Page table changes
-d page

# MMU operations
-d mmu

# All CPU-related events
-d cpu

# Guest errors (unimplemented features, invalid accesses)
-d guest_errors

# Unimplemented features
-d unimplemented

# Combine multiple
-d int,cpu_reset,page,guest_errors

# Log to file (instead of stderr)
-D /tmp/qemu.log
```

### Stopping on Triple Fault

```bash
# Without -no-reboot, QEMU resets on triple fault and you lose the state.
# With -no-reboot, QEMU stops and prints a message.
qemu-system-x86_64 ... -no-reboot -d cpu_reset
```

### Full debug invocation

```bash
qemu-system-x86_64 \
    -kernel build/kernel.bin \
    -m 256M \
    -s -S \
    -serial stdio \
    -monitor stdio \
    -vga std \
    -no-reboot \
    -d int,cpu_reset,guest_errors \
    -D /tmp/qemu-debug.log \
    -drive file=build/disk.img,format=raw,if=ide \
    -netdev user,id=net0 -device e1000,netdev=net0
```

This gives you:
- GDB stub on port 1234 (`-s -S`)
- Serial and monitor in the terminal (prefix with <kbd>Ctrl+A c</kbd> to toggle)
- No reboot on triple fault (`-no-reboot`)
- Interrupt + reset logging to `/tmp/qemu-debug.log`

### Useful QEMU Command-line Options Reference

| Option | Purpose |
|--------|---------|
| `-s` | Shorthand for `-gdb tcp::1234` |
| `-S` | Freeze CPU at startup (wait for GDB) |
| `-gdb tcp::PORT` | GDB stub on custom port |
| `-S` | Start frozen |
| `-no-reboot` | Exit on triple fault instead of resetting |
| `-no-shutdown` | Don't exit on shutdown (keep window open) |
| `-d int` | Log interrupt events |
| `-d cpu_reset` | Log CPU state on reset |
| `-d guest_errors` | Log guest misbehavior |
| `-D FILE` | Redirect `-d` output to file |
| `-monitor stdio` | QEMU monitor on stdin/stdout |
| `-serial stdio` | Serial port on stdin/stdout |
| `-serial tcp::PORT,server,nowait` | Serial over TCP |
| `-display none` | No display window (headless) |
| `-vga none` | No VGA card |
| `-vga std` | Standard VGA (for GUI) |
| `-enable-kvm` | Hardware acceleration (Linux only) |
| `-accel tcg` | Software emulation (default, no KVM) |

---

## 10. DMA / Device Debugging

### QEMU Trace Events

QEMU supports trace events for all device models. Enable with `-trace`:

```bash
# List available trace events
qemu-system-x86_64 -trace help | grep e1000
qemu-system-x86_64 -trace help | grep usb
qemu-system-x86_64 -trace help | grep ata
qemu-system-x86_64 -trace help | grep ahci
qemu-system-x86_64 -trace help | grep virtio
qemu-system-x86_64 -trace help | grep pci
```

### Ethernet (e1000) Debugging

```bash
# Full e1000 tracing
qemu-system-x86_64 ... -trace e1000*

# Specific events
qemu-system-x86_64 ... -trace e1000_irq -trace e1000_rx -trace e1000_tx

# Dump packets
qemu-system-x86_64 ... -trace e1000_rx_desc -trace e1000_tx_desc

# e1000 register access
qemu-system-x86_64 ... -trace e1000_mmio_read -trace e1000_mmio_write

# i8257x (Intel PRO/1000) PIO
qemu-system-x86_64 ... -trace e1000_io_read -trace e1000_io_write
```

### USB Debugging

```bash
# USB host controller
qemu-system-x86_64 ... -trace usb* -trace usb_ehci*

# USB packet tracing
qemu-system-x86_64 ... -trace usb_packet_state_change

# USB mass storage
qemu-system-x86_64 ... -trace usb_msd*
```

### ATA / AHCI Debugging

```bash
# ATA IDE
qemu-system-x86_64 ... -trace ide*

# AHCI
qemu-system-x86_64 ... -trace ahci*

# DMA
qemu-system-x86_64 ... -trace dma*
```

### PCI Debugging

```bash
# PCI configuration space
qemu-system-x86_64 ... -trace pci_cfg_read -trace pci_cfg_write

# All PCI events
qemu-system-x86_64 ... -trace pci*
```

### Virtio Debugging

```bash
# Virtio common
qemu-system-x86_64 ... -trace virtio*

# Virtio net
qemu-system-x86_64 ... -trace virtio_net*

# Virtio block
qemu-system-x86_64 ... -trace virtio_blk*
```

### Combining Traces

```bash
# Trace multiple subsystems at once, log to file
qemu-system-x86_64 ... \
    -trace e1000* \
    -trace pci* \
    -trace ide* \
    -D /tmp/qemu-trace.log
```

### Trace Events at Runtime

If QEMU is started with `-trace events=events.txt`, you can also toggle tracing
at runtime from the QEMU monitor:

```monitor
trace-event e1000_irq on
trace-event e1000_rx on
info trace-events
```

Where `events.txt` contains (one per line):

```
e1000_irq
e1000_rx
e1000_tx
pci_cfg_read
pci_cfg_write
```

---

## Quick Reference Cheat Sheet

### Minimal Debug Session

```bash
# Terminal 1
cd /home/ubuntu/os
make debug

# Terminal 2
cd /home/ubuntu/os
x86_64-elf-gdb build/kernel.elf -x .gdbinit
(gdb) connect
(gdb) break kernel_main
(gdb) continue
# ... QEMU starts, hits breakpoint at kernel_main ...
(gdb) stepi
(gdb) regs
(gdb) hexdump $rsp 64
```

### Most Common First Steps

```gdb
# After connecting:
break kernel_main        # first C function to execute
continue                 # let it boot until kernel_main
break page_fault_handler # catch future page faults
break timer_handler      # verify timer is working
break schedule           # watch scheduling
regs                     # see full CPU state
hexdump &idt 256         # check IDT contents
print process_table[0]   # first process
```

### Debugging a Specific Crash

```gdb
# When kernel prints:
#   *** KERNEL PAGE FAULT ***
#   CR2=0x...  error=0x...

# Immediately:
info registers cr2       # the faulting virtual address
info registers cr3       # PML4 physical base
print $rip               # what was executing
x/10i $rip               # the instructions leading to the fault
hexdump $cr2 64          # memory at the fault address (might show pattern)
bt                       # backtrace (if stack is intact)
```

---

*Last updated: June 2026 — This guide is specific to the OS kernel at /home/ubuntu/os.*
