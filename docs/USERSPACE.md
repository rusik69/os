# Userspace Transition Plan — 50 Tasks

> **Goal:** Move every non-essential subsystem out of the kernel into userspace processes.
> **Principle:** The kernel retains only boot-essential and performance-critical code — scheduler, memory management, interrupt dispatch, syscall entry, and minimal IPC primitives. Everything else runs as a ring-3 process.
> **Target:** A microkernel-like architecture where drivers, filesystems, network protocols, services, and applications are all userspace daemons.

---

## Phase 0 — Userspace Foundation (6 tasks)

Before anything can move, the kernel must export the primitives userspace processes need.

### Task 1 — Implement syscall ABI and vDSO

Create a proper syscall interface that userspace programs can call.

- Define syscall numbers in a shared header (`src/include/syscall_nums.h`)
- Implement `syscall` / `sysret` entry/exit in `syscall_asm.asm`
- Create a vDSO page mapped into every process with fast-path implementations of `gettimeofday()`, `clock_gettime()`, `getpid()`
- Wire up `SYSCALL_DEFINE` macros for consistent handler registration
- Add `clone()`, `execve()`, `exit()`, `wait4()`, `brk()`, `mmap()`, `munmap()`, `open()`, `read()`, `write()`, `close()`, `ioctl()`, `poll()`, `sched_yield()`, `nanosleep()` as mandatory baseline syscalls

**Kernel changes:** `src/kernel/syscall.c`, `src/kernel/syscall_asm.asm`, `src/include/syscall_nums.h`  
**Deliverable:** A userspace "hello world" ELF can call `write(1, buf, len)` via `syscall` instruction and see output.

### Task 2 — Userspace ELF loader

Move `elf.c` logic into a proper userspace-capable ELF loader.

- Refactor `elf_exec()` to load arbitrary ELF64 executables from VFS
- Support PT_LOAD, PT_INTERP, PT_GNU_STACK, PT_GNU_RELRO
- Map segments at correct addresses, set page permissions (R/W/X)
- Parse `PT_INTERP` to load the dynamic linker (`/lib/ld.so`)
- Set up initial stack with argv, envp, auxv (AT_PHDR, AT_PAGESZ, AT_ENTRY, AT_SECURE, AT_RANDOM, AT_SYSINFO_EHDR for vDSO)
- Jump to userspace entry point with correct CS (ring 3 code segment)

**Kernel changes:** `src/kernel/elf.c`, `src/process/process.c`  
**Deliverable:** any ELF64 executable on the filesystem can be spawned as a ring-3 process.

### Task 3 — Userspace libc (musl-compatible subset)

Build a minimal C library for userspace programs with syscall wrappers.

- `_start` entry point → `__libc_start_main` → `main(argc, argv, environ)`
- String: `strlen`, `strcmp`, `strcpy`, `strcat`, `memcpy`, `memmove`, `memset`, `memcmp`
- Stdio: `printf`, `fprintf`, `sprintf`, `putchar`, `puts`, `getchar` (via `read`/`write` syscalls)
- Stdlib: `malloc`, `free`, `calloc`, `realloc` (via `brk` or `mmap`)
- `abort()`, `exit()`, `atexit()`
- `errno` via thread-local or `__errno_location()`
- POSIX: `open`, `close`, `read`, `write`, `lseek`, `stat`, `fstat`, `unlink`, `mkdir`, `rmdir`, `chdir`, `getcwd`
- `getpid()`, `getppid()`, `fork()` → `clone()`, `execve()`, `waitpid()`, `exit()`
- `mmap`, `munmap`, `mprotect`, `brk`
- `signal`, `sigaction`, `kill`

**Location:** `src/lib/` (shared between kernel and userspace, or a separate `user/libc/`)  
**Deliverable:** A trivial userspace C program (`int main(void) { printf("hello\\n"); return 0; }`) compiles and runs in ring 3.

### Task 4 — Minimal init (PID 1) and process lifecycle

Create a minimal `/sbin/init` that performs first-stage userspace setup.

