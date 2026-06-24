#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "uio.h"
#include "string.h"
#define MAX_UIO 8
static int uio_count = 0;
void uio_init(void) {
    kprintf("[OK] UIO subsystem initialized\n");
}
int uio_register(uint64_t phys_addr, size_t size) {
    if (uio_count >= MAX_UIO) return -1;
    kprintf("[UIO] register phys=0x%llx size=%llu\n", (unsigned long long)phys_addr, (unsigned long long)size);
    return uio_count++;
}

/* ── Stub: uio_open ─────────────────────────────── */
int uio_open(void *file)
{
    (void)file;
    kprintf("[UIO] uio_open: not yet implemented\n");
    return 0;
}
/* ── Stub: uio_release ─────────────────────────────── */
int uio_release(void *file)
{
    (void)file;
    kprintf("[UIO] uio_release: not yet implemented\n");
    return 0;
}
/* ── Stub: uio_read ─────────────────────────────── */
int uio_read(void *file, void *buf, size_t count)
{
    (void)file;
    (void)buf;
    (void)count;
    kprintf("[UIO] uio_read: not yet implemented\n");
    return 0;
}
/* ── Stub: uio_write ─────────────────────────────── */
int uio_write(void *file, const void *buf, size_t count)
{
    (void)file;
    (void)buf;
    (void)count;
    kprintf("[UIO] uio_write: not yet implemented\n");
    return 0;
}
/* ── Stub: uio_mmap ─────────────────────────────── */
int uio_mmap(void *file, void *vma)
{
    (void)file;
    (void)vma;
    kprintf("[UIO] uio_mmap: not yet implemented\n");
    return 0;
}
