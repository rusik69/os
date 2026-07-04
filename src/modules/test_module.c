/* test_module.c — loadable kernel module for lifecycle testing (M39, D233)
 *
 * This module tracks its own lifecycle state and logs every state
 * transition.  It is the primary test module for verifying the
 * .ko build system, module loader, and refcounting infrastructure.
 *
 * State machine:
 *   UNINITIALIZED → LOADED → INIT_CALLED → ACTIVE → EXIT_CALLED → UNLOADED
 *
 * Usage:
 *   make                                        # build
 *   insmod /modules/test_module.ko             # load
 *   rmmod test_module                          # unload
 */

#include "module.h"
#include "printf.h"
#include "spinlock.h"
#include "string.h"

/* ── State tracking ─────────────────────────────────────────────── */

enum test_mod_state {
    TEST_MOD_UNINITIALIZED = 0,
    TEST_MOD_LOADED,       /* init_module called and succeeded */
    TEST_MOD_ACTIVE,       /* module is fully live */
    TEST_MOD_EXITING,      /* cleanup_module called */
    TEST_MOD_UNLOADED,     /* module fully removed */
};

static enum test_mod_state g_test_mod_state = TEST_MOD_UNINITIALIZED;
static int g_test_mod_init_count = 0;
static int g_test_mod_exit_count = 0;
static spinlock_t g_test_mod_lock;

/* Initialize the test module's internal state.
 * Must be called before the module is first loaded. */
void test_mod_reset(void)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_test_mod_lock, &irq_flags);
    g_test_mod_state = TEST_MOD_UNINITIALIZED;
    g_test_mod_init_count = 0;
    g_test_mod_exit_count = 0;
    spinlock_irqsave_release(&g_test_mod_lock, irq_flags);
}

/* Return the current test module state as a string. */
const char *test_mod_state_str(void)
{
    switch (g_test_mod_state) {
    case TEST_MOD_UNINITIALIZED: return "uninitialized";
    case TEST_MOD_LOADED:        return "loaded";
    case TEST_MOD_ACTIVE:        return "active";
    case TEST_MOD_EXITING:       return "exiting";
    case TEST_MOD_UNLOADED:      return "unloaded";
    default:                     return "unknown";
    }
}

/* Return the number of times init_module has been called. */
int test_mod_init_count(void)
{
    return g_test_mod_init_count;
}

/* Return the number of times cleanup_module has been called. */
int test_mod_exit_count(void)
{
    return g_test_mod_exit_count;
}

/* ── Module entry points ────────────────────────────────────────── */

int __init init_module(void)
{
    kprintf("[TEST_MOD] init_module called (state=%s)\n", test_mod_state_str());

    spinlock_init(&g_test_mod_lock);

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_test_mod_lock, &irq_flags);

    g_test_mod_init_count++;
    g_test_mod_state = TEST_MOD_LOADED;

    spinlock_irqsave_release(&g_test_mod_lock, irq_flags);

    kprintf("[TEST_MOD] init_module complete (count=%d, state=%s)\n",
            g_test_mod_init_count, test_mod_state_str());
    return 0;
}

void __exit cleanup_module(void)
{
    kprintf("[TEST_MOD] cleanup_module called (state=%s)\n", test_mod_state_str());

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_test_mod_lock, &irq_flags);

    g_test_mod_exit_count++;
    g_test_mod_state = TEST_MOD_EXITING;

    spinlock_irqsave_release(&g_test_mod_lock, irq_flags);

    kprintf("[TEST_MOD] cleanup_module complete (count=%d, state=%s)\n",
            g_test_mod_exit_count, test_mod_state_str());
}

/* ── Module metadata ──────────────────────────────────────────── */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS Kernel Team");
MODULE_DESCRIPTION("Loadable kernel module for lifecycle, refcount, and dependency testing (D233)");
MODULE_VERSION("1.0");

/* ── Legacy API stubs ──────────────────────────────────────────── */

/* test_module_init — called from built-in test harness */
int test_module_init(void)
{
    test_mod_reset();
    kprintf("[test_mod] Test module initialized (built-in)\n");
    return 0;
}

/* test_module_exit — called from built-in test harness */
int test_module_exit(void)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_test_mod_lock, &irq_flags);
    g_test_mod_state = TEST_MOD_UNLOADED;
    spinlock_irqsave_release(&g_test_mod_lock, irq_flags);
    kprintf("[test_mod] Test module exited (built-in, state=%s)\n",
            test_mod_state_str());
    return 0;
}

/* test_module_ioctl — stub for device operations */
int test_module_ioctl(void *file, int cmd, void *arg)
{
    (void)file;
    (void)cmd;
    (void)arg;
    return 0;
}