- `init` starts as PID 1 (spawned by `kernel_main` after ELF load)
- Mounts `/proc`, `/sys`, `/dev` (kept as kernel FUSE mounts initially)
- Starts `devmand` (device manager) — loads UIO drivers
- Parses `/etc/inittab` and spawns configured services (respawn, askfirst, once, sysinit)
- Reaps orphaned children (`waitpid` in a loop)
- Handles `SIGCHLD` and `SIGTERM` gracefully
- On Ctrl+Alt+Del / poweroff signal: kills all processes, syncs filesystems, calls `reboot()` or `poweroff()` syscall

**Files:** `src/init/init.c` → becomes `user/sbin/init.c`  
**Deliverable:** After kernel boots, execution transitions entirely to userspace. The kernel does not create any kernel-mode threads except idle, ksoftirqd, and workqueue.

### Task 5 — Userspace IPC library and system call wrappers

Create a shared library (`libipc.a` / `libipc.so`) that wraps kernel IPC syscalls for userspace.

- Pipe: `int pipe(int fd[2])` → `sys_pipe()`
- FIFO: wrappers via `mknod()` + `open()`
- Shared memory: `shmget()`, `shmat()`, `shmdt()`, `shmctl()` → kernel-backed SHM regions
- POSIX semaphores: `sem_open()`, `sem_wait()`, `sem_post()` → kernel semaphore objects
- POSIX message queues: `mq_open()`, `mq_send()`, `mq_receive()`
- Eventfd: `eventfd()` syscall wrapper
- Signalfd: `signalfd()` syscall wrapper
- Timerfd: `timerfd_create()`, `timerfd_settime()`, `timerfd_gettime()`
- Unix domain sockets (if moved to userspace): `socket(AF_UNIX, ...)`

**Location:** `user/lib/libipc/`  
**Deliverable:** Userspace programs can communicate via pipes, SHM, semaphores, and message queues without kernel-mode helper threads.

### Task 6 — Dynamic linker (`/lib/ld.so`)

Implement a minimal ELF dynamic linker to support shared libraries.

- Parse ELF executable headers for `PT_DYNAMIC`
- Load referenced shared libraries (`DT_NEEDED`) recursively
- Resolve symbols (`DT_SYMTAB`, `DT_STRTAB`) using relocations `R_X86_64_64`, `R_X86_64_PC32`, `R_X86_64_GLOB_DAT`, `R_X86_64_JUMP_SLOT`, `R_X86_64_RELATIVE`
- Implement lazy binding via `DT_PLTGOT` (GOT[1], GOT[2] protocol)
- Support `LD_LIBRARY_PATH`, `DT_RPATH`, `DT_RUNPATH`
- GOT protection: `mprotect` to read-only after relocation (partial RELRO / full RELRO)
- Symbol resolution order: executable → loaded shared objects (breadth-first, interposition rules)

**Location:** `user/lib/ld/`  
**Deliverable:** Userspace programs can link against shared `.so` libraries loaded at runtime.

---

## Phase 1 — Device Drivers → Userspace via UIO (12 tasks)

All hardware drivers move to userspace using a UIO (Userspace I/O) framework. The kernel provides only: MMIO mapping, interrupt delivery (via eventfd), and DMA buffer allocation.

### Task 7 — UIO framework kernel module

Create the kernel-side UIO subsystem that userspace drivers need.

- `/dev/uio0`, `/dev/uio1`, ... character devices
- `mmap` on `/dev/uioN` maps device MMIO region into userspace
- `read` / `poll` on `/dev/uioN` blocks and returns interrupt count (via eventfd)
- `ioctl` to: enable/disable IRQ, configure DMA, allocate physically-contiguous buffers
- Each UIO device has: name, version, MMIO physical address + size, IRQ vector
- Support MSI/MSI-X interrupt forwarding to the eventfd
- Register UIO devices from kernel-side PCI enumeration probe

