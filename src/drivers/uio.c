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
    kprintf("[uio] register phys=0x%llx size=%zu\n", (unsigned long long)phys_addr, size);
    return uio_count++;
}
