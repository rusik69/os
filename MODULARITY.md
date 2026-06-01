# Modular Kernel Transition

## Current State → Target Architecture

The kernel currently has a **simple built-in module registry** (`src/kernel/module.c`, `src/include/module.h`) with a fixed 16-slot table for registering statically-linked code as "modules." This is not yet a true loadable kernel module system.

**Current:** Everything is linked into the single `kernel.elf` binary. The `module_load()` API registers function pointers from already-linked code. There is no ELF loading, no symbol resolution, no runtime code insertion.

**Target:** A Linux-style modular kernel where drivers, filesystems, network protocols, and optional subsystems can be compiled independently as `.ko` files, loaded at runtime via `insmod`, and unloaded via `rmmod`.

---

## Architecture Overview

```
┌────────────────────────────────────────────────────┐
│                  Kernel Core                        │
│  (always built-in: boot, scheduler, VMM, PMM,      │
│   VFS core, TCP/IP base, syscalls, security)        │
├────────────────────────────────────────────────────┤
│                  Module Loader                      │
│  ELF parsing → relocation → symbol resolution       │
│  → dependencies → init()/exit() lifecycle           │
├──────────┬──────────┬──────────┬───────────────────┤
│ Drivers  │ FS       │ Network  │ Optional          │
│ e1000.ko │ ext2.ko  │ ipv6.ko  │ doom.ko           │
│ nvme.ko  │ fat32.ko │ ipip.ko  │ dos.ko            │
│ ahci.ko  │ iso.ko   │ vxlan.ko │ cc.ko             │
│ ...      │ ...      │ ...      │ ...               │
└──────────┴──────────┴──────────┴───────────────────┘
```

The modular boundary is drawn to keep performance-critical and boot-essential code built-in while making everything else runtime-loadable. The kernel base is ~20% of the codebase; ~80% becomes modules.

---

## Implementation Phases

### Phase 1 — Symbol Export Table (prerequisite)

A module loaded at runtime needs access to kernel functions (`kmalloc`, `printk`, `vmm_map_page`, etc.). These must be exported in a table the module loader can query.

**Key structures:**

```c
// Kernel symbol table entry (in ksymtab section)
struct kernel_symbol {
    uint64_t value;     // address of the symbol
    const char *name;   // symbol name string
};

// Linker section for exported symbols
// Placed in .rodata.ksymtab so the module loader can find them
#define __EXPORT_SYMBOL(sym) \
    static const struct kernel_symbol __ksym_##sym \
    __attribute__((__used__, __section__(".ksymtab"))) = { \
        .value = (uint64_t)&sym, \
        .name = #sym, \
    }

// Usage:
// int EXPORT_SYMBOL(kmalloc);
// → __EXPORT_SYMBOL(kmalloc)
```

**Files to create/modify:**
- `src/include/export.h` — EXPORT_SYMBOL macros
- `src/kernel/module.c` — export table search
- `linker.ld` — add `.ksymtab` section

### Phase 2 — Module ELF Loader

Load ET_REL (relocatable) ELF files from the filesystem into kernel memory. This is the core of the modular system.

```c
// Module loading sequence
int load_module(const char *path) {
    // 1. Read ELF file from VFS
    uint8_t *elf_data = vfs_read_file(path);
    
    // 2. Validate ELF header (magic, class 64, type ET_REL)
    if (!elf_validate_relocatable(elf_data)) return -EINVAL;
    
    // 3. Allocate module memory region
    struct module *mod = alloc_module_struct();
    
    // 4. Program headers: copy sections (.text, .data, .rodata, .bss)
    //    into module memory region with correct permissions
    load_module_sections(elf_data, mod);
    
    // 5. Resolve external symbols against kernel export table
    //    R_X86_64_64:        sym_value + addend
    //    R_X86_64_PC32:      sym_value + addend - location
    //    R_X86_64_PLT32:     sym_value + addend - location (same as PC32)
    //    R_X86_64_32:        sym_value + addend (32-bit truncated)
    resolve_module_symbols(mod);
    
    // 6. Apply relocations to sections (.text, .data)
    apply_module_relocations(mod);
    
    // 7. Mark memory executable (for .text) and writable (for .data)
    set_module_permissions(mod);
    
    // 8. Call module_init() entry point
    call_mod_init(mod);
    
    return mod->id;
}
```