**Files:** `src/drivers/uio.c` (enhance existing stub)  
**Deliverable:** A userspace program `cat /dev/uio0` blocks until the hardware interrupts, and `mmap` gives direct register access.

### Task 8 — Userspace e1000 NIC driver

Move the e1000 driver from kernel to a userspace daemon.

- Write `user/sbin/e1000d`: userspace program using UIO
- `mmap` the e1000 MMIO BAR (registers + descriptor rings)
- Allocate DMA buffers via `ioctl(UIO_ALLOC_DMA)` or `dma_alloc_coherent` equivalent
- Implement descriptor ring management (tx_ring, rx_ring) entirely in userspace
- Register with the userspace network stack (Task 28) via AF_PACKET or a netlink-like scheme
- Handle link state changes, interrupt moderation, RSS
- Remove the in-kernel `e1000.c` driver and all its dependencies from the kernel build

**Before:** `src/drivers/e1000.c` → `src/net/net.c` → `socket.c` (all kernel)  
**After:** `e1000d` (userspace) ↔ `/dev/uio0` ↔ kernel PCI + DMA helpers

### Task 9 — Userspace NVMe driver

Move NVMe SSD driver to a userspace daemon.

- `user/sbin/nvmed`: UIO-based userspace NVMe driver
- Map NVMe BAR (registers + doorbells) via UIO `mmap`
- Allocate I/O SQ/CQ pairs in DMA-able physical memory
- Submit NVMe commands entirely in userspace (read/write/identify/flush)
- Expose a block device to the rest of userspace via shared memory ring or `/dev/nvme0` (kernel provides only a simple shim)
- Support multiple queues, MSI-X per queue, PRP lists

**Before:** `src/drivers/nvme.c` — requires PCI, APIC, DMA, blockdev, pagecache APIs  
**After:** `nvmed` — userspace process with UIO + block service protocol

### Task 10 — Userspace AHCI SATA driver

Move AHCI SATA driver to a userspace daemon.

- `user/sbin/ahcid`: UIO-based AHCI driver
- Map AHCI BAR (HBA registers, port registers, command list, FIS, received FIS)
- Manage port command slots, write DMA FIS, handle port interrupts
- Support NCQ (Native Command Queuing), ATAPI, hotplug
- Expose block device interface to `vfsd` or as a shared-memory ring
- Remove kernel `ahci.c`, `ata.c`

**Before:** `src/drivers/ahci.c`, `src/drivers/ata.c`  
**After:** `ahcid` + `atad` (userspace)

### Task 11 — Userspace virtio-blk driver

Move virtio block driver to a userspace daemon.

- `user/sbin/virtioblkd`: UIO-based virtio-blk driver
- Map virtio PCI capability BAR, negotiate virtio features
- Set up virtqueue in DMA memory, submit block requests
- Handle config changes (capacity resize)
- Integrate with userspace block service

**Before:** `src/drivers/virtio_blk.c`  
**After:** `virtioblkd` (userspace)

### Task 12 — Userspace virtio-net driver

Move virtio network driver to a userspace daemon.

- `user/sbin/virtionetd`: UIO-based virtio-net driver
- Virtqueue management for rx/tx/ctrl queues
- Mergeable receive buffers, indirect descriptors, checksum offload
- Control queue for MAC filtering, multiqueue, VLAN
- Deliver packets to the userspace network stack via shared memory ring

**Before:** `src/drivers/virtio_net.c`  
**After:** `virtionetd` (userspace)

### Task 13 — Userspace USB host controller driver

Move USB EHCI/xHCI drivers to userspace.

- `user/sbin/usbd`: UIO-based USB host controller driver
- Map xHCI/EHCI BAR (capability registers, operational registers, doorbells)
- Manage device context arrays, transfer rings, endpoint contexts
- Handle port status change events, device connection/disconnection
- Expose USB device access to userspace (via `/dev/bus/usb/` or FUSE)
- Keep only minimal kernel USB core (device numbering, descriptor parsing) or eliminate entirely

