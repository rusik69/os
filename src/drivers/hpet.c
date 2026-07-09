#define KERNEL_INTERNAL
#include "hpet.h"
#include "printf.h"
#include "types.h"
#include "io.h"
#include "pci.h"
#define HPET_BASE 0xFED00000
static int hpet_present = 0;
void hpet_init(void) {
    volatile uint32_t *cfg = (volatile uint32_t*)(uintptr_t)HPET_BASE;
    if (cfg && (cfg[0] & 0xFFFFFFFF) != 0xFFFFFFFF) {
        hpet_present = 1;
        kprintf("[OK] HPET timer detected at 0xFED00000\n");
    } else {
        kprintf("[--] No HPET timer found\n");
    }
}
int hpet_is_present(void) { return hpet_present; }

/* ── Stub: hpet_read ─────────────────────────────── */
static int hpet_read(void *buf, size_t count)
{
    (void)buf;
    (void)count;
    kprintf("[HPET] hpet_read: not yet implemented\n");
    return 0;
}
/* ── Stub: hpet_ioctl ─────────────────────────────── */
static int hpet_ioctl(int cmd, void *arg)
{
    (void)cmd;
    (void)arg;
    kprintf("[HPET] hpet_ioctl: not yet implemented\n");
    return 0;
}