**Relocation types to support:**
| Type | Description |
|------|-------------|
| `R_X86_64_64` | Absolute 64-bit: sym + A |
| `R_X86_64_PC32` | 32-bit PC-relative: sym + A - P |
| `R_X86_64_PLT32` | PLT-relative (same as PC32 for kernel) |
| `R_X86_64_32` | 32-bit zero-extended: sym + A |
| `R_X86_64_32S` | 32-bit sign-extended: sym + A |
| `R_X86_64_PC64` | 64-bit PC-relative: sym + A - P |

**Files to create/modify:**
- `src/include/module_elf.h` — ELF relocation types, section types
- `src/kernel/module_elf.c` — ELF parsing, section loading, relocation
- `src/include/module.h` — extend struct kernel_module for loaded modules
- `src/kernel/syscall.c` — add sys_init_module, sys_finit_module, sys_delete_module

### Phase 3 — Dependency Tracking & Module Info

```c
// Module metadata (embedded in .modinfo section of .ko)
struct module_info {
    const char *name;        // module name
    const char *author;      // author string
    const char *description; // description string
    const char *license;     // "GPL", "MIT", "BSD", "Proprietary"
    const char **depends;    // NULL-terminated list of dependency names
    const char *version;     // version string
    int (*init)(void);       // init function
    void (*exit)(void);      // exit function
};

// module_depends_on(a, b) returns true if a depends on b
// resolve_dependencies(mod) loads all dependencies first
// module_can_unload(mod) returns false if any other module depends on it
```

**Dependency resolution algorithm:**
1. Read `depends` from module's modinfo
2. For each dependency: check if loaded; if not, try to autoload
3. Topological sort: load dependencies before the module
4. Cycle detection: reject modules that would create a dependency cycle

**Files to create/modify:**
- `src/include/module.h` — extend struct kernel_module with deps
- `src/kernel/module_deps.c` — dependency graph management
- `src/shell/cmds/cmd_depmod.c` — implement /sbin/depmod

### Phase 4 — Module Parameters

Already partially exists in `module_param` macros. Extend for runtime-loaded modules:

```c
// Parameter passing at load time
// insmod mymodule.ko param_name=42 param_str="hello"

// In the module:
static int my_param = 0;
module_param(my_param, INT, 0644);

static char my_str[64] = "";
module_param_string(my_str, my_str, sizeof(my_str), 0644);
```

**Files to modify:**
- `src/include/module.h` — add module_param_string, module_param_named
- `src/kernel/module.c` — parameter parsing from load-time string

### Phase 5 — Module Unloading

```c
int unload_module(int module_id) {
    // 1. Check if any other module depends on this one
    if (module_has_dependents(module_id)) return -EBUSY;
    
    // 2. Call module exit function
    if (mod->exit) mod->exit();
    
    // 3. Remove from dependency graph
    module_remove_deps(mod);
    
    // 4. Free allocated module memory
    vmm_unmap_page_range(mod->base, mod->size);
    pmm_free_frames(mod->base_phys, mod->num_pages);
    kfree(mod);
}
```

**Files to modify:**
- `src/kernel/module.c` — full unload implementation
- `src/kernel/syscall.c` — sys_delete_module

### Phase 6 — Module Autoloading

```c
// request_module(name) — load module by name
// Called when:
//   - New PCI device discovered → request_module("e1000")
//   - New USB device discovered → request_module("usb-storage")
//   - mount("ext2") → request_module("ext2")

// Module aliases (in .modinfo section):
MODULE_DEVICE_TABLE(pci, e1000_pci_table);
// → alias: "pci:v00008086d0000100Esv*sd*bc*sc*i*"
// → matches PCI vendor 0x8086, device 0x100E

// Filesystem aliases:
MODULE_ALIAS("fs-ext2");
MODULE_ALIAS("fs-fat");
```

**Files to create/modify:**
- `src/kernel/module_autoload.c` — request_module, alias matching
- `src/drivers/pci.c` — PCI device discovery triggers request_module
- `src/fs/fs.c` — mount triggers filesystem module load

### Phase 7 — Module Files (`.ko` format)

Create a build system for `.ko` files:

```makefile
# In Makefile — build individual .ko files
# Each .ko is a relocatable ELF with:
#   - .modinfo section (metadata)
#   - .ksymtab section (symbol exports, if any)
#   - .text, .data, .rodata, .bss

# Build rule:
#   $(CC) $(CFLAGS) -c src/drivers/e1000.c -o build/modules/e1000.o
#   $(LD) -r build/modules/e1000.o -o build/modules/e1000.ko

modules: $(patsubst src/drivers/%.c, build/modules/%.ko, $(DRIVER_SRCS))
```