**Before:** `src/drivers/usb_ehci.c`, `src/drivers/xhci.c`, `src/drivers/usb_msc.c`  
**After:** `usbd` (userspace)

### Task 14 — Userspace GPU modesetting driver

Move Intel GPU driver to userspace.

- `user/sbin/gpud`: UIO-based GPU modesetting
- Map GPU BAR (register space, opregion, stolen memory) via UIO `mmap`
- Implement DPLL configuration, transcoder, pipe, plane programming
- EDID parsing from GPU's GMBUS/i2c
- Mode setting, framebuffer scanout, cursor plane
- Expose `/dev/dri/card0` or FUSE-based DRM interface
- Remove kernel `intel_gpu.c`, `edid.c`, `vga.c`, `fbcon.c`

**Before:** `src/drivers/intel_gpu.c`, `src/drivers/edid.c`, `src/drivers/vga.c`, `src/drivers/fbcon.c`  
**After:** `gpud` (userspace) handles all display initialization

### Task 15 — Userspace audio driver (AC97/HD Audio)

Move audio drivers to userspace.

- `user/sbin/audiod`: UIO-based audio driver
- Map audio controller BAR (PCM/Buffer descriptor registers)
- DMA buffer management for playback/capture streams
- Support AC97 or HDA codec communication
- Expose `/dev/dsp` or PulseAudio-compatible socket
- Remove kernel `ac97.c`, `sound_oss.c`

**Before:** `src/drivers/ac97.c`, `src/drivers/sound_oss.c`  
**After:** `audiod` (userspace)

### Task 16 — Userspace input subsystem (evdev)

Move PS/2 keyboard and mouse to userspace.

- `user/sbin/inputd`: userspace input daemon
- Port I/O from userspace via `iopl()` / `ioperm()` for PS/2 controller
- Or use UIO for legacy device access
- Translate PS/2 scancodes → keycodes (keymap in userspace)
- Expose input events via `/dev/input/event0` character device or a FUSE filesystem
- Support mouse absolute/relative mode, scroll wheel
- Remove kernel `keyboard.c`, `mouse.c`, `ps2.c`

**Before:** `src/drivers/keyboard.c`, `src/drivers/mouse.c`, `src/drivers/ps2.c`  
**After:** `inputd` (userspace)

### Task 17 — Userspace SPI/I2C/SMBus/GPIO

Move remaining bus drivers to userspace.

- `user/sbin/busd`: unified bus daemon
- `/dev/i2c-N` style access via UIO or `iopl()` + GPIO bit-banging
- `/dev/spi-N` via UIO MMIO access
- `/dev/gpiochipN` via UIO
- `/dev/smbus` via UIO (SMBus controller BAR)

**Before:** `src/drivers/spi.c`, `src/drivers/i2c.c`, `src/drivers/smbus.c`, `src/drivers/gpio.c`  
**After:** `busd` (userspace)

### Task 18 — Remove floppy/speaker/watchdog/RTC from kernel

Move remaining simple drivers to userspace.

- `user/sbin/floppyd`: floppy controller via UIO + DMA
- `user/sbin/beepd`: PC speaker via `iopl()` + PIT channel 2 programming
- `user/sbin/watchdogd`: watchdog via UIO
- `user/sbin/rtcd`: RTC/CMOS via UIO or RTC device pass-through
- Remove kernel: `floppy.c`, `speaker.c`, `watchdog.c`, `rtc.c`, `cmos.c`

**Before:** various kernel drivers  
**After:** all are userspace daemons

---

## Phase 2 — Filesystems → Userspace via FUSE (10 tasks)

Filesystems are moved to userspace daemons using a FUSE-like protocol. The kernel retains only the VFS core — the syscall dispatch, dentry cache, inode number allocation, and a minimal FUSE device (`/dev/fuse`).

### Task 19 — FUSE kernel device (`/dev/fuse`)

Create a minimal kernel FUSE transport.

