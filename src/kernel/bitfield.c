#include "bitfield.h"
#include "printf.h"
#include "kernel.h"

/*
 * bitfield.c – placeholder for any runtime initialisation needed.
 * All bitfield operations are implemented as macros/inlines in the header.
 */

void bitfield_init(void)
{
    kprintf("[OK] bitfield: BIT/GENMASK/FIELD_GET/FIELD_PREP macros ready\n");
}

/* ── Stub: bitfield_read ─────────────────────────────── */
uint64_t bitfield_read(const void *addr, int start, int end)
{
    (void)addr;
    (void)start;
    (void)end;
    kprintf("[bitfield] bitfield_read: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: bitfield_write ─────────────────────────────── */
int bitfield_write(void *addr, int start, int end, uint64_t val)
{
    (void)addr;
    (void)start;
    (void)end;
    (void)val;
    kprintf("[bitfield] bitfield_write: not yet implemented\n");
    return -ENOSYS;
}
