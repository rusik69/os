/* scs.c — Shadow Call Stack
 *
 * Implements a separate shadow stack for return addresses, providing
 * protection against return-oriented programming (ROP) attacks.
 *
 * On function call, the return address is pushed to both the regular
 * stack and the shadow stack.  On function return, the addresses
 * are compared; a mismatch indicates the regular stack was corrupted.
 *
 * This is similar to ARM64's SCS and x86-64's Intel CET shadow stack,
 * but implemented in software as a defence-in-depth measure.
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "panic.h"
#include "errno.h"

/* ── Configuration ─────────────────────────────────────────────────── */

#define SCS_PER_PROCESS         1       /* allocate per-process shadow stack */
#define SCS_DEFAULT_SIZE        4096    /* 4 KB per shadow stack */
#define SCS_MAGIC               0x5C5C5C5C5C5C5C5Full

/* ── Shadow stack entry (per-process) ──────────────────────────────── */

struct scs_shadow_stack {
    uint64_t magic;             /* integrity check */
    uint64_t *base;             /* base of shadow stack memory */
    uint64_t *sp;               /* current shadow stack pointer (grows down) */
    size_t    size;             /* total size in bytes */
    int       in_use;
};

/* ── Globals ───────────────────────────────────────────────────────── */

#define SCS_MAX_STACKS 256
static struct scs_shadow_stack g_scs_stacks[SCS_MAX_STACKS];
static int g_scs_num_stacks = 0;
static int scs_enabled = 1;

/* Statistics */
static uint64_t scs_pushes = 0;
static uint64_t scs_pops   = 0;
static uint64_t scs_mismatches = 0;

/* ── Find or allocate a shadow stack ───────────────────────────────── */

static int scs_find_stack(uint64_t *stack_base)
{
    for (int i = 0; i < SCS_MAX_STACKS; i++) {
        if (g_scs_stacks[i].in_use && g_scs_stacks[i].base == stack_base)
            return i;
    }
    return -1;
}

/* ── Allocate a new shadow stack for a process ─────────────────────── */

int scs_alloc(struct process *proc)
{
    if (!scs_enabled) return 0;
    if (!proc) return -EINVAL;

    /* Check if already allocated */
    if (scs_find_stack(proc->kernel_stack) >= 0)
        return 0;

    if (g_scs_num_stacks >= SCS_MAX_STACKS)
        return -ENOMEM;

    int idx = g_scs_num_stacks;
    struct scs_shadow_stack *ss = &g_scs_stacks[idx];

    /* Allocate a page for the shadow stack */
    uint64_t scs_page = pmm_alloc_frame();
    if (!scs_page) return -ENOMEM;

    /* Map it into kernel space */
    uint64_t scs_virt = (uint64_t)phys_to_virt(scs_page);
    memset((void *)scs_virt, 0, 4096);

    ss->magic = SCS_MAGIC;
    ss->base  = (uint64_t *)scs_virt;
    ss->sp    = (uint64_t *)(scs_virt + SCS_DEFAULT_SIZE - 8);
    ss->size  = SCS_DEFAULT_SIZE;
    ss->in_use = 1;
    proc->scs_base = ss->base;
    proc->scs_sp   = ss->sp;

    g_scs_num_stacks++;
    return 0;
}

/* ── Free a shadow stack ───────────────────────────────────────────── */

void scs_free(struct process *proc)
{
    if (!proc || !proc->scs_base) return;

    int idx = scs_find_stack(proc->scs_base);
    if (idx < 0) return;

    struct scs_shadow_stack *ss = &g_scs_stacks[idx];
    uint64_t phys = virt_to_phys((uint64_t)ss->base);
    if (phys) pmm_free_frame(phys);

    memset(ss, 0, sizeof(*ss));
    proc->scs_base = NULL;
    proc->scs_sp = NULL;
}

/* ── Shadow stack push (call) ──────────────────────────────────────── */

void scs_push(uint64_t return_address)
{
    if (!scs_enabled) return;

    struct process *p = process_get_current();
    if (!p || !p->scs_sp) return;

    /* Push return address to shadow stack */
    *p->scs_sp = return_address;
    p->scs_sp = (uint64_t *)((uint8_t *)p->scs_sp - 8);
    scs_pushes++;
}

/* ── Shadow stack pop/verify (return) ──────────────────────────────── */

int scs_pop_verify(uint64_t expected_return)
{
    if (!scs_enabled) return 1; /* allow if disabled */

    struct process *p = process_get_current();
    if (!p || !p->scs_sp) return 1; /* no SCS → allow */

    /* Advance sp to the stored return address */
    p->scs_sp = (uint64_t *)((uint8_t *)p->scs_sp + 8);
    uint64_t stored = *p->scs_sp;
    scs_pops++;

    if (stored != expected_return) {
        scs_mismatches++;
        kprintf("[SCS] MISMATCH: stored=0x%016llx expected=0x%016llx pid=%d\n",
                (unsigned long long)stored,
                (unsigned long long)expected_return,
                p->pid);
        return 0; /* mismatch — ROP detected */
    }

    return 1; /* match */
}

/* ── Query / status ────────────────────────────────────────────────── */

int scs_get_enabled(void) { return scs_enabled; }
void scs_set_enabled(int val) { scs_enabled = val ? 1 : 0; }
uint64_t scs_get_pushes(void) { return scs_pushes; }
uint64_t scs_get_pops(void) { return scs_pops; }
uint64_t scs_get_mismatches(void) { return scs_mismatches; }
int scs_get_num_stacks(void) { return g_scs_num_stacks; }

/* ── Initialization ────────────────────────────────────────────────── */

void scs_init(void)
{
    memset(g_scs_stacks, 0, sizeof(g_scs_stacks));
    g_scs_num_stacks = 0;

    kprintf("[OK] SCS: Shadow Call Stack initialized (%d stacks max, %d bytes each)\n",
            SCS_MAX_STACKS, SCS_DEFAULT_SIZE);
}