- `/dev/fuse` character device — userspace FS daemon reads requests and writes replies
- Operations: LOOKUP, GETATTR, READ, WRITE, OPEN, RELEASE, READDIR, MKDIR, RMDIR, CREATE, UNLINK, RENAME, STATFS, SETATTR, SYMLINK, LINK, READLINK, ACCESS, IOCTL, POLL, FALLOCATE
- Each FUSE request has a unique ID; kernel blocks the calling process until the daemon replies
- Cache coherency protocol: attribute cache timeout, dentry cache timeout
- Kernel VFS integration: install FUSE as a filesystem type via `register_filesystem`
- Keep the VFS core minimal: dentry cache, inode operations dispatch, mount table, path resolution

**Files:** `src/fs/fuse.c` (new), enhance `src/kernel/vfs.c`  
**Deliverable:** A userspace program can mount a FUSE filesystem and serve files.

### Task 20 — ext2 → userspace (`ext2d`)

- `user/sbin/ext2d`: FUSE daemon implementing ext2
- Read block groups, inode tables, block bitmaps
- Support symlinks, fast symlinks, sparse files, HTree indexed directories
- Block cache in userspace (readahead via large FUSE read requests)
- Use a block device daemon (e.g., `nvmed` or `ahcid`) for disk I/O via shared memory
- Remove kernel `src/fs/ext2.c`

### Task 21 — FAT32 → userspace (`fat32d`)

- `user/sbin/fat32d`: FUSE daemon implementing FAT32
- Read/write FAT, directory clusters, long filename entries
- Support FAT12/16/32 auto-detection, volume labels
- Use block device daemon for raw sector access
- Remove kernel `src/fs/fat32.c`

### Task 22 — ISO9660 → userspace (`iso9660d`)

- `user/sbin/iso9660d`: FUSE daemon for CD-ROM
- Rock Ridge extensions (POSIX attributes, symlinks)
- Joliet extensions (Unicode filenames)
- Multi-session support
- Remove kernel `src/fs/iso9660.c`

### Task 23 — tarfs / cpio → userspace (`tard`, `cpiod`)

- `user/sbin/tard`: FUSE daemon mounting a TAR archive
- `user/sbin/cpiod`: FUSE daemon mounting a CPIO archive
- Read-only, embedded filesystem demos
- Remove kernel `src/fs/tarfs.c`, `src/fs/cpio.c`

### Task 24 — romfs → userspace (`romfsd`)

- `user/sbin/romfsd`: FUSE daemon for simple ROM filesystem
- Remove kernel `src/fs/romfs.c`

### Task 25 — overlay/union → userspace (`overlayd`)

- `user/sbin/overlayd`: FUSE daemon for overlay/union mounts
- Upper + lower directories, whiteout files, copy-up on write
- Remove kernel `src/kernel/overlay.c`

### Task 26 — procfs → userspace (`procd`)

- `user/sbin/procd`: FUSE daemon producing /proc content
- Read from kernel via `/sys/kernel/procfs` or a dedicated syscall interface
  - `/proc/cpuinfo` → read from kernel CPU topology
  - `/proc/meminfo` → read from kernel PMM/VMM
  - `/proc/self/...` → per-process data from kernel process table
  - `/proc/uptime` → read kernel timer
- The kernel needs to expose a "procfs data source" — a simple shared-memory or syscall interface for the daemon to query
- Remove kernel `src/fs/procfs.c`

### Task 27 — sysfs → userspace (`sysfsd`)

- `user/sbin/sysfsd`: FUSE daemon producing /sys content
- Query kernel objects via a new `/sys/kernel/export` shared-memory region or sysctl-like syscall
- Device hierarchy, driver model, module parameters
- Remove kernel `src/fs/sysfs.c`

### Task 28 — devfs → userspace (`devfsd`)

- `user/sbin/devfsd`: FUSE daemon for /dev
- Dynamic device node creation when devices appear
- Device permissions, major/minor number management
- Remove kernel `src/fs/devfs.c`

---

## Phase 3 — Network Stack → Userspace (10 tasks)