### Phase 8 (Final) — Sysfs Interface

```c
// /sys/module/<name>/
//   ├── initstate        → "live" | "loading" | "unloading" | "error"
//   ├── refcnt           → reference count
//   ├── version          → module version
//   ├── srcversion       → source checksum
//   ├── holders/         → symlinks to dependent modules
//   └── parameters/      → module parameter values (read/write per perm)
//       ├── param_name   → current value
//       └── ...
```

---

## Subsystem Modularization Plan

Which subsystems move to loadable modules, and in what order:

### Tier 1 (Easy — standalone, few kernel dependencies)
| Module | Files | Reason |
|--------|-------|--------|
| `doom.ko` | `src/doom/*` | Standalone game, no kernel deps beyond framebuffer + input |
| `dos.ko` | `src/dos/*` | DOS emulation, standalone |
| `cc.ko` | `src/compiler/*` | in-kernel C compiler, standalone |
| `speaker.ko` | `src/drivers/speaker.c` | PC speaker driver, simple |
| `gui.ko` | `src/gui/*` | GUI system, optional |

### Tier 2 (Medium — VFS or socket API calls)
| Module | Files | Reason |
|--------|-------|--------|
| `ext2.ko` | `src/fs/ext2.c` | Filesystem — register via VFS |
| `fat32.ko` | `src/fs/fat32.c` | Filesystem |
| `iso9660.ko` | `src/fs/iso9660.c` | Filesystem |
| `tarfs.ko` | `src/fs/tarfs.c` | Filesystem |
| `romfs.ko` | `src/fs/romfs.c` | Filesystem |
| `cpio.ko` | `src/fs/cpio.c` | Filesystem |
| `overlay.ko` | `src/kernel/overlay.c` | Filesystem |
| `procfs.ko` | `src/fs/procfs.c` | Filesystem |
| `sysfs.ko` | `src/fs/sysfs.c` | Filesystem |
| `devfs.ko` | `src/fs/devfs.c` | Filesystem |
| `tmpfs.ko` | `src/fs/tmpfs.c` | Filesystem (but rootfs needs built-in) |
| `debugfs.ko` | `src/fs/debugfs.c` | Filesystem |

### Tier 3 (Medium — device driver API)
| Module | Files | Reason |
|--------|-------|--------|
| `e1000.ko` | `src/drivers/e1000.c` | NIC driver |
| `nvme.ko` | `src/drivers/nvme.c` | NVMe driver |
| `ahci.ko` | `src/drivers/ahci.c` | AHCI SATA driver |
| `ata.ko` | `src/drivers/ata.c` | ATA PIO driver |
| `virtio_blk.ko` | `src/drivers/virtio_blk.c` | Virtio block |
| `virtio_net.ko` | `src/drivers/virtio_net.c` | Virtio net |
| `usb_ehci.ko` | `src/drivers/usb_ehci.c` | USB EHCI |
| `xhci.ko` | `src/drivers/xhci.c` | USB xHCI |
| `usb_msc.ko` | `src/drivers/usb_msc.c` | USB mass storage |
| `intel_gpu.ko` | `src/drivers/intel_gpu.c` | GPU driver |
| `ac97.ko` | `src/drivers/ac97.c` | Audio driver |
| `hda.ko` | `src/drivers/hda.c` (planned) | HD Audio |
| `floppy.ko` | `src/drivers/floppy.c` | Floppy driver |

### Tier 4 (Hard — network protocol or complex)
| Module | Files | Reason |
|--------|-------|--------|
| `ipip.ko` | `src/net/ipip.c` | Tunnel protocol |
| `gre.ko` | `src/net/gre.c` (planned) | Tunnel protocol |
| `vxlan.ko` | `src/net/vxlan.c` (planned) | Overlay network |
| `wireguard.ko` | `src/net/wireguard.c` | VPN |
| `ipvs.ko` | `src/net/ipvs.c` | Load balancing |
| `bridge.ko` | `src/net/bridge.c` | Bridge |
| `vlan.ko` | `src/net/vlan.c` | VLAN |
| `netfilter.ko` | `src/net/netfilter.c` | Firewall |
| `conntrack.ko` | `src/net/conntrack.c` (planned) | Connection tracking |
| `ssh.ko` | `src/net/sshd.c`, `src/kernel/ssh_*` | SSH server |
| `telnetd.ko` | `src/net/telnetd.c` | Telnet server |
| `httpd.ko` | `src/net/httpd.c` | HTTP server |

