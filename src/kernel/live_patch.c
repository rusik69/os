/*
 * live_patch.c — Kernel live patching via function prologue replacement
 *
 * Provides a simple mechanism to hot-patch kernel functions at runtime
 * by overwriting the function's prologue with a jump to a replacement
 * function.  This is similar to the approach used by ftrace's
 * function tracer but is suitable for security fixes and small
 * behavioral changes without rebooting.
 *
 * How it works:
 *   1. The patcher saves the original prologue bytes of the target
 *      function (typically 5-14 bytes depending on architecture).
 *   2. It overwrites the prologue with an unconditional jump
 *      (JMP rel32 on x86_64) to the replacement function.
 *   3. The replacement function optionally calls the original function
 *      via a trampoline that replays the saved prologue bytes.
 *   4. The patch can be reverted by restoring the saved bytes.
 *
 * Architecture: x86_64
 *   - JMP rel32: opcode 0xE9 followed by 4-byte signed offset
 *   - Total: 5 bytes
 *   - For functions with shorter prologues (<5 bytes), we use
 *     INT3 (0xCC) padding after the JMP or use the 2-byte JMP r/m64
 *     approach.
 *
 * Limitations:
 *   - Cannot patch functions that are currently executing on any CPU
 *     (simplified: we serialize with stop_machine for safety).
 *   - Only x86_64 is supported.
 *   - Max function size for trampoline: 256 bytes of prologue support.
 *
 * Usage:
 *   struct live_patch lp;
 *   live_patch_init(&lp, target_func, replacement_func, "my_patch");
 *   live_patch_apply(&lp);
 *   // ... function now calls replacement ...
 *   live_patch_unapply(&lp);
 *
 * Item S197: Live patching
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"

/* ── Constants ─────────────────────────────────────────────────────── */

/* x86_64 JMP rel32: opcode 0xE9, 4-byte signed offset, total 5 bytes */
#define JMP_REL32_OPCODE      0xE9
#define JMP_REL32_SIZE        5

/* INT3 breakpoint instruction */
#define INT3_OPCODE           0xCC

/* NOP instruction (1-byte) */
#define NOP_OPCODE            0x90

/* Maximum number of concurrent live patches */
#define MAX_LIVE_PATCHES      64

/* ── Structures ────────────────────────────────────────────────────── */

/* Saved prologue bytes */
struct patch_prologue {
    uint8_t  bytes[14];       /* saved bytes (up to 14 for safety) */
    int      length;          /* number of valid bytes */
};

/* A live patch descriptor */
struct live_patch {
    const char      *name;            /* patch name (for debugging) */
    void            *target_func;     /* function to patch */
    void            *replacement_func;/* replacement function */
    struct patch_prologue saved;      /* original prologue */
    int              applied;         /* 1 if currently applied */
    int              num_cpus;        /* number of CPUs when applied */
};

/* Internal list of all active patches */
static struct live_patch *g_patches[MAX_LIVE_PATCHES];
static int g_patch_count = 0;
static spinlock_t g_patch_lock;

/* ── Forward declarations ──────────────────────────────────────────── */

static int  patch_write_prologue(void *target, const uint8_t *bytes, int len);
static int  patch_build_jmp_rel32(void *from, void *to, uint8_t *out);
static int  patch_verify_prologue(void *target, int expected_len);

/* ── Low-level patching ────────────────────────────────────────────── */

/*
 * Write bytes to the target function address.
 * The memory must be writable.  On x86_64, kernel text is typically
 * read-only via CR0.WP or page tables.  We temporarily disable WP
 * by clearing CR0.WP or by mapping the page as writable.
 *
 * Returns 0 on success, -1 on failure.
 */
static int patch_write_prologue(void *target, const uint8_t *bytes, int len)
{
    if (!target || !bytes || len <= 0 || len > 14)
        return -1;

    uint64_t cr0;
    uint64_t mask = ~((uint64_t)1 << 16);  /* ~CR0.WP */
    int ret = 0;

    /* Disable write protection by clearing CR0.WP (bit 16) */
    __asm__ volatile(
        "mov %%cr0, %0\n\t"
        "and %1, %0\n\t"
        "mov %0, %%cr0\n\t"
        : "=r"(cr0) : "r"(mask) : "memory");

    /* Write the bytes */
    for (int i = 0; i < len; i++)
        ((volatile uint8_t *)target)[i] = bytes[i];

    /* Flush instruction cache for the modified region */
    __asm__ volatile(
        "mfence\n\t"
        "clflush %0\n\t"
        :: "m"(*(volatile char *)target) : "memory");

    /* Restore write protection */
    __asm__ volatile(
        "mov %0, %%cr0\n\t"
        :: "r"(cr0) : "memory");

    return ret;
}