The kernel retains only bare sockets + AF_PACKET for raw device access. The entire TCP/IP stack, routing, and network services move to userspace.

### Task 29 — Userspace TCP/IP stack library (`libnet.so`)

Create a comprehensive TCP/IP stack as a shared library.

- ARP: cache management, probe, announce
- IP: forwarding, fragmentation, options, TTL
- ICMP: echo, unreachable, redirect, timestamp
- TCP: full state machine, sliding window, congestion control (CUBIC/BBR), RACK-TLP loss detection, SYN cookies, TFO, SACK
- UDP: connected sockets, checksums, multicast
- Routing table: longest-prefix match (LC-trie), ECMP, policy routing
- Socket API: `tcp_connect()`, `tcp_listen()`, `tcp_accept()`, `udp_sendto()`, `udp_recvfrom()`
- `poll()` / `select()` / `epoll`-like event loop integration
- Remove kernel: `src/net/net.c`, `src/net/net_tcp.c`, `src/net/net_udp.c`

### Task 30 — Userspace network interface daemon (`netd`)

The binding layer between UIO drivers and the userspace TCP/IP stack.

- `user/sbin/netd`: receives raw packets from UIO drivers (e1000, virtio-net) via shared-memory rings
- Demultiplexes by ethertype → ARP/IP/...
- Delivers to `libnet.so` protocol handlers
- Handles packet transmission via the NIC UIO interface
- Enforces packet filter rules (netfilter in userspace)
- Manages routing table, neighbor cache, interface state

**Deliverable:** Full network stack runs in userspace. Kernel socket layer is a thin shim over `netd`.

### Task 31 — DHCP → userspace (`dhcpcd`)

- `user/sbin/dhcpcd`: standalone DHCP client
- Uses AF_PACKET or `netd` API to send/receive DHCP frames
- Configuration file: `/etc/dhcpcd.conf`
- Interface IP address configuration via `ioctl` (netlink-style) to the kernel
- Remove kernel `src/net/dhcp.c`

### Task 32 — DNS resolver → userspace (`dnsd`)

- `user/sbin/dnsd`: local DNS resolver/cache
- Recursive or stub resolution
- LRU cache with TTL expiry
- `/etc/resolv.conf` management
- Remove kernel `src/net/dns_cache.c`

### Task 33 — HTTP server → userspace (`httpd`)

- `user/sbin/httpd`: web server using `libnet.so`
- Static file serving, MIME types
- Thread pool or event-driven (epoll) connection handling
- Remove kernel `src/net/httpd.c`

### Task 34 — SSH server → userspace (`sshd`)

- `user/sbin/sshd`: SSH server using `libnet.so` + `libcrypto`
- Key exchange, session encryption, channel multiplexing
- PTY allocation for shell sessions
- Remove kernel `src/net/sshd.c`, `src/kernel/ssh_client.c`, `src/kernel/ssh_crypto.c`

### Task 35 — Telnet server → userspace (`telnetd`)

- `user/sbin/telnetd`: telnet server as userspace daemon
- Remove kernel `src/net/telnetd.c`

### Task 36 — WireGuard → userspace (`wgd`)

- `user/sbin/wgd`: WireGuard VPN as userspace daemon
- Uses `libnet.so` for crypto + tunneling
- Lower overhead: in-kernel crypto can stay for performance, userspace handles control
- Remove kernel `src/net/wireguard.c` (or keep crypto, move control)

### Task 37 — Bridge / VLAN / VXLAN / IPIP → userspace

- `user/sbin/bridged`: Ethernet bridge (STP, FDB, forwarding)
- `user/sbin/vland`: VLAN 802.1Q processing
- `user/sbin/tund`: TUN/TAP device emulation
- `user/sbin/vxlansrv`: VXLAN tunnel endpoint
- `user/sbin/gred`: GRE tunnel
- Remove kernel: `bridge.c`, `vlan.c`, `tun.c`, `vxlan.c`, `gre.c`, `ipip.c`

### Task 38 — Netfilter/conntrack → userspace (`nfd`)

