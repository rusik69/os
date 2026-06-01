#include "irq_regs.h"
#include "printf.h"
#include "kernel.h"
#include "smp.h"
#include "pmm.h"
#include "string.h"

/*
 * Per-CPU IRQ register save/restore with stack checking.
 *
 * Each CPU has:
 *   1. A dedicated IRQ stack (16 KB) allocated from PMM
 *   2. A frame stack of saved pt_regs pointers for nesting
 *   3. Stack boundary tracking for overflow detection
 *
 * On IRQ entry, the handler should call irq_stack_switch() to move to
 * the per-CPU IRQ stack (if not already there), then call set_irq_regs()
 * to record the register frame.  On exit, irq_stack_restore() reverts
 * the stack and get_irq_regs() returns the last saved frame.
 */

/* ── Per-CPU data (static arrays since no dynamic per-CPU allocator) ── */

static struct irq_regs_cpu irq_regs_per_cpu[SMP_MAX_CPUS];

/* Per-CPU IRQ stack tracking */
static struct irq_stack_info irq_stacks[SMP_MAX_CPUS];

/* Physical addresses of IRQ stacks (for cleanup / debugging) */
static uint64_t irq_stack_phys[SMP_MAX_CPUS];

/* ── Forward declarations ──────────────────────────────────────────── */

static int current_cpu(void);
static uint64_t read_rsp(void);

/* ── Current CPU helper ────────────────────────────────────────────── */

static int current_cpu(void)
{
    /* Read GS_BASE MSR to get the per-CPU cpu_info pointer,
     * then extract cpu_id from the first field.  This is the
     * correct portable approach (unlike reading gs:0 directly). */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101));
    uint64_t gs_base = ((uint64_t)hi << 32) | lo;
    struct cpu_info *info = (struct cpu_info *)gs_base;

    if (!info)
        return 0;
    int cpu = (int)info->cpu_id;
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        return 0;
    return cpu;
}

/* ── Read current stack pointer ────────────────────────────────────── */

static uint64_t read_rsp(void)
{
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}

/* ── IRQ stack allocation ──────────────────────────────────────────── */

static int allocate_irq_stack(int cpu_id)
{
    /* Allocate IRQ_STACK_SIZE / PAGE_SIZE pages for the per-CPU IRQ stack */
    uint64_t phys = (uint64_t)(uintptr_t)pmm_alloc_frames(IRQ_STACK_SIZE / PAGE_SIZE);
    if (!phys) {
        kprintf("[!!] irq_regs: failed to allocate IRQ stack for CPU %d\n", cpu_id);
        return -1;
    }

    uint64_t virt = (uint64_t)PHYS_TO_VIRT((uint64_t)phys);

    /* Store physical address for potential debug/cleanup */
    irq_stack_phys[cpu_id] = phys;

    /* Record stack boundaries.
     * Stack grows downward: stack_top is the highest address,
     * stack_bottom is the lowest. */
    irq_stacks[cpu_id].stack_bottom  = virt;
    irq_stacks[cpu_id].stack_top     = virt + IRQ_STACK_SIZE;
    irq_stacks[cpu_id].stack_watermark = virt + IRQ_STACK_SIZE; /* highest RSP seen */
    irq_stacks[cpu_id].magic         = IRQ_STACK_MAGIC;
    irq_stacks[cpu_id].cpu_id        = cpu_id;

    /* Place a magic value at the bottom (low end) of the stack for
     * overflow detection.  If RSP ever reaches this region, we've
     * overflowed. */
    *((volatile uint64_t *)virt) = IRQ_STACK_MAGIC;

    /* Poison the entire stack with a known pattern to detect
     * uninitialized stack usage or overflow after the fact. */
    memset((void *)virt, 0xCC, IRQ_STACK_SIZE);

    kprintf("[OK] irq_regs: CPU %d IRQ stack: virt 0x%llx-0x%llx phys 0x%llx\n",
            cpu_id,
            (unsigned long long)virt,
            (unsigned long long)(virt + IRQ_STACK_SIZE),
            (unsigned long long)phys);

    return 0;
}

/* ── Stack switch: enter IRQ context on per-CPU stack ─────────────── */

uint64_t irq_stack_switch(void)
{
    int cpu = current_cpu();
    uint64_t current_rsp = read_rsp();

    /* If we're already on the IRQ stack, this is a nested interrupt.
     * No need to switch — just return 0 to indicate nesting. */
    if (current_rsp >= irq_stacks[cpu].stack_bottom &&
        current_rsp <  irq_stacks[cpu].stack_top) {
        return 0;
    }

    /* Save the old RSP (the interrupted task's stack pointer) and
     * switch to the top of the per-CPU IRQ stack.
     *
     * NOTE: This function returns with RSP set to the IRQ stack.
     * The caller must preserve the old RSP and pass it to
     * irq_stack_restore() when leaving IRQ context. */
    uint64_t prev_rsp = current_rsp;
    uint64_t new_rsp  = irq_stacks[cpu].stack_top;

    __asm__ volatile("mov %0, %%rsp" : : "r"(new_rsp) : "memory");

    return prev_rsp;
}

/* ── Stack restore: leave IRQ context, revert to original stack ────── */

void irq_stack_restore(uint64_t prev_rsp)
{
    if (prev_rsp == 0) {
        /* Nested interrupt — already on IRQ stack, nothing to restore */
        return;
    }
    __asm__ volatile("mov %0, %%rsp" : : "r"(prev_rsp) : "memory");
}

