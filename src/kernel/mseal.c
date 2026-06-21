#include "mseal.h"
#include "kernel.h"
#include "printf.h"
#include "errno.h"

static struct mseal_range mseal_ranges[MSEAL_SEAL_COUNT];
static int mseal_initialised = 0;

void mseal_init(void)
{
    if (mseal_initialised)
        return;

    for (int i = 0; i < MSEAL_SEAL_COUNT; i++) {
        mseal_ranges[i].used = 0;
        mseal_ranges[i].virt_start = 0;
        mseal_ranges[i].virt_end = 0;
    }
    mseal_initialised = 1;
    kprintf("mseal: initialised with %d range slots\n", MSEAL_SEAL_COUNT);
}

static int mseal_find_free(void)
{
    for (int i = 0; i < MSEAL_SEAL_COUNT; i++) {
        if (!mseal_ranges[i].used)
            return i;
    }
    return -ENOSPC;
}

int mseal(uint64_t addr, uint64_t len, int flags)
{
    (void)flags;

    if (addr + len < addr)  /* overflow */
        return -EINVAL;
    if (len == 0)
        return -EINVAL;

    /* Check for overlap with existing sealed ranges. */
    for (int i = 0; i < MSEAL_SEAL_COUNT; i++) {
        if (!mseal_ranges[i].used)
            continue;
        uint64_t a_start = addr;
        uint64_t a_end   = addr + len;
        uint64_t b_start = mseal_ranges[i].virt_start;
        uint64_t b_end   = mseal_ranges[i].virt_end;
        /* Allow coalescing if ranges are adjacent. */
        if (a_end == b_start || b_end == a_start)
            continue;
        /* Check mutual overlap. */
        if (a_start < b_end && b_start < a_end)
            return -EINVAL;  /* overlaps an existing sealed region */
    }

    int idx = mseal_find_free();
    if (idx < 0)
        return idx;

    mseal_ranges[idx].virt_start = addr;
    mseal_ranges[idx].virt_end   = addr + len;
    mseal_ranges[idx].used = 1;

    return 0;
}

int mseal_check(uint64_t addr, uint64_t len)
{
    if (addr + len < addr)
        return -EINVAL;
    if (len == 0)
        return -EINVAL;

    for (int i = 0; i < MSEAL_SEAL_COUNT; i++) {
        if (!mseal_ranges[i].used)
            continue;
        uint64_t r_start = mseal_ranges[i].virt_start;
        uint64_t r_end   = mseal_ranges[i].virt_end;
        /* Check if the query range falls entirely within this sealed range. */
        if (addr >= r_start && addr + len <= r_end)
            return 0;
    }
    return -EACCES;
}

int mseal_is_sealed(uint64_t addr)
{
    if (!mseal_initialised)
        return 0;

    for (int i = 0; i < MSEAL_SEAL_COUNT; i++) {
        if (!mseal_ranges[i].used)
            continue;
        if (addr >= mseal_ranges[i].virt_start && addr < mseal_ranges[i].virt_end)
            return 1;
    }
    return 0;
}

/* ── Stub: mseal_seal ─────────────────────────────── */
int mseal_seal(uint64_t addr, size_t len)
{
    (void)addr;
    (void)len;
    kprintf("[mseal] mseal_seal: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: mseal_unseal ─────────────────────────────── */
int mseal_unseal(uint64_t addr, size_t len)
{
    (void)addr;
    (void)len;
    kprintf("[mseal] mseal_unseal: not yet implemented\n");
    return -ENOSYS;
}