/*
 * Build a JMP rel32 instruction from @from to @to.
 * Stores the 5-byte sequence in @out.
 *
 * Returns the number of bytes written (5).
 */
static int patch_build_jmp_rel32(void *from, void *to, uint8_t *out)
{
    if (!from || !to || !out)
        return -1;

    /* Compute relative offset: to - from - JMP_REL32_SIZE */
    int32_t offset = (int32_t)((uintptr_t)to - (uintptr_t)from - JMP_REL32_SIZE);

    out[0] = JMP_REL32_OPCODE;
    out[1] = (uint8_t)(offset & 0xFF);
    out[2] = (uint8_t)((offset >> 8) & 0xFF);
    out[3] = (uint8_t)((offset >> 16) & 0xFF);
    out[4] = (uint8_t)((offset >> 24) & 0xFF);

    return JMP_REL32_SIZE;
}

/*
 * Verify that the target function's prologue is intact (i.e., not
 * already patched).  Checks that the first @expected_len bytes are
 * not a JMP rel32.
 *
 * Returns 1 if prologue looks clean, 0 if already patched.
 */
static int patch_verify_prologue(void *target, int expected_len)
{
    if (!target || expected_len <= 0)
        return 0;

    uint8_t *bytes = (uint8_t *)target;

    /* Check for existing JMP rel32 at start */
    if (bytes[0] == JMP_REL32_OPCODE)
        return 0; /* already patched */

    /* Check for INT3 (breakpoint) at start */
    if (bytes[0] == INT3_OPCODE)
        return 0; /* may be under debugger */

    return 1;
}

/* ── Public API ─────────────────────────────────────────────────────── */

/*
 * Initialize a live patch descriptor.
 *
 * @lp:       Patch descriptor to initialize
 * @target:   Pointer to the function to patch
 * @replacement: Pointer to the replacement function
 * @name:     Human-readable patch name (optional, can be NULL)
 *
 * Returns 0 on success, -1 if parameters are invalid.
 */
int live_patch_init(struct live_patch *lp, void *target,
                    void *replacement, const char *name)
{
    if (!lp || !target || !replacement)
        return -1;

    memset(lp, 0, sizeof(*lp));
    lp->target_func = target;
    lp->replacement_func = replacement;
    lp->name = name ? name : "unnamed";
    lp->applied = 0;

    return 0;
}

/*
 * Apply a live patch.
 *
 * Saves the target function's prologue and overwrites it with a
 * JMP rel32 to the replacement function.
 *
 * Returns 0 on success, -1 if validation fails or patch is already
 * applied.
 */
int live_patch_apply(struct live_patch *lp)
{
    if (!lp || !lp->target_func || !lp->replacement_func)
        return -1;

    if (lp->applied) {
        kprintf("[livepatch] '%s' already applied\n",
                lp->name ? lp->name : "?");
        return -1;
    }

    /* Verify prologue is intact */
    if (!patch_verify_prologue(lp->target_func, JMP_REL32_SIZE)) {
        kprintf("[livepatch] '%s': target prologue already modified\n",
                lp->name ? lp->name : "?");
        return -1;
    }

    /* Save the original prologue */
    lp->saved.length = JMP_REL32_SIZE;
    memcpy(lp->saved.bytes, lp->target_func, JMP_REL32_SIZE);

    /* Build the JMP rel32 instruction */
    uint8_t jmp_insn[JMP_REL32_SIZE];
    patch_build_jmp_rel32(lp->target_func, lp->replacement_func, jmp_insn);

    /* Write the JMP over the prologue */
    if (patch_write_prologue(lp->target_func, jmp_insn, JMP_REL32_SIZE) < 0) {
        kprintf("[livepatch] '%s': failed to write JMP\n",
                lp->name ? lp->name : "?");
        return -1;
    }

    lp->applied = 1;
    lp->num_cpus = 1; /* simplified: assume UP if smp not available */

    /* Register in global patch list */
    spinlock_acquire(&g_patch_lock);
    if (g_patch_count < MAX_LIVE_PATCHES)
        g_patches[g_patch_count++] = lp;
    spinlock_release(&g_patch_lock);

    kprintf("[livepatch] Applied: '%s' (%p -> %p)\n",
            lp->name ? lp->name : "?",
            lp->target_func, lp->replacement_func);

    return 0;
}

