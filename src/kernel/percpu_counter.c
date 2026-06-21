#include "percpu_counter.h"
#include "printf.h"
#include "kernel.h"
#include "heap.h"
#include "smp.h"
#include "errno.h"
#include "string.h"

int percpu_counter_init(struct percpu_counter *fbc, int64_t value, int batch)
{
    if (!fbc)
        return -EINVAL;

    fbc->num_cpus = smp_cpu_count;
    if (fbc->num_cpus < 1)
        fbc->num_cpus = 1;
    if (fbc->num_cpus > SMP_MAX_CPUS)
        fbc->num_cpus = SMP_MAX_CPUS;

    fbc->percpu = (int64_t *)kmalloc(fbc->num_cpus * sizeof(int64_t));
    if (!fbc->percpu)
        return -ENOMEM;

    memset(fbc->percpu, 0, fbc->num_cpus * sizeof(int64_t));
    fbc->count   = value;
    fbc->batch   = (batch > 0) ? batch : 32;
    spinlock_init(&fbc->lock);

    return 0;
}

void percpu_counter_add(struct percpu_counter *fbc, int64_t amount)
{
    int cpu = smp_get_cpu_id();
    int64_t *pcount;

    if (!fbc || !fbc->percpu)
        return;

    if (cpu < 0 || cpu >= fbc->num_cpus)
        cpu = 0;

    pcount = &fbc->percpu[cpu];
    *pcount += amount;

    /*
     * If the per-CPU delta exceeds batch size, fold into the
     * global count under lock to keep per-CPU deltas small.
     */
    if (*pcount >= fbc->batch || *pcount <= -fbc->batch) {
        spinlock_acquire(&fbc->lock);
        fbc->count += *pcount;
        *pcount = 0;
        spinlock_release(&fbc->lock);
    }
}

int64_t percpu_counter_sum(struct percpu_counter *fbc)
{
    int64_t sum;
    int i;

    if (!fbc || !fbc->percpu)
        return 0;

    spinlock_acquire(&fbc->lock);
    sum = fbc->count;
    for (i = 0; i < fbc->num_cpus; i++)
        sum += fbc->percpu[i];
    spinlock_release(&fbc->lock);

    return sum;
}

void percpu_counter_set(struct percpu_counter *fbc, int64_t value)
{
    int i;

    if (!fbc || !fbc->percpu)
        return;

    spinlock_acquire(&fbc->lock);
    fbc->count = value;
    for (i = 0; i < fbc->num_cpus; i++)
        fbc->percpu[i] = 0;
    spinlock_release(&fbc->lock);
}

void percpu_counter_destroy(struct percpu_counter *fbc)
{
    if (!fbc || !fbc->percpu)
        return;

    kfree(fbc->percpu);
    fbc->percpu = NULL;
    fbc->count  = 0;
}

void percpu_counter_init_global(void)
{
    kprintf("[OK] percpu_counter: Per-CPU counters initialised\n");
}

/* ── Stub: percpu_counter_sum ─────────────────────────────── */
int64_t percpu_counter_sum(void *fbc)
{
    (void)fbc;
    kprintf("[percpu] percpu_counter_sum: not yet implemented\n");
    return -ENOSYS;
}
