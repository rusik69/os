#include "io_map.h"
#include "printf.h"
#include "kernel.h"
#include "string.h"
#include "spinlock.h"
#include "export.h"
/*
 * I/O memory mapping.
 *
 * Manages a fixed-size table of ioremap-style mappings.  In a real
 * system io_map_create() would modify the kernel page tables to create
 * an uncached mapping of the physical region.  Here we maintain the
 * bookkeeping and provide accessor helpers.
 */

static struct io_map io_mappings[IO_MAP_MAX_MAPPINGS];
static spinlock_t io_map_lock;

/* Track the next virtual address to hand out (simulated) */
static uint64_t next_virt_base = 0xFFFF800000000000ULL;

void *io_map_create(uint64_t phys_addr, size_t size, int flags)
{
    if (size == 0)
        return NULL;

    spinlock_acquire(&io_map_lock);

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < IO_MAP_MAX_MAPPINGS; i++) {
        if (!io_mappings[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        spinlock_release(&io_map_lock);
        return NULL;
    }

    /* Allocate virtual address range (simulated) */
    void *virt = (void *)(uintptr_t)next_virt_base;
    next_virt_base += size;
    next_virt_base = (next_virt_base + 0xFFF) & ~0xFFFULL;

    io_mappings[slot].virt_addr = virt;
    io_mappings[slot].phys_addr = phys_addr;
    io_mappings[slot].size      = size;
    io_mappings[slot].flags     = flags;
    io_mappings[slot].refcount  = 1;
    io_mappings[slot].in_use    = 1;

    spinlock_release(&io_map_lock);

    /* In a real kernel, install page-table entries here.
     * On x86-64: set PAT bits for uncached/WC/etc. */
    kprintf("io_map: mapped phys %llx size %llu -> virt %p flags %d\n",
            phys_addr, size, virt, flags);

    return virt;
}

void io_map_destroy(void *virt_addr)
{
    if (!virt_addr)
        return;

    spinlock_acquire(&io_map_lock);

    for (int i = 0; i < IO_MAP_MAX_MAPPINGS; i++) {
        if (io_mappings[i].in_use &&
            io_mappings[i].virt_addr == virt_addr) {

            io_mappings[i].refcount--;
            if (io_mappings[i].refcount <= 0) {
                /* Remove page-table entries (stub) */
                io_mappings[i].in_use = 0;
                io_mappings[i].virt_addr = NULL;
                kprintf("io_map: unmapped virt %p\n", virt_addr);
            }
            spinlock_release(&io_map_lock);
            return;
        }
    }

    spinlock_release(&io_map_lock);
}

uint32_t io_map_read(const volatile void *addr)
{
    if (!addr) return 0;
    return *(volatile uint32_t *)addr;
}

void io_map_write(volatile void *addr, uint32_t value)
{
    if (!addr) return;
    *(volatile uint32_t *)addr = value;
}

uint8_t io_map_read8(const volatile void *addr)
{
    if (!addr) return 0;
    return *(volatile uint8_t *)addr;
}

void io_map_write8(volatile void *addr, uint8_t value)
{
    if (!addr) return;
    *(volatile uint8_t *)addr = value;
}

void io_map_init(void)
{
    spinlock_init(&io_map_lock);

    for (int i = 0; i < IO_MAP_MAX_MAPPINGS; i++) {
        io_mappings[i].in_use = 0;
        io_mappings[i].virt_addr = NULL;
        io_mappings[i].refcount = 0;
    }

    kprintf("[OK] io_map: I/O memory mapping initialised\n");
}

/* ── Exported symbols for driver modules ─────────────────────────── */
EXPORT_SYMBOL(io_map_create);
EXPORT_SYMBOL(io_map_destroy);
EXPORT_SYMBOL(io_map_read);
EXPORT_SYMBOL(io_map_write);
EXPORT_SYMBOL(io_map_read8);
EXPORT_SYMBOL(io_map_write8);

/* ── Stub: io_map_read32 ─────────────────────────────── */
uint32_t io_map_read32(uintptr_t addr)
{
    (void)addr;
    kprintf("[iomap] io_map_read32: not yet implemented\n");
    return 0;
}
/* ── Stub: io_map_write32 ─────────────────────────────── */
int io_map_write32(uintptr_t addr, uint32_t val)
{
    (void)addr;
    (void)val;
    kprintf("[iomap] io_map_write32: not yet implemented\n");
    return 0;
}