### Tier 5 (Optional — infrequently used)
| Module | Files | Reason |
|--------|-------|--------|
| `rcu.ko` | `src/kernel/rcu.c` | RCU (but better built-in) |
| `ksm.ko` | `src/memory/ksm.c` | Memory dedup |
| `thp.ko` | `src/memory/thp.c` | Huge pages |
| `zram.ko` | `src/memory/zram.c` | Compressed RAM |
| `cma.ko` | `src/memory/cma.c` | CMA allocator |
| `compaction.ko` | `src/memory/compaction.c` | Memory compaction |
| `memhotplug.ko` | `src/memory/memhotplug.c` | Memory hotplug |
| `seccomp.ko` | `src/kernel/seccomp.c` | Seccomp (but security is core) |
| `landlock.ko` | `src/kernel/landlock.c` | LSM |
| `audit.ko` | `src/kernel/audit.c` | Auditing |

---

## Symbol Export Strategy

Not all kernel functions should be exported. A minimal export set:

```c
// Core — almost every module needs these
EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kfree);
EXPORT_SYMBOL(kprintf);
EXPORT_SYMBOL(memset);
EXPORT_SYMBOL(memcpy);
EXPORT_SYMBOL(strncpy);
EXPORT_SYMBOL(strlen);
EXPORT_SYMBOL(vmm_map_page);
EXPORT_SYMBOL(vmm_unmap_page);
EXPORT_SYMBOL(vmm_get_physaddr);
EXPORT_SYMBOL(pmm_alloc_frame);
EXPORT_SYMBOL(pmm_free_frame);

// VFS — filesystem modules need these
EXPORT_SYMBOL(vfs_register);
EXPORT_SYMBOL(vfs_mount);
EXPORT_SYMBOL(vfs_open);
EXPORT_SYMBOL(vfs_read);
EXPORT_SYMBOL(vfs_write);
EXPORT_SYMBOL(register_filesystem);

// PCI — driver modules
EXPORT_SYMBOL(pci_register_driver);
EXPORT_SYMBOL(pci_read_config);
EXPORT_SYMBOL(pci_write_config);
EXPORT_SYMBOL(pci_enable_msi);
EXPORT_SYMBOL(pci_enable_msix);
EXPORT_SYMBOL(request_irq);
EXPORT_SYMBOL(free_irq);

// Network — protocol modules
EXPORT_SYMBOL(sock_register);
EXPORT_SYMBOL(dev_add_pack);
EXPORT_SYMBOL(dev_remove_pack);
EXPORT_SYMBOL(register_netdev);
EXPORT_SYMBOL(unregister_netdev);
EXPORT_SYMBOL(netif_rx);

// Block — storage modules
EXPORT_SYMBOL(register_blkdev);
EXPORT_SYMBOL(unregister_blkdev);
EXPORT_SYMBOL(alloc_disk);
EXPORT_SYMBOL(add_disk);
EXPORT_SYMBOL(bio_alloc);
EXPORT_SYMBOL(bio_submit);

// Sync/IPC
EXPORT_SYMBOL(spinlock_init);
EXPORT_SYMBOL(spinlock_acquire);
EXPORT_SYMBOL(spinlock_release);
EXPORT_SYMBOL(mutex_init);
EXPORT_SYMBOL(mutex_lock);
EXPORT_SYMBOL(mutex_unlock);
```

---

## Build System Changes

### Makefile additions:

```makefile
# Module build directory
MODULE_DIR := build/modules
MODULE_CFLAGS := $(CFLAGS) -DMODULE -include src/include/module_prefix.h
MODULE_LDFLAGS := -r                  # partial link → .ko file

# Module build targets
MODULE_SRCS := $(shell find src -name '*_mod.c' -o -name '*.ko.c')
MODULE_OBJS := $(patsubst src/%.c, $(MODULE_DIR)/%.o, $(MODULE_SRCS))
MODULE_KOS  := $(MODULE_OBJS:.o=.ko)

# Build a .ko: compile → partial link
$(MODULE_DIR)/%.ko: $(MODULE_DIR)/%.o
    $(LD) $(MODULE_LDFLAGS) $< -o $@

# Install modules to filesystem image
module_install: $(MODULE_KOS)
    mkdir -p $(BUILD_DIR)/modules
    cp $^ $(BUILD_DIR)/modules/
```

