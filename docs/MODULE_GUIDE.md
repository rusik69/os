# Hermes OS Module Programming Guide

A practical reference for writing **loadable kernel modules (LKMs)** for Hermes OS (x86-64, C17). This guide covers module structure, the build system, parameters, dependencies, and complete examples.

---

## Table of Contents

1. [Module Concepts](#1-module-concepts)
2. [Module Lifecycle](#2-module-lifecycle)
3. [Module Structure](#3-module-structure)
4. [Module Metadata](#4-module-metadata)
5. [Exporting Symbols](#5-exporting-symbols)
6. [Module Parameters](#6-module-parameters)
7. [Module Dependencies](#7-module-dependencies)
8. [Build System Integration](#8-build-system-integration)
9. [Example: Minimal "Hello" Module](#9-example-minimal-hello-module)
10. [Example: PCI Driver Module](#10-example-pci-driver-module)
11. [Vermagic and Module Versioning](#11-vermagic-and-module-versioning)
12. [Signed Modules and Compression](#12-signed-modules-and-compression)
13. [Best Practices](#13-best-practices)

---

## 1. Module Concepts

Hermes OS supports two modes of code organisation:

- **Built-in** — code compiled and linked directly into `kernel.elf`. Always present, never unloaded.
- **Loadable module (.ko)** — a relocatable ELF file loaded at runtime by the kernel module loader. Can be inserted (`insmod`), removed (`rmmod`), and queried (`lsmod`).

The modular boundary is drawn to keep boot-essential and performance-critical subsystems (scheduler, VMM, syscall dispatch, PCI core, VFS) built-in, while drivers, filesystems, network protocols, and optional components are loadable.

The current implementation uses the Phase 2 module loader (`src/kernel/module_elf.c`) which parses ET_REL ELF files, resolves symbols against the kernel's exported symbol table (`.ksymtab`), applies relocations, calls `init_module()`, and supports dependency resolution.

---

## 2. Module Lifecycle

Every module follows a strict lifecycle:

```
 insmod → kernel reads .ko file from VFS
       → ELF parsing + section loading
       → symbol resolution (kernel exports + other modules)
       → relocation application
       → module_init() called
       → module marked "live"
 rmmod → module_exit() called
       → symbol unexport (if any)
       → memory freed
       → module removed from registry
```

The module registry (`src/kernel/module.c`) maintains a hash table of loaded modules, reference counts for dependencies, and the exported symbol tree.

---

## 3. Module Structure

Every loadable module must provide exactly one init function and optionally one exit function:

```c
#include <module.h>

/* Called when the module is loaded (insmod) */
static int __init my_module_init(void)
{
    /* Probe hardware, register driver, allocate resources */
    return 0;   /* 0 = success, negative = failure */
}

/* Called when the module is unloaded (rmmod) */
static void __exit my_module_exit(void)
{
    /* Unregister, free resources, undo init */
}

module_init(my_module_init);
module_exit(my_module_exit);
```

**Key macros:**

| Macro | Purpose |
|-------|---------|
| `module_init(fn)` | Registers `fn` as the module entry point. For built-in code this becomes an initcall; for modules it creates the `init_module()` symbol alias. |
| `module_exit(fn)` | Registers `fn` as the cleanup handler. Creates `cleanup_module()` alias for modules; no-op for built-in code. |
| `MODULE_LICENSE(license)` | Declares the module's license string (e.g., `"GPL"`, `"MIT"`, `"Proprietary"`). Affects symbol availability — GPL-only symbols are restricted to GPL-licensed modules. |
| `MODULE_AUTHOR(author)` | Declares the module author. |
| `MODULE_DESCRIPTION(desc)` | Declares a human-readable description. |
| `MODULE_VERSION(ver)` | Declares module version string. |
| `MODULE_ALIAS(alias)` | Declares an alias (used by autoloader). |

---

## 4. Module Metadata

Module metadata is stored in a special ELF section (`.modinfo`) and is readable via `modinfo(8)` or the kernel's `/proc/modules` interface:

```c
#include <module.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jane Developer <jane@example.com>");
MODULE_DESCRIPTION("Example PCI Ethernet driver");
MODULE_VERSION("1.0.0");
MODULE_ALIAS("pci:v00008086d0000100E*");
```

---

## 5. Exporting Symbols

Modules can export symbols for use by other modules using `EXPORT_SYMBOL()`:

```c
// In module source
int my_function(int arg)
{
    return arg * 2;
}
EXPORT_SYMBOL(my_function);
```

The kernel's `EXPORT_SYMBOL` macro places the symbol name and address into the `.ksymtab` section, which is scanned by the module loader during dependency resolution.

Two variants are supported:

| Variant | Visibility |
|---------|-----------|
| `EXPORT_SYMBOL(sym)` | Available to all modules |
| `EXPORT_SYMBOL_GPL(sym)` | Available only to GPL-licensed modules |

Other modules can use `symbol_request(name)` to look up an exported symbol by name at runtime.

---

## 6. Module Parameters

Modules can accept parameters passed at load time via the `module_param` macros:

```c
#include <module.h>
#include <module_param.h>

/* Integer parameter, writable via /sys/module/<name>/parameters/ */
static int debug_level = 0;
module_param(debug_level, int, 0644);

/* String parameter */
static char *interface = "eth0";
module_param(interface, charp, 0644);

/* Boolean parameter */
static bool use_dma = true;
module_param(use_dma, bool, 0644);
```

Parameters are set at insmod time:

```
insmod mydriver.ko debug_level=3 use_dma=0
```

The third argument to `module_param` is the sysfs permission mask — `0` makes the parameter read-only after init.

---

## 7. Module Dependencies

Modules can depend on symbols exported by other modules. When loading a module, the kernel:

1. Resolves all undefined symbols against the kernel's export table.
2. If a symbol is provided by another loaded module, increments that module's reference count.
3. Fails the load if any symbol cannot be resolved.

Dependencies are tracked in a reference-counted DAG. A module cannot be unloaded while another module holds a reference to it.

Use `MODULE_SOFTDEP()` to declare ordering hints for the autoloader:

```c
MODULE_SOFTDEP("pre: pci_core post: thermal");
```

---

## 8. Build System Integration

The Makefile provides a `obj-m` variable that lists module `.ko` files to build. Modules are compiled with `-DMODULE` and partially linked with `ld -r`.

### Adding a new module

1. Place the source file(s) under `src/` (e.g., `src/drivers/mydev.c`).
2. Add an entry to `obj-m` in the Makefile:
   ```makefile
   obj-m += drivers/mydev.ko
   ```
3. For multi-file modules, define the `-objs` variable:
   ```makefile
   mydev-objs := drivers/mydev_core drivers/mydev_io
   ```
4. Build:
   ```bash
   make modules               # Build all modules
   make modules_install       # Copy .ko to staging directory
   ```

### Module compilation flags

Module sources are compiled with `MODULE_CFLAGS` which is derived from the base `CFLAGS` with adjustments:

```makefile
MODULE_CFLAGS  = $(filter-out -nostdinc -fno-builtin -mno-mmx -mno-sse \
                  -fstack-protector-strong ... , $(CFLAGS)) \
                 -DMODULE -ffreestanding -nostdlib -fno-builtin \
                 -fno-stack-protector -mcmodel=small -Isrc/include
```

Key differences from built-in code:
- `-DMODULE` — enables the module API (module_init, module_exit macros).
- `-mcmodel=small` — modules are loaded into the lower 2 GB of virtual memory.
- No stack protector — module code is expected to be self-contained.

---

## 9. Example: Minimal "Hello" Module

This is the simplest possible loadable module:

```c
// src/drivers/hello.c
#include <module.h>
#include <printf.h>

static int __init hello_init(void)
{
    kprintf("[hello] Hello, Hermes OS kernel module world!\n");
    return 0;
}

static void __exit hello_exit(void)
{
    kprintf("[hello] Goodbye from the module.\n");
}

module_init(hello_init);
module_exit(hello_exit);
MODULE_LICENSE("MIT");
MODULE_AUTHOR("Hermes OS Project");
MODULE_DESCRIPTION("Minimal hello-world loadable module");
MODULE_VERSION("1.0");
```

### Building and running

Add to Makefile:
```makefile
obj-m += drivers/hello.ko
```

Build and load:
```bash
make modules
insmod build/modules/drivers/hello.ko
# Kernel log: [hello] Hello, Hermes OS kernel module world!
rmmod hello
# Kernel log: [hello] Goodbye from the module.
```

---

## 10. Example: PCI Driver Module

A more realistic example: a PCI Ethernet driver that registers itself with the PCI subsystem:

```c
// src/drivers/myeth.c
#include <module.h>
#include <printf.h>
#include <pci.h>
#include <netdevice.h>

static struct pci_device_id myeth_ids[] = {
    { .vendor = 0x8086, .device = 0x100E },
    { .vendor = 0x8086, .device = 0x100F },
    { /* sentinel */ }
};

struct myeth_priv {
    struct net_device *netdev;
    uint64_t          mmio_base;
    uint32_t          irq;
};

static int myeth_probe(struct pci_device *pdev)
{
    struct myeth_priv *priv;
    uint64_t bar0;

    priv = kmalloc(sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    bar0 = pdev->bar[0] & ~0xFULL;
    priv->mmio_base = bar0;
    priv->irq       = pdev->irq;

    pci_enable_bus_master(pdev);

    kprintf("[myeth] probed %04x:%04x at %02x:%02x.%x "
            "BAR0=0x%llx IRQ=%u\n",
            pdev->vendor, pdev->device,
            pdev->bus, pdev->slot, pdev->func,
            (unsigned long long)bar0, (unsigned)priv->irq);

    /* Register network device */
    priv->netdev = netdev_register("eth%d");
    if (!priv->netdev) {
        kfree(priv);
        return -ENOMEM;
    }
    netdev_set_priv(priv->netdev, priv);

    /* Register IRQ handler */
    if (request_irq(priv->irq, myeth_irq_handler, 0, "myeth", priv) < 0) {
        netdev_unregister(priv->netdev);
        kfree(priv);
        return -EIO;
    }

    pci_set_drvdata(pdev, priv);
    return 0;
}

static void myeth_remove(struct pci_device *pdev)
{
    struct myeth_priv *priv = pci_get_drvdata(pdev);
    if (!priv)
        return;

    free_irq(priv->irq, priv);
    netdev_unregister(priv->netdev);
    pci_disable_bus_master(pdev);
    kfree(priv);
    kprintf("[myeth] removed\n");
}

static struct pci_driver myeth_driver = {
    .name     = "myeth",
    .id_table = myeth_ids,
    .probe    = myeth_probe,
    .remove   = myeth_remove,
};

static int __init myeth_init(void)
{
    return pci_register_driver(&myeth_driver);
}

static void __exit myeth_exit(void)
{
    pci_unregister_driver(&myeth_driver);
}

module_init(myeth_init);
module_exit(myeth_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Project");
MODULE_DESCRIPTION("Example PCI Ethernet driver module");
MODULE_VERSION("1.0");
MODULE_ALIAS("pci:v00008086d0000100Esv*");
```

---

## 11. Vermagic and Module Versioning

Every .ko file carries a **vermagic** string embedded during compilation. The module loader checks that the vermagic of the module matches the running kernel before loading, preventing ABI mismatches.

The vermagic includes:
- Kernel version (KVERSION)
- Preemption model (`CONFIG_PREEMPT`, `CONFIG_PREEMPT_VOLUNTARY`)
- SMP status (`CONFIG_SMP`)
- Compiler version
- Module format version

```
6.1.0-osdev SMP PREEMPT x86_64-gcc-13.3.0
```

Modules built with mismatched vermagic are rejected with `-EINVAL`.

---

## 12. Signed Modules and Compression

Hermes OS supports **module signing** and **module compression** via:

| Feature | Config | Description |
|---------|--------|-------------|
| Module signing | `CONFIG_MODULE_SIG` | Modules must carry a valid RSA/SHA-256 signature appended to the .ko file. The kernel verifies the signature against a built-in public key before loading. |
| Module compression | `CONFIG_MODULE_COMPRESS` | Modules can be compressed with gzip or xz. The module loader decompresses them transparently during loading. |

To sign a module after building:

```bash
scripts/sign-module.sh build/modules/drivers/mydev.ko
```

---

## 13. Best Practices

1. **Always check return values** — `kmalloc`, `pci_register_driver`, `request_irq`, and most other kernel APIs return error codes. Propagate them to the caller.
2. **Clean up on failure** — If init fails partway through, undo any successful operations before returning an error.
3. **Use the right license** — `MODULE_LICENSE("GPL")` is required to access GPL-only kernel symbols.
4. **Keep init/exit fast** — Module init runs under the module lock. Do heavy lifting in a workqueue or deferred probe.
5. **Declare parameters** — Use `module_param` for tunable values. This enables runtime tuning via sysfs.
6. **Add proper metadata** — Always include `MODULE_LICENSE`, `MODULE_AUTHOR`, `MODULE_DESCRIPTION`, and `MODULE_VERSION`.
7. **Test as a module** — Build both as built-in and as a module to catch `#ifdef MODULE`-gated issues.
8. **Handle module unloading** — Make exit functions truly undo everything init did. Memory leaks in module exit prevent unloading.
9. **Use `__init` and `__exit`** — These attributes free init-only code after boot and exclude exit code from built-in builds.
10. **Check vermagic** — A module built against a different kernel version will fail to load. Always rebuild modules after a kernel update.

---

## References

- `src/kernel/module.c` — Module registry and lifecycle
- `src/kernel/module_elf.c` — ELF loading and relocation
- `src/kernel/module_deps.c` — Dependency tracking
- `src/kernel/module_signature.c` — Signature verification
- `src/kernel/module_compress.c` — Compression/decompression
- `src/kernel/module_autoload.c` — Autoload and alias resolution
- `src/include/module.h` — Public module API header
- `MODULARITY.md` — Architecture overview of the modular transition
