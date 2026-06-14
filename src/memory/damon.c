// SPDX-License-Identifier: GPL-2.0-only
/*
 * damon.c — Data Access Monitor (DAMON) skeleton
 *
 * Monitors data access patterns of memory regions to enable
 * proactive memory management (reclaim, migration, etc.).
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"
#include "timer.h"

#define DAMON_MAX_REGIONS   256
#define DAMON_MIN_REGION    4096
#define DAMON_SAMPLE_INTERVAL 5000 /* 5ms in timer ticks */

struct damon_region {
    uint64_t start;
    uint64_t end;
    uint64_t nr_accesses;
    uint64_t last_sample_time;
    int accessed;
};

struct damon_ctx {
    struct damon_region regions[DAMON_MAX_REGIONS];
    int nr_regions;
    uint64_t sample_interval;
    uint64_t monitoring_start;
    int running;
    spinlock_t lock;
};

static struct damon_ctx damon_ctx;

/* Add a region to monitor */
int damon_add_region(uint64_t start, uint64_t end)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&damon_ctx.lock, &irq_flags);

    if (damon_ctx.nr_regions >= DAMON_MAX_REGIONS) {
        spinlock_irqsave_release(&damon_ctx.lock, irq_flags);
        return -ENOMEM;
    }

    struct damon_region *r = &damon_ctx.regions[damon_ctx.nr_regions];
    r->start = start;
    r->end = end;
    r->nr_accesses = 0;
    r->last_sample_time = timer_get_ticks();
    r->accessed = 0;
    damon_ctx.nr_regions++;

    spinlock_irqsave_release(&damon_ctx.lock, irq_flags);
    return 0;
}

/* Remove a region */
int damon_remove_region(uint64_t start)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&damon_ctx.lock, &irq_flags);

    int found = 0;
    for (int i = 0; i < damon_ctx.nr_regions; i++) {
        if (damon_ctx.regions[i].start == start) {
            /* Shift remaining regions */
            for (int j = i; j < damon_ctx.nr_regions - 1; j++)
                damon_ctx.regions[j] = damon_ctx.regions[j + 1];
            damon_ctx.nr_regions--;
            found = 1;
            break;
        }
    }

    spinlock_irqsave_release(&damon_ctx.lock, irq_flags);
    return found ? 0 : -ENOENT;
}

/* Sample access patterns (check if pages are accessed via PTE A/D bits) */
void damon_sample(void)
{
    if (!damon_ctx.running)
        return;

    uint64_t now = timer_get_ticks();
    uint64_t irq_flags;

    spinlock_irqsave_acquire(&damon_ctx.lock, &irq_flags);

    for (int i = 0; i < damon_ctx.nr_regions; i++) {
        struct damon_region *r = &damon_ctx.regions[i];

        if (now - r->last_sample_time >= damon_ctx.sample_interval) {
            /* Simulate access check — in real implementation, check PTE A bit */
            r->nr_accesses += r->accessed ? 1 : 0;
            r->accessed = 0;
            r->last_sample_time = now;
        }
    }

    spinlock_irqsave_release(&damon_ctx.lock, irq_flags);
}

/* Mark a page as accessed (called from page fault handler) */
void damon_mark_accessed(uint64_t addr)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&damon_ctx.lock, &irq_flags);

    for (int i = 0; i < damon_ctx.nr_regions; i++) {
        struct damon_region *r = &damon_ctx.regions[i];
        if (addr >= r->start && addr < r->end) {
            r->accessed = 1;
            break;
        }
    }

    spinlock_irqsave_release(&damon_ctx.lock, irq_flags);
}

/* Start monitoring */
int damon_start(void)
{
    if (damon_ctx.running)
        return -EBUSY;

    damon_ctx.running = 1;
    damon_ctx.monitoring_start = timer_get_ticks();
    kprintf("[DAMON] Data access monitoring started\n");
    return 0;
}

/* Stop monitoring */
int damon_stop(void)
{
    damon_ctx.running = 0;
    kprintf("[DAMON] Data access monitoring stopped\n");
    return 0;
}

/* Get access statistics for a region */
uint64_t damon_get_access_count(uint64_t start)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&damon_ctx.lock, &irq_flags);

    for (int i = 0; i < damon_ctx.nr_regions; i++) {
        if (damon_ctx.regions[i].start == start) {
            uint64_t count = damon_ctx.regions[i].nr_accesses;
            spinlock_irqsave_release(&damon_ctx.lock, irq_flags);
            return count;
        }
    }

    spinlock_irqsave_release(&damon_ctx.lock, irq_flags);
    return 0;
}

void damon_init(void)
{
    memset(&damon_ctx, 0, sizeof(damon_ctx));
    spinlock_init(&damon_ctx.lock);
    damon_ctx.sample_interval = DAMON_SAMPLE_INTERVAL;
    kprintf("[OK] DAMON — Data Access Monitor skeleton\n");
}