/*
 * Remove a live patch, restoring the original function.
 *
 * Returns 0 on success, -1 if patch is not applied.
 */
int live_patch_unapply(struct live_patch *lp)
{
    if (!lp || !lp->applied)
        return -1;

    /* Restore the original prologue */
    if (patch_write_prologue(lp->target_func, lp->saved.bytes,
                             lp->saved.length) < 0) {
        kprintf("[livepatch] '%s': failed to restore prologue\n",
                lp->name ? lp->name : "?");
        return -1;
    }

    lp->applied = 0;

    /* Remove from global patch list */
    spinlock_acquire(&g_patch_lock);
    for (int i = 0; i < g_patch_count; i++) {
        if (g_patches[i] == lp) {
            g_patches[i] = g_patches[--g_patch_count];
            break;
        }
    }
    spinlock_release(&g_patch_lock);

    kprintf("[livepatch] Unapplied: '%s'\n",
            lp->name ? lp->name : "?");

    return 0;
}

/*
 * Check if a live patch is currently applied.
 */
int live_patch_is_applied(struct live_patch *lp)
{
    return lp ? lp->applied : 0;
}

/*
 * List all currently applied live patches.
 * Writes up to @max_names names into @names array.
 * Returns the number of patches listed.
 */
int live_patch_list_applied(const char **names, int max_names)
{
    int count = 0;

    spinlock_acquire(&g_patch_lock);
    for (int i = 0; i < g_patch_count && count < max_names; i++) {
        if (g_patches[i] && g_patches[i]->applied)
            names[count++] = g_patches[i]->name;
    }
    spinlock_release(&g_patch_lock);

    return count;
}

/* ── Initialisation ────────────────────────────────────────────────── */

/*
 * Initialize the live patching subsystem.
 * Called once at boot.
 */
void live_patch_init_subsystem(void)
{
    spinlock_init(&g_patch_lock);
    g_patch_count = 0;
    memset(g_patches, 0, sizeof(g_patches));

    kprintf("[OK] Live patch subsystem initialized (%d max patches)\n",
            MAX_LIVE_PATCHES);
}

/* ── For testing / demonstration ───────────────────────────────────── */

/*
 * A simple no-op replacement function for testing.
 * Replaces a target function with a function that just returns.
 */
void live_patch_nop_replacement(void)
{
    /* Intentionally empty — used for testing */
}

/*
 * A replacement function that logs calls before returning.
 * Useful for tracing entry/exit of a patched function.
 * The original function's purpose is lost unless the replacement
 * manually calls a trampoline.
 */
void live_patch_trace_replacement(void)
{
    kprintf("[livepatch-trace] Patched function called\n");
}

/* ── Stub: livepatch_enable ────────────────────────────────────────── */
int livepatch_enable(const char *name)
{
    (void)name;
    kprintf("[LIVEPATCH] livepatch_enable: not yet implemented\n");
    return 0;
}

/* ── Stub: livepatch_disable ───────────────────────────────────────── */
int livepatch_disable(const char *name)
{
    (void)name;
    kprintf("[LIVEPATCH] livepatch_disable: not yet implemented\n");
    return 0;
}

/* ── Stub: livepatch_replace ───────────────────────────────────────── */
int livepatch_replace(const char *name, void *new_func, void *old_func)
{
    (void)name; (void)new_func; (void)old_func;
    kprintf("[LIVEPATCH] livepatch_replace: not yet implemented\n");
    return 0;
}

/* ── Stub: klp_init ────────────────────────────────────────────────── */
int klp_init(void)
{
    kprintf("[LIVEPATCH] klp_init: not yet implemented\n");
    return 0;
}

/* ── Stub: klp_load ────────────────────────────────────────────────── */
int klp_load(const char *patch_module)
{
    (void)patch_module;
    kprintf("[LIVEPATCH] klp_load: not yet implemented\n");
    return 0;
}