- `user/sbin/nfd`: packet filtering daemon
- Ruleset from `/etc/nftables.conf`
- Connection tracking table with timeout
- NAT (SNAT/DNAT/Masquerade)
- Remove kernel: `src/net/netfilter.c`, `src/net/conntrack.c`

---

## Phase 4 — Services → Userspace (6 tasks)

### Task 39 — Init/service manager → userspace (`init`)

Full PID 1 init with service supervision.

- Already started in Task 4 — complete the implementation
- Support SysV-style `/etc/inittab` runlevels
- Service dependency graph (before/after, requires)
- Daemon respawn with backoff (max 5 restarts, then disable)
- Logging: capture stdout/stderr of services to `/var/log/`
- Remove kernel `src/kernel/service.c`, `src/init/init.c`

### Task 40 — Device manager → userspace (`devmand`)

Userspace device hotplug and driver loading.

- `user/sbin/devmand`: monitors `/sys/devices/` for new devices
- Matches against driver database (`/etc/devmand/`)
- Spawns the appropriate UIO driver daemon (e1000d, nvmed, ahcid, ...)
- Creates device nodes via `devfsd`
- Hotplug event handling (USB insert/remove, PCI hotplug)
- Remove kernel `src/drivers/pci.c`? No — PCI enumeration stays minimal in kernel for boot, but `devmand` can rescan

### Task 41 — Power management → userspace (`powerd`)

- `user/sbin/powerd`: power management daemon
- Idle time tracking, suspend-to-RAM scheduling
- CPU frequency scaling governor selection (schedutil/ondemand from userspace)
- Wakeup event monitoring
- Battery level monitoring and ACPI event handling
- Remove kernel: `src/power/suspend.c`, `src/power/wakeup.c`, `src/power/cpufreq.c`, `src/power/pm_qos.c`, `src/kernel/cpuidle.c`

### Task 42 — ACPI → userspace (`acpid`)

- `user/sbin/acpid`: ACPI event daemon
- Parse ACPI tables (RSDP, RSDT, MADT, FADT, DSDT, SSDT) in userspace
- EC (embedded controller) communication via UIO/LPC
- Thermal zone monitoring and cooling decisions
- Power button, lid switch events
- Battery status
- Remove kernel: `src/drivers/acpi.c`, `src/drivers/acpi_ec.c`, `src/drivers/acpi_thermal.c`, `src/drivers/acpi_power_button.c`, `src/drivers/battery.c`, `src/drivers/dmi.c`
- Kernel retains only: RSDP location, MADT parsing for APIC

### Task 43 — Logging → userspace (`syslogd`)

- `user/sbin/syslogd`: kernel log + system log collector
- Read `dmesg` from kernel ring buffer via syscall (`sys_syslog`)
- Service logs capture (from `init`)
- Log rotation (`/var/log/*.gz`)
- Remote syslog (RFC 5424)
- Remove kernel: `src/kernel/logbuf.c`, `src/kernel/dmesg.c` (keep minimal ring buffer, move readers to userspace)

### Task 44 — Core dump → userspace (`coredumpd`)

- `user/sbin/coredumpd`: userspace core dump handler
- Kernel writes core dump data to a pipe or shared memory
- `coredumpd` formats and saves to `/var/crash/`
- Configurable: `ulimit -c`, compression, notification
- Remove kernel: `src/drivers/coredump.c`, `src/kernel/coredump_core.c`

---

## Phase 5 — Applications → Userspace (6 tasks)

### Task 45 — Shell → userspace (`sh`)

The kernel-mode shell already has a userspace path. Complete the transition.

- Compile shell as a standalone ELF executable (target `user/bin/sh`)
- Ensure all built-in commands (`cd`, `ls`, `cat`, `echo`, `mkdir`, `rm`, `ps`, `kill`, `mount`, `umount`, `insmod`, `lsmod`) work via syscalls
- Job control: background (`&`), foreground (`fg`), suspend (`Ctrl+Z`)
- Pipes (`|`), redirection (`>`, `>>`, `<`), heredoc (`<<`)
- Environment variables, aliases, shell functions, tab completion
- Remove kernel shell path: `src/shell/` → becomes only a userspace program

