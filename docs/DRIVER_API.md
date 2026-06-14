# Hermes OS Device Driver API

A practical guide for writing device drivers for the Hermes OS kernel (x86-64, C17).

---

## Table of Contents

1. [Basic Driver Structure](#1-basic-driver-structure)
2. [PCI Device Discovery](#2-pci-device-discovery)
3. [MMIO Access](#3-mmio-access)
4. [Interrupt Registration](#4-interrupt-registration)
5. [DMA Buffer Allocation](#5-dma-buffer-allocation)
6. [Timer API](#6-timer-api)
7. [Locking Requirements](#7-locking-requirements)
8. [Exporting Symbols](#8-exporting-symbols)
9. [Makefile Integration](#9-makefile-integration)
10. [Module Metadata](#10-module-metadata)

---

## 1. Basic Driver Structure

Every driver has an **init function** called once at boot (or module load time) and an optional **exit function** for cleanup. The kernel uses a `module_init` / `module_exit` convention that works identically for built-in code and loadable modules:

```c
#include "printf.h"
#include "string.h"
#include "pmm.h"

static int g_present = 0;

static int my_driver_init(void)
{
    kprintf("[mydrv] initialising...\n");

    /* Hardware probe happens here */
    g_present = 1;
    return 0;
}

static void my_driver_exit(void)
{
    kprintf("[mydrv] shutting down\n");
    g_present = 0;
}

module_init(my_driver_init);
module_exit(my_driver_exit);
```

**Key points:**
- `module_init(fn)` — registers `fn` as a device initcall for built-in builds, or creates an `init_module` alias for loadable `.ko` files.
- `module_exit(fn)` — a no-op for built-in code (never unloaded); creates a `cleanup_module` alias for modules.
- When building as a module (`-DMODULE`), the loader calls `init_module()` then `cleanup_module()` on unload.
- Use `#include "module.h"` guarded by `#ifdef MODULE` if you need module-specific API (e.g., `MODULE_LICENSE`).

---

## 2. PCI Device Discovery

Use the PCI subsystem to find devices by **vendor/device ID** or **class/subclass**. The `struct pci_device` contains BARs, IRQ line, and bus/slot/function coordinates.

```c
#include "pci.h"

/* Find by vendor+device ID (most common) */
static int probe_my_pci_device(void)
{
    struct pci_device dev;

    if (pci_find_device(0x8086, 0x1234, &dev) < 0) {
        kprintf("[mydrv] device not found\n");
        return -1;
    }

    /* Enable PCI bus mastering (DMA) */
    pci_enable_bus_master(&dev);

    /* Read BAR0 (MMIO base address) */
    uint64_t bar0 = dev.bar[0] & ~0xFULL;      /* mask lower 4 flag bits */
    uint8_t  irq  = dev.irq;

    kprintf("[mydrv] found at %02x:%02x.%x  BAR0=0x%llx  IRQ=%u\n",
            dev.bus, dev.slot, dev.func,
            (unsigned long long)bar0, (unsigned)irq);

    return 0;
}

/* Alternative: find by class/subclass (e.g., mass storage controller) */
static int probe_by_class(void)
{
    struct pci_device dev;
    /* class=0x01 (mass storage), subclass=0x08 (NVMe) */
    if (pci_find_class(0x01, 0x08, &dev) < 0)
        return -1;
    /* ... */
    return 0;
}
```

**PCI API reference:**

| Function | Purpose |
|----------|---------|
| `pci_find_device(vendor, device, &out)` | Find by vendor/device ID |
| `pci_find_class(class, subclass, &out)` | Find by class/subclass |
| `pci_enable_bus_master(&dev)` | Enable bus mastering (DMA) |
| `dev.bar[N]` | Base address of BAR N (flags in low 4 bits) |
| `dev.irq` | Legacy IRQ line number |
| `dev.bus, dev.slot, dev.func` | PCI bus address |

For MSI/MSI-X support:

```c
/* High-level: best-effort interrupt setup (MSI-X → MSI → INTx fallback) */
struct pci_interrupt_config cfg;
pci_setup_interrupts(&dev, &cfg, my_irq_handler);

/* Or manual MSI */
struct msi_info info;
if (pci_find_msi_cap(dev.bus, dev.slot, dev.func, &info) == 0) {
    pci_enable_msi(&dev, 0x30, apic_id, 1, PCI_MSI_DM_FIXED);
}
```

---

## 3. MMIO Access

Memory-mapped I/O (MMIO) registers are accessed through the kernel's high-half virtual memory mapping. The key function is `vmm_map_phys()`.

```c
#include "vmm.h"

/* Map a 128 KB MMIO region (e.g., from PCI BAR0) */
volatile uint8_t *mmio_base;

static int map_mmio(uint64_t phys_base)
{
    mmio_base = (volatile uint8_t *)vmm_map_phys(
        phys_base,
        0x20000,                              /* size (128 KB) */
        VMM_FLAG_PRESENT | VMM_FLAG_WRITE     |* uncacheable implied for MMIO */
    );

    if (!mmio_base) {
        kprintf("[mydrv] failed to map MMIO\n");
        return -1;
    }
    return 0;
}

/* Read/write MMIO registers via volatile pointers */
static inline uint32_t mydrv_read32(uint32_t reg)
{
    return *(volatile uint32_t *)(mmio_base + reg);
}

static inline void mydrv_write32(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(mmio_base + reg) = val;
}
```

**Pattern used by existing drivers (e1000.c, nvme.c, ahci.c, tpm_tis.c):**

```c
/* e1000.c style — map at init, keep global volatile pointer */
mmio_base = (volatile uint8_t *)vmm_map_phys(bar0, 0x20000,
                VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
if (!mmio_base) { return -1; }

/* nvme.c style — PHYS_TO_VIRT direct mapping for PCI MMIO */
g_nvme_ctrl.regs = (uint64_t)PHYS_TO_VIRT((void*)(uintptr_t)mmio_base);

/* tpm_tis.c style — registers accessed via dev->mmio_base offset */
static inline uint32_t tis_read32(struct tpm_device *dev, uint16_t off) {
    return *(volatile uint32_t *)(dev->mmio_base + off);
}
```

**Important:** MMIO regions should use uncacheable memory semantics (no caching). The `vmm_map_phys()` function sets the Page Cache Disable (PCD) bit automatically for MMIO mappings, or you can specify `VMM_FLAG_NOCACHE` explicitly.

---

## 4. Interrupt Registration

Register interrupt handlers with the IDT subsystem (`idt_register_handler`) and handle PIC/IOAPIC unmasking.

### Legacy (PIC/IOAPIC) interrupts

```c
#include "idt.h"
#include "pic.h"
#include "apic.h"

/* Interrupt handler — receives a register frame */
static void mydrv_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;

    /* Read-and-clear interrupt status register */
    uint32_t status = mydrv_read32(REG_ISR);

    /* Handle interrupt... */

    irq_ack(g_irq_line);               /* EOI to PIC/IOAPIC */
    net_rx_signal();                   /* if network driver */
}

/* Registration during init */
void register_interrupts(uint8_t irq_line)
{
    /* IDT vector = 32 + IRQ number (ISA IRQs start at vector 32) */
    idt_register_handler(32 + irq_line, mydrv_irq_handler);

    /* Unmask in both APIC IOAPIC and legacy PIC */
    if (apic_is_init_complete())
        ioapic_unmask_irq(irq_line);
    pic_unmask(irq_line);
}
```

### Named interrupt handlers (for /proc/interrupts)

```c
idt_register_handler_named(32 + irq_line, mydrv_irq_handler, "my_driver");
```

### MSI interrupts

```c
/* See PCI section above — pci_setup_interrupts() handles everything */
```

### IRQ handler best practices

- **Keep handlers fast.** Do minimal work (read status, clear source, schedule bottom-half if needed).
- **Hold spinlocks with IRQs disabled** when touching shared state (see Section 7).
- **Use `irq_ack()`** at the end to signal End-Of-Interrupt to the PIC/IOAPIC.
- **Network drivers** should call `net_rx_signal()` after acking to wake the network poll loop.

---

## 5. DMA Buffer Allocation

Use the physical memory manager for DMA-capable buffers. The kernel provides `pmm_alloc_frame()` (single 4 KB page) and `pmm_alloc_frames()` (contiguous multi-page block).

```c
#include "pmm.h"

/* Allocate a single physically-contiguous 4 KB page */
uint64_t dma_frame = pmm_alloc_frame();
if (!dma_frame) return -1;

uint64_t dma_phys = dma_frame * 4096;
void    *dma_virt = PHYS_TO_VIRT((void*)(uintptr_t)dma_phys);

memset(dma_virt, 0, 4096);

/* The physical address is what you program into the device's DMA register */
mydrv_write32(REG_DMA_ADDR_LO, (uint32_t)(dma_phys & 0xFFFFFFFF));
mydrv_write32(REG_DMA_ADDR_HI, (uint32_t)(dma_phys >> 32));

/* Free when done */
pmm_free_frame(dma_frame);
```

**Multi-page allocation:**

```c
/* Allocate N contiguous frames */
size_t count = 8;  /* 32 KB */
uint64_t *frames = pmm_alloc_frames(count);
if (!frames) return -1;

uint64_t phys_base = frames[0] * 4096;
void    *virt_base = PHYS_TO_VIRT((void*)(uintptr_t)phys_base);

/* ... use DMA buffers ... */

pmm_free_frames_contiguous(phys_base, count);
```

**Real-world example (nvme.c):**

```c
/* Allocate admin submission/ completion queue pages */
uint64_t sq_frame = pmm_alloc_frame();
uint64_t cq_frame = pmm_alloc_frame();

g_nvme_ctrl.admin_sq = PHYS_TO_VIRT((void*)(uintptr_t)(sq_frame * 4096));
g_nvme_ctrl.admin_cq = PHYS_TO_VIRT((void*)(uintptr_t)(cq_frame * 4096));
g_nvme_ctrl.admin_sq_phys = sq_frame * 4096;
g_nvme_ctrl.admin_cq_phys = cq_frame * 4096;

memset(g_nvme_ctrl.admin_sq, 0, 4096);
memset(g_nvme_ctrl.admin_cq, 0, 4096);

/* Program PRP (Physical Region Page) entries in NVMe commands */
cmd.prp1 = data_phys;
```

**Allocation constraints:**
- All `pmm_*` allocations return **physically contiguous** memory (required for DMA).
- Allocations are **page-aligned** (4 KB boundary).
- Convert between physical and virtual addresses using `PHYS_TO_VIRT()` / `VIRT_TO_PHYS()` macros (defined via the kernel's direct mapping).

---

## 6. Timer API

Use the dynamic timer subsystem for delayed work, polling loops, and periodic tasks.

### Timer API reference

```c
#include "timers.h"

/* Timer callback type */
typedef void (*timer_callback_t)(void *arg);

/* Schedule a one-shot timer */
int timer_schedule(timer_callback_t fn, void *arg, uint64_t delay_ticks);

/* Cancel a pending timer */
void timer_cancel(int timer_id);
```

### One-shot timer example

```c
static int g_my_timer_id = -1;

static void my_timer_callback(void *arg)
{
    (void)arg;
    kprintf("[mydrv] timer fired!\n");
    g_my_timer_id = -1;
}

void start_deferred_work(void)
{
    /* Fire after 50 timer ticks (500 ms at 100 Hz) */
    g_my_timer_id = timer_schedule(my_timer_callback, NULL, 50);
    if (g_my_timer_id < 0) {
        kprintf("[mydrv] failed to schedule timer\n");
    }
}
```

### Periodic / polling timer pattern

For periodic tasks, reschedule the timer at the end of each callback:

```c
static int g_poll_timer = -1;

static void poll_callback(void *arg)
{
    (void)arg;

    /* Poll hardware status */
    uint32_t status = mydrv_read32(REG_STATUS);
    if (status & STATUS_DATA_READY) {
        /* Process data... */
    }

    /* Re-schedule for next poll (every 10 ticks = 100 ms) */
    g_poll_timer = timer_schedule(poll_callback, NULL, 10);
}

void start_polling(void)
{
    g_poll_timer = timer_schedule(poll_callback, NULL, 10);
}

void stop_polling(void)
{
    timer_cancel(g_poll_timer);
    g_poll_timer = -1;
}
```

**Real-world examples from the codebase:**

| Driver | Usage |
|--------|-------|
| `watchdog.c` | Schedules periodic ticks; pet failure triggers reset |
| `acpi_thermal.c` | Periodic thermal zone polling |
| `bridge.c` | STP bridge protocol timer (1-tick scheduling) |
| `devfreq.c` | Device frequency scaling governor timer |

### Time conversion helpers

```c
#include "timer.h"

#define TIMER_FREQ 100              /* 100 Hz tick rate */
#define NS_PER_TICK (1000000000ULL / TIMER_FREQ)  /* 10 ms per tick */

uint64_t timer_get_ticks(void);     /* current tick count */
uint64_t timer_get_ns(void);        /* approximate nanoseconds */
uint64_t timer_get_ms(void);        /* approximate milliseconds */

/* Convert milliseconds to timer ticks */
uint64_t ms_to_ticks(uint64_t ms) {
    return ms * TIMER_FREQ / 1000;
}
```

---

## 7. Locking Requirements

Drivers that touch shared state (global data, device registers accessed from multiple CPUs) must use spinlocks. The kernel provides both plain and IRQ-safe variants.

### Plain spinlock (preemption-safe only)

```c
#include "spinlock.h"

static spinlock_t my_lock = SPINLOCK_INIT;   /* static initialiser */
/* or: spinlock_init(&my_lock); */

void my_unsafe_function(void)
{
    spinlock_acquire(&my_lock);
    /* critical section */
    spinlock_release(&my_lock);
}
```

### IRQ-safe spinlock (for interrupt handlers)

When a lock is shared between interrupt context and normal kernel code, use `spinlock_irqsave_acquire` / `spinlock_irqsave_release` — these disable local interrupts while the lock is held, preventing deadlocks:

```c
static spinlock_t g_state_lock = SPINLOCK_INIT;

void my_driver_ioctl(struct some_args *args)
{
    uint64_t flags;

    /* Save interrupt flag, disable IRQs, then acquire */
    spinlock_irqsave_acquire(&g_state_lock, &flags);
    /* critical section — IRQs are OFF, preemption is OFF */
    spinlock_irqsave_release(&g_state_lock, flags);
    /* interrupt state restored */
}

/* In the interrupt handler (IRQs already disabled by hardware) */
static void my_irq_handler(struct interrupt_frame *frame)
{
    uint64_t flags;
    spinlock_irqsave_acquire(&g_state_lock, &flags);
    /* ... */
    spinlock_irqsave_release(&g_state_lock, flags);
}
```

**Real-world example (e1000.c):**

```c
static spinlock_t e1000_lock = SPINLOCK_INIT;

static void e1000_irq_handler(struct interrupt_frame *frame)
{
    uint64_t __e1k_flags;
    spinlock_irqsave_acquire(&e1000_lock, &__e1k_flags);

    /* ... update descriptor rings ... */

    spinlock_irqsave_release(&e1000_lock, __e1k_flags);

    irq_ack(e1000_irq_line);
    net_rx_signal();
}
```

**Locking rules:**
- Always use `spinlock_irqsave_*` when the lock is accessed by both IRQ handlers and non-IRQ code.
- Never sleep while holding a spinlock (no `kprintf`-style I/O with consults, no blocking calls).
- The `spinlock_acquire_timeout(&lock, max_iters)` variant avoids indefinite spinning.

---

## 8. Exporting Symbols

Drivers that are compiled as loadable modules or provide an API to other modules can export symbols using `EXPORT_SYMBOL()` and `EXPORT_SYMBOL_GPL()`.

```c
#include "export.h"

/* Public — available to any module */
int my_helper_function(void)
{
    return 42;
}
EXPORT_SYMBOL(my_helper_function);

/* GPL-only — only modules with GPL-compatible license can use */
void my_gpl_function(void)
{
    /* ... */
}
EXPORT_SYMBOL_GPL(my_gpl_function);
```

**Real-world examples:**

```c
/* pci.c — exports PCI access functions for use by other drivers */
EXPORT_SYMBOL(pci_read);
EXPORT_SYMBOL(pci_write);
EXPORT_SYMBOL(pci_find_device);
EXPORT_SYMBOL(pci_find_class);
EXPORT_SYMBOL(pci_enable_msi);
EXPORT_SYMBOL(pci_enable_bus_master);

/* mpath.c — multipath I/O API */
EXPORT_SYMBOL(mpath_init);
EXPORT_SYMBOL(mpath_create);
EXPORT_SYMBOL(mpath_add_path);
EXPORT_SYMBOL(mpath_select_path);
```

**How it works:**
- Each `EXPORT_SYMBOL(fn)` creates a `struct ksym_entry` in the `.ksymtab` ELF section.
- At boot, `ksym_init()` sorts the table by name for binary-search lookups.
- The module loader calls `find_ksym(name, gpl_ok)` to resolve external references.
- Symbols marked `EXPORT_SYMBOL_GPL` are only returned when `gpl_ok` is true.

---

## 9. Makefile Integration

The kernel has two build models:

### Built-in drivers (always compiled)

Add your driver source file to the `C_SRCS` list in `Makefile` under the appropriate `src/drivers/` path:

```makefile
# In Makefile, under C_SRCS:
C_SRCS = ... \
    src/drivers/mydriver.c \
    ...
```

No other changes needed — all built-in drivers are compiled every time.

### Loadable kernel modules (optional .ko files)

1. Add the `.ko` to `obj-m`:
   ```makefile
   obj-m += drivers/mydriver.ko
   ```

2. For multi-file modules, specify the component objects:
   ```makefile
   mydriver-objs := drivers/mydriver_core drivers/mydriver_utils
   ```

3. The build system compiles each `.c → .o` with `-DMODULE` and partially links (via `ld -r`) into a `.ko` file.

4. Install with `make modules && make modules_install`.

**Example from the current Makefile:**

```makefile
# Single-file module
obj-m += drivers/e1000.ko
obj-m += drivers/nvme.ko

# Multi-file module
obj-m += drivers/usb.ko
usb-objs := drivers/usb_ehci drivers/usb_msc drivers/usb_hid \
            drivers/usb_cdc_acm drivers/usb_hub

# Module with subdirectory components
obj-m += doom.ko
doom-objs := doom/doom_task doom/doom_combat doom/doom_doors ...
```

### Dual built-in/module compilation

For drivers that can be compiled **either** as built-in or as a module, guard module-specific code with `#ifdef MODULE`:

```c
#ifdef MODULE
#include "module.h"

int init_module(void) { return my_driver_init(); }
void cleanup_module(void) { my_driver_exit(); }

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("My Awesome Driver");
MODULE_VERSION("1.0");
MODULE_ALIAS("pci:v00001234d00005678sv*sd*bc*sc*i*");
MODULE_DEPENDS("pci");
#endif
```

For built-in use, `module_init(my_driver_init)` already handles registration via the initcall system.

---

## 10. Module Metadata

Module metadata is embedded in the `.modinfo` section using macros:

| Macro | Purpose |
|-------|---------|
| `MODULE_LICENSE("GPL")` | License tag (GPL, GPL v2, BSD, MIT, Proprietary) |
| `MODULE_AUTHOR("name")` | Author name |
| `MODULE_DESCRIPTION("desc")` | Brief description |
| `MODULE_VERSION("1.0")` | Version string |
| `MODULE_ALIAS("pci:v*d*sv*sd*bc*sc*i*")` | Device alias for autoloading |
| `MODULE_DEPENDS("mod1", "mod2")` | Module dependency declaration |

### Module autoloading aliases

Module aliases use a glob-like pattern matching. The system generates a modalias string from the discovered device and searches for a matching module. Common formats:

- **PCI:** `pci:v0000VVVVd0000DDDDsv*sd*bc*sc*i*`
  - `VVVV` = vendor ID, `DDDD` = device ID
- **USB:** `usb:vVVVVpPPPPd*dc*dsc*dp*ic*isc*ip*`

Example (from nvme.c):
```c
MODULE_ALIAS("pci:v00008086d0000F1A5sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00001AF4d00005841sv*sd*bc*sc*i*");
```

---

## Appendix: Common Includes

| Header | Provides |
|--------|----------|
| `"pci.h"` | `pci_find_device`, `pci_find_class`, `pci_enable_bus_master`, BAR access |
| `"vmm.h"` | `vmm_map_phys`, `vmm_unmap_phys`, `PHYS_TO_VIRT`, `VIRT_TO_PHYS` |
| `"pmm.h"` | `pmm_alloc_frame`, `pmm_alloc_frames`, `pmm_free_frame` |
| `"idt.h"` | `idt_register_handler`, `isr_handler_t`, `struct interrupt_frame` |
| `"apic.h"` | `apic_is_init_complete`, `ioapic_unmask_irq` |
| `"pic.h"` | `pic_unmask`, legacy PIC support |
| `"timers.h"` | `timer_schedule`, `timer_cancel` |
| `"timer.h"` | `timer_get_ticks`, `TIMER_FREQ` |
| `"spinlock.h"` | `spinlock_t`, `spinlock_acquire`, `spinlock_irqsave_acquire` |
| `"export.h"` | `EXPORT_SYMBOL`, `EXPORT_SYMBOL_GPL` |
| `"printf.h"` | `kprintf` (kernel print) |
| `"string.h"` | `memset`, `memcpy`, `snprintf` |
| `"heap.h"` | `kmalloc`, `kfree` |
| `"module.h"` | `MODULE_LICENSE`, `module_init`, `module_exit`, `MODULE_ALIAS` |

---

*Based on the Hermes OS kernel driver patterns. See existing drivers under `src/drivers/` for complete reference implementations.*
