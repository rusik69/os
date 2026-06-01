#ifndef IO_MAP_H
#define IO_MAP_H

#include "types.h"

/*
 * I/O memory mapping abstraction.
 *
 * Provides a unified interface for mapping physical I/O memory regions
 * into the kernel's virtual address space, with tracking and reference
 * counting.  Based on the Linux ioremap concept.
 *
 * On x86-64 this typically uses page-table manipulation to create
 * uncached mappings of device MMIO regions.
 */

/* I/O mapping flags */
#define IO_MAP_UNCACHED         (1 << 0)    /* strongly-ordered (UC) */
#define IO_MAP_WC               (1 << 1)    /* write-combining (WC) */
#define IO_MAP_CACHED           (1 << 2)    /* normal cached (WB) */
#define IO_MAP_READONLY         (1 << 3)    /* read-only mapping */

/* Maximum number of simultaneous I/O mappings */
#define IO_MAP_MAX_MAPPINGS     64

struct io_map {
    void    *virt_addr;     /* kernel virtual address */
    uint64_t phys_addr;     /* physical base address */
    size_t   size;          /* mapped size in bytes */
    int      flags;         /* mapping flags */
    int      refcount;      /* reference count */
    int      in_use;        /* slot is occupied */
};

/*
 * io_map_create  - Map a physical I/O region into virtual address space.
 * @phys_addr:  physical base address of the region
 * @size:       size of the region in bytes
 * @flags:      mapping flags (IO_MAP_*)
 *
 * Returns a kernel virtual address, or NULL on failure.
 */
void *io_map_create(uint64_t phys_addr, size_t size, int flags);

/*
 * io_map_destroy  - Unmap an I/O mapping previously created with
 *                   io_map_create().
 * @virt_addr:  virtual address returned by io_map_create()
 */
void io_map_destroy(void *virt_addr);

/*
 * io_map_read  - Read a 32-bit value from an I/O mapped address.
 */
uint32_t io_map_read(const volatile void *addr);

/*
 * io_map_write  - Write a 32-bit value to an I/O mapped address.
 */
void io_map_write(volatile void *addr, uint32_t value);

/*
 * io_map_read8  - Read an 8-bit value from an I/O mapped address.
 */
uint8_t io_map_read8(const volatile void *addr);

/*
 * io_map_write8  - Write an 8-bit value to an I/O mapped address.
 */
void io_map_write8(volatile void *addr, uint8_t value);

/*
 * io_map_init  - Initialise the I/O memory mapping subsystem.
 */
void io_map_init(void);

#endif /* IO_MAP_H */