### Module source file convention:
- `src/drivers/e1000_mod.c` — module-ified driver source
- Or use `#ifdef MODULE` guards in existing files:
```c
#ifdef MODULE
#include <module.h>
module_init(e1000_init);
module_exit(e1000_exit);
#else
// Built-in init is via initcall
device_initcall(e1000_init);
#endif
```

---

## Module Loading Syscalls

```c
// insmod: load a module from file
long sys_init_module(const char *path, size_t len, const char *params);

// finit_module: load from file descriptor
long sys_finit_module(int fd, const char *params, int flags);

// rmmod: unload a module
long sys_delete_module(const char *name, int flags);

// lsmod: query loaded modules
long sys_query_module(const char *name, int which, void *buf, size_t bufsize, size_t *ret);

// request_module: kernel-initiated module load (autoloading)
int request_module(const char *fmt, ...);
```

**Shell commands:**
- `insmod <path> [params]` — load module
- `rmmod <name>` — unload module
- `modprobe <name> [params]` — load with dependency resolution
- `lsmod` — list loaded modules
- `modinfo <path>` — display module info

---

## Core vs Module Boundary

The following MUST remain built-in (too fundamental for loading):
- Boot code (`boot.asm`, `gdt.c`, `idt.c`, `kernel.c`)
- Memory management core (`pmm.c`, `vmm.c`, `heap.c`, `slab.c`)
- Scheduler and process lifecycle (`scheduler.c`, `process.c`, `signal.c`)
- Syscall dispatcher (`syscall.c`, `syscall_asm.asm`)
- Interrupt handling (`apic.c`, `fault.c`, `pic.c`)
- VFS core (`vfs.c`, `fs.c`)
- Core IPC (`pipe.c`, `shm.c`, `mutex.c`)
- TCP/IP core (`net.c`, `net_tcp.c`, `net_udp.c`, `socket.c`)
- Security primitives (`spinlock.h`, `lockdep.c`)

Everything else should eventually be loadable.

---

## Timeline

| Phase | Description | Est. Effort |
|-------|-------------|-------------|
| 1 | Symbol export table + linker section | Small (1-2 cycles) |
| 2 | ELF module loader (relocations) | Large (5-10 cycles) |
| 3 | Module dependencies + modinfo | Medium (3-5 cycles) |
| 4 | Module parameters (runtime) | Small (1-2 cycles) |
| 5 | Module unloading + cleanup | Medium (3-5 cycles) |
| 6 | Module autoloading (request_module) | Medium (3-5 cycles) |
| 7 | Build system for .ko files | Small (1-2 cycles) |
| 8 | Tier 1 modules (doom, dos, cc, speaker, gui) | Medium (5-8 cycles) |
| 9 | Tier 2 modules (filesystems) | Large (8-12 cycles) |
| 10 | Tier 3 modules (drivers) | Large (10-15 cycles) |
| 11 | Tier 4 modules (net protocols) | Large (10-15 cycles) |
| 12 | Tier 5 modules (optional features) | Medium (5-10 cycles) |
| 13 | Sysfs module interface + depmod tool | Medium (3-5 cycles) |
| 14 | Module signing + verification | Medium (3-5 cycles) |
| 15 | kmod userspace tool port | Small (2-3 cycles) |

**Total:** ~60-85 improvement cycles to complete the transition.

---

## Key Challenges

1. **Init ordering:** Built-in initcalls run at boot; module init runs at load time. Some modules must be loaded before others. Need a consistent init ordering policy.

2. **Memory allocation:** Modules need permanent kernel memory for code + data. The module loader must allocate PMM frames, map them with proper permissions (RX for .text, RW for .data), and manage the module region.

3. **Symbol resolution speed:** The kernel export table could have hundreds of symbols. Binary search (by name hash) keeps module loading fast.

4. **ABI stability:** Once modules can be built separately, the kernel symbol set becomes an ABI. Changes need version tracking or vermagic checks.

5. **Reference counting:** Preventing module unloading while in use requires per-module refcounts. Each `request_irq`, `register_filesystem`, `register_netdev` increments; `free_irq`, etc. decrements.

6. **Module recursion:** A module's init function might trigger autoloading of another module. Need reentrancy in the module loader.

7. **Stack usage:** Module loading involves ELF parsing, relocations, and memory allocation. Ensure kernel stack is sufficient for the deep call chain.