/* ── Stack check: verify IRQ stack integrity ───────────────────────── */

int irq_stack_check(void)
{
    int cpu = current_cpu();
    uint64_t current_rsp = read_rsp();
    struct irq_stack_info *info = &irq_stacks[cpu];

    /* If this CPU's IRQ stack hasn't been allocated yet, skip checks */
    if (info->magic != IRQ_STACK_MAGIC)
        return -1;

    /* Check I: Is current RSP within the per-CPU IRQ stack? */
    int on_irq_stack = (current_rsp >= info->stack_bottom &&
                        current_rsp <  info->stack_top);

    if (!on_irq_stack) {
        /* We may be running in process context — that's normal.
         * Only warn if we're in IRQ context (depth > 0) but not on
         * the IRQ stack, which means we're using the task's stack. */
        if (irq_regs_per_cpu[cpu].depth > 0) {
            kprintf("[!!] irq_stack_check: CPU %d in IRQ context but not on IRQ stack!\n"
                    "    RSP=0x%llx stack=[0x%llx-0x%llx)\n",
                    cpu,
                    (unsigned long long)current_rsp,
                    (unsigned long long)info->stack_bottom,
                    (unsigned long long)info->stack_top);
            return -1;
        }
        return 0; /* Process context, not on IRQ stack — normal */
    }

    /* Check II: Stack overflow detection — verify magic is intact */
    uint64_t magic_val = *((volatile uint64_t *)info->stack_bottom);
    if (magic_val != IRQ_STACK_MAGIC) {
        kprintf("[!!] irq_stack_check: CPU %d IRQ stack OVERFLOW detected!\n"
                "    RSP=0x%llx magic=0x%llx (expected 0x%llx)\n",
                cpu,
                (unsigned long long)current_rsp,
                (unsigned long long)magic_val,
                (unsigned long long)IRQ_STACK_MAGIC);
        return -1;
    }

    /* Check III: Update stack watermark (lowest RSP seen) */
    if (current_rsp < info->stack_watermark) {
        info->stack_watermark = current_rsp;
    }

    /* Check IV: Ensure at least 256 bytes of headroom remain */
    uint64_t headroom = current_rsp - info->stack_bottom;
    if (headroom < 256) {
        kprintf("[!!] irq_stack_check: CPU %d IRQ stack critically low!\n"
                "    RSP=0x%llx headroom=%llu bytes\n",
                cpu,
                (unsigned long long)current_rsp,
                (unsigned long long)headroom);
        return -1;
    }

    return 0;
}

/* ── Check if currently on IRQ stack ───────────────────────────────── */

int in_irq_stack(void)
{
    int cpu = current_cpu();
    uint64_t current_rsp = read_rsp();

    if (irq_stacks[cpu].magic != IRQ_STACK_MAGIC)
        return 0;

    return (current_rsp >= irq_stacks[cpu].stack_bottom &&
            current_rsp <  irq_stacks[cpu].stack_top) ? 1 : 0;
}

/* ── Register save/restore with stack checking ────────────────────── */

struct pt_regs *set_irq_regs(struct pt_regs *regs)
{
    int cpu = current_cpu();
    struct irq_regs_cpu *cpu_state = &irq_regs_per_cpu[cpu];
    struct pt_regs *previous = NULL;

    /* Perform stack integrity check */
    irq_stack_check();

    if (cpu_state->depth > 0)
        previous = cpu_state->frames[cpu_state->depth - 1];

    if (cpu_state->depth >= IRQ_REGS_MAX_FRAMES) {
        /* Nesting overflow — this is a serious issue */
        kprintf("[!!] irq_regs: CPU %d IRQ nesting depth overflow!\n"
                "    depth=%d max=%d\n",
                cpu,
                cpu_state->depth,
                IRQ_REGS_MAX_FRAMES);
        return previous;
    }

    cpu_state->frames[cpu_state->depth++] = regs;

    return previous;
}

struct pt_regs *get_irq_regs(void)
{
    int cpu = current_cpu();
    struct irq_regs_cpu *cpu_state = &irq_regs_per_cpu[cpu];
    struct pt_regs *regs = NULL;

    if (cpu_state->depth > 0)
        regs = cpu_state->frames[cpu_state->depth - 1];

    return regs;
}

/* ── Nesting depth query ──────────────────────────────────────────── */

int irq_nesting_depth(void)
{
    int cpu = current_cpu();
    return irq_regs_per_cpu[cpu].depth;
}

/* ── Initialization ────────────────────────────────────────────────── */

void irq_regs_init(void)
{
    int max_cpus;

    /* Clear per-CPU structures */
    memset(irq_regs_per_cpu, 0, sizeof(irq_regs_per_cpu));
    memset(irq_stacks, 0, sizeof(irq_stacks));
    memset(irq_stack_phys, 0, sizeof(irq_stack_phys));

    /* Determine how many CPUs we need stacks for */
    max_cpus = smp_get_cpu_count();
    if (max_cpus < 1) max_cpus = 1;
    if (max_cpus > SMP_MAX_CPUS) max_cpus = SMP_MAX_CPUS;

    /* Allocate per-CPU IRQ stacks */
    for (int i = 0; i < max_cpus; i++) {
        if (allocate_irq_stack(i) != 0) {
            /* If allocation fails, mark as not ready */
            irq_stacks[i].magic = 0;
        }
    }

    kprintf("[OK] irq_regs: IRQ stacks + register save/restore initialised (%d CPUs)\n",
            max_cpus);
}
