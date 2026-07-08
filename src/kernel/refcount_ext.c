#include "refcount_ext.h"
#include "printf.h"
#include "kernel.h"

#define REFCOUNT_SATURATED  (INT32_MAX / 2)

static inline int refcount_warn_saturate(struct refcount_struct *r,
                                          const char *msg)
{
    kprintf("[WARN] refcount: %s (refs=%d)\n", msg, atomic_read(&r->refs));
    atomic_set(&r->refs, REFCOUNT_SATURATED);
    return 0;
}

void refcount_inc(struct refcount_struct *r)
{
    int old = atomic_add_return(&r->refs, 1);
    if (old <= 0)
        refcount_warn_saturate(r, "refcount_inc: saturated/negative");
}

void refcount_dec(struct refcount_struct *r)
{
    int old = atomic_sub_return(&r->refs, 1);
    if (old <= 0)
        refcount_warn_saturate(r, "refcount_dec: underflow");
}

int refcount_inc_not_zero(struct refcount_struct *r)
{
    int old, new;

    do {
        old = atomic_read(&r->refs);
        if (old <= 0)
            return 0;
        new = old + 1;
    } while (__sync_val_compare_and_swap(&r->refs.counter, old, new) != old);

    return 1;
}

int refcount_sub_and_test(struct refcount_struct *r, int i)
{
    int old = atomic_sub_return(&r->refs, i);
    if (old < 0)
        refcount_warn_saturate(r, "refcount_sub_and_test: underflow");
    return old == 0;
}

void refcount_ext_init(void)
{
    kprintf("[OK] refcount_ext: Extended refcount with saturation initialised\n");
}

/* ── Stub: refcount_ext_inc ─────────────────────────────── */
static int refcount_ext_inc(void *r)
{
    (void)r;
    kprintf("[refcount] refcount_ext_inc: not yet implemented\n");
    return 0;
}
/* ── Stub: refcount_ext_dec ─────────────────────────────── */
static int refcount_ext_dec(void *r)
{
    (void)r;
    kprintf("[refcount] refcount_ext_dec: not yet implemented\n");
    return 0;
}
/* ── Stub: refcount_ext_read ─────────────────────────────── */
static unsigned int refcount_ext_read(void *r)
{
    (void)r;
    kprintf("[refcount] refcount_ext_read: not yet implemented\n");
    return 0;
}
/* ── Stub: refcount_ext_set ─────────────────────────────── */
static int refcount_ext_set(void *r, unsigned int val)
{
    (void)r;
    (void)val;
    kprintf("[refcount] refcount_ext_set: not yet implemented\n");
    return 0;
}