### Task 46 — GUI/display server → userspace (`guisd`)

Move the GUI system to a userspace display server.

- `user/sbin/guisd`: display server running as a userspace process
- Uses `/dev/fb0` (kernel framebuffer) or `gpud` (UIO GPU) for scanout
- Input events from `inputd` (keyboard, mouse)
- Compositing window manager (double-buffer, alpha blending)
- Client protocol (send commands / receive events via `AF_UNIX` socket)
- Remove kernel: `src/gui/gui.c`, `src/gui/gui_shell.c`, `src/gui/gui_task.c`, `src/gui/gui_widgets.c`

### Task 47 — Doom → userspace (`doom`)

- Compile `src/doom/*.c` as a userspace ELF
- Replace kernel framebuffer drawing with `guisd` or `/dev/fb0` `mmap`
- Replace kernel-space keyboard input with `inputd` events
- Timer via `nanosleep()` or `clock_gettime()`
- Remove kernel: `src/doom/`

### Task 48 — DOS emulator → userspace (`dosbox`)

- Compile `src/dos/*.c` as a userspace ELF
- Replace kernel-specific calls with userspace equivalents
- Real-mode interrupt emulation via signal handlers
- Remove kernel: `src/dos/`

### Task 49 — C compiler → userspace (`cc`)

- Compile `src/compiler/*.c` as a userspace ELF
- File I/O uses `open`/`read`/`write` syscalls (already portable)
- Remove kernel: `src/compiler/`

### Task 50 — Container runtime → userspace (`containerd`)

- Move OCI container runtime to userspace
- `user/sbin/containerd`: uses `clone()` with CLONE_NEWNS|CLONE_NEWPID|CLONE_NEWNET|CLONE_NEWIPC
- Namespace creation via syscalls (not kernel functions)
- cgroups via `/sys/fs/cgroup` FUSE or kernel syscall
- Remove kernel: `src/container/config.c`, `src/container/runtime.c`

---

## Boot Sequence After Transition

The minimal kernel boot sequence after all tasks complete:

```
kernel loaded by GRUB
  → boot.asm (paging, long mode)
  → gdt_init, idt_init, pic_init (hardware basics)
  → pmm_init, vmm_init, heap_init, slab_init (memory)
  → ist_init, fault_init, mce_init (exception handling)
  → process_init, scheduler_init, smp_init (process/scheduler)
  → timer_init, syscall_init, vfs_init (core services)
  → vsyscall_init, vdso_init (userspace entry)
  → elf_exec("/sbin/init")  ← THE ONLY KERNEL-SPAWNED PROCESS
  → sti() → idle loop
```

PID 1 (`/sbin/init`) then brings up:
1. `devmand` — discover PCI devices → spawn driver daemons
2. Driver daemons: `nvmed`, `ahcid`, `virtioblkd`, `e1000d`, `virtionetd`, `inputd`, `gpud`, `audiod`, ...
3. `procd`, `sysfsd`, `devfsd` — /proc, /sys, /dev
4. Filesystem daemons (`ext2d`, `fat32d`) for `/etc`, `/var`, `/home`
5. `netd` + `dhcpcd` — network
6. `init` spawns getty → `sh` for each console
7. `guisd`, `syslogd`, `powerd`, `acpid`, `coredumpd` — services

Kernel size reduction: ~159K LOC → ~30K LOC (scheduler, mm, IPC core, syscall entry, VFS core, FUSE, minimal PCI, minimal APIC)

---

## Tracking

Each task corresponds to a tracked item in `.hermes/plans/` or a GitHub issue.  
Tasks can be parallelized for independent subsystems.  
After all 50 tasks: **every subsystem that is not the scheduler, memory manager, interrupt dispatcher, syscall entry, or VFS core runs as a userspace process.**
