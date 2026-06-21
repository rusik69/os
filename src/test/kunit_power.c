/*
 * kunit_power.c — KUnit test suites for power management subsystems
 *
 * Tests for: runtime PM state transitions (pm_runtime), RAPL MSR
 * domain enumeration and energy counter reading.
 *
 * Item S-plan: pm_runtime, rapl
 *
 * Run via:
 *   # echo 1 > /sys/kernel/debug/kunit/run_all
 *   # cat /sys/kernel/debug/kunit/results
 */

#include "kunit.h"
#include "pm_runtime.h"
#include "rapl.h"
#include "string.h"
#include "printf.h"
#include "heap.h"

/* ====================================================================
 *  1. PM_RUNTIME — device runtime PM state transitions
 * ==================================================================== */

/* Dummy device PM callbacks for testing */
static int dummy_resume_count = 0;
static int dummy_suspend_count = 0;
static int dummy_idle_count = 0;

static int dummy_runtime_resume(void *dev)
{
    (void)dev;
    dummy_resume_count++;
    return 0;
}

static int dummy_runtime_suspend(void *dev)
{
    (void)dev;
    dummy_suspend_count++;
    return 0;
}

static int dummy_runtime_idle(void *dev)
{
    (void)dev;
    dummy_idle_count++;
    return 0;
}

static struct dev_pm_ops dummy_pm_ops = {
    .runtime_suspend = dummy_runtime_suspend,
    .runtime_resume  = dummy_runtime_resume,
    .runtime_idle    = dummy_runtime_idle,
};

/* Initialize PM runtime, register a device, get/put sync. */
static void pm_runtime_register_get_put(struct kunit *test)
{
    struct pm_runtime_device dev;
    memset(&dev, 0, sizeof(dev));

    /* Must init first */
    pm_runtime_init();

    dev.name = "kunit_test_dev1";
    dev.dev  = NULL;
    dev.ops  = &dummy_pm_ops;

    int ret = pm_runtime_register(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* After register, usage_count should be 1 */
    KUNIT_EXPECT_EQ(test, (int64_t)dev.usage_count, (int64_t)1);
    KUNIT_EXPECT_EQ(test, (int64_t)dev.suspended, (int64_t)0);

    /* Get sync (increment usage) */
    ret = pm_runtime_get_sync(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)dev.usage_count, (int64_t)2);

    /* Put sync (decrement usage) */
    ret = pm_runtime_put_sync(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)dev.usage_count, (int64_t)1);

    /* Put again (usage goes to 0, triggers suspend) */
    ret = pm_runtime_put_sync(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)dev.usage_count, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)dev.suspended, (int64_t)1);

    /* Get again (should resume) */
    ret = pm_runtime_get_sync(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)dev.usage_count, (int64_t)1);
    KUNIT_EXPECT_EQ(test, (int64_t)dev.suspended, (int64_t)0);

    pm_runtime_unregister(&dev);
}

/* Test immediate suspend/resume API. */
static void pm_runtime_immediate_suspend_resume(struct kunit *test)
{
    struct pm_runtime_device dev;
    memset(&dev, 0, sizeof(dev));

    pm_runtime_init();

    dev.name = "kunit_imm_dev";
    dev.dev  = NULL;
    dev.ops  = &dummy_pm_ops;

    int ret = pm_runtime_register(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* Immediately suspend */
    ret = pm_runtime_suspend_immediately(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)dev.suspended, (int64_t)1);

    /* Immediately resume */
    ret = pm_runtime_resume_immediately(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)dev.suspended, (int64_t)0);

    /* Suspend again */
    ret = pm_runtime_suspend_immediately(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)dev.suspended, (int64_t)1);

    pm_runtime_unregister(&dev);
}

/* Test autosuspend delay setting. */
static void pm_runtime_autosuspend_delay(struct kunit *test)
{
    struct pm_runtime_device dev;
    memset(&dev, 0, sizeof(dev));

    pm_runtime_init();

    dev.name = "kunit_delay_dev";
    dev.dev  = NULL;
    dev.ops  = &dummy_pm_ops;

    int ret = pm_runtime_register(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* Default delay should be 0 */
    KUNIT_EXPECT_EQ(test, (int64_t)dev.suspend_after_ms, (int64_t)0);

    /* Set a delay */
    pm_runtime_set_autosuspend_delay(&dev, 500);
    KUNIT_EXPECT_EQ(test, (int64_t)dev.suspend_after_ms, (int64_t)500);

    pm_runtime_unregister(&dev);
}

/* Unregister a device, then operations on it should fail. */
static void pm_runtime_unregister_then_ops_fail(struct kunit *test)
{
    struct pm_runtime_device dev;
    memset(&dev, 0, sizeof(dev));

    pm_runtime_init();

    dev.name = "kunit_unreg_dev";
    dev.dev  = NULL;
    dev.ops  = &dummy_pm_ops;

    int ret = pm_runtime_register(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    ret = pm_runtime_unregister(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* After unregister, get/put should fail */
    ret = pm_runtime_get_sync(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-1);
}

/* Test that get/put on an unregistered device fails. */
static void pm_runtime_null_ops(struct kunit *test)
{
    int ret;

    /* Call with NULL should fail */
    ret = pm_runtime_get_sync(NULL);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-1);

    ret = pm_runtime_put_sync(NULL);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-1);

    ret = pm_runtime_register(NULL);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-1);

    ret = pm_runtime_unregister(NULL);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-1);
}

/* Verify that suspend/resume callbacks are invoked correctly. */
static void pm_runtime_callback_counts(struct kunit *test)
{
    /* Reset counters */
    dummy_resume_count = 0;
    dummy_suspend_count = 0;
    dummy_idle_count = 0;

    struct pm_runtime_device dev;
    memset(&dev, 0, sizeof(dev));

    pm_runtime_init();

    dev.name = "kunit_cb_dev";
    dev.dev  = NULL;
    dev.ops  = &dummy_pm_ops;

    int ret = pm_runtime_register(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* Immediately suspend */
    ret = pm_runtime_suspend_immediately(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)dummy_suspend_count, (int64_t)1);

    /* Immediately resume */
    ret = pm_runtime_resume_immediately(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)dummy_resume_count, (int64_t)1);

    /* Put to 0 triggers idle callback, then suspend */
    /* Usage count is 1 from register, put to 0 */
    pm_runtime_put_sync(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)dummy_idle_count, (int64_t)1);
    KUNIT_EXPECT_EQ(test, (int64_t)dummy_suspend_count, (int64_t)2);

    /* Get resumes */
    pm_runtime_get_sync(&dev);
    KUNIT_EXPECT_EQ(test, (int64_t)dummy_resume_count, (int64_t)2);

    pm_runtime_unregister(&dev);
}

/* Register multiple devices. */
static void pm_runtime_multiple_devices(struct kunit *test)
{
    struct pm_runtime_device dev1, dev2;
    memset(&dev1, 0, sizeof(dev1));
    memset(&dev2, 0, sizeof(dev2));

    pm_runtime_init();

    dev1.name = "kunit_multi_1";
    dev1.dev  = NULL;
    dev1.ops  = &dummy_pm_ops;

    dev2.name = "kunit_multi_2";
    dev2.dev  = NULL;
    dev2.ops  = &dummy_pm_ops;

    int r1 = pm_runtime_register(&dev1);
    int r2 = pm_runtime_register(&dev2);
    KUNIT_EXPECT_EQ(test, (int64_t)r1, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)r2, (int64_t)0);

    KUNIT_EXPECT_NE(test, (int64_t)&dev1, (int64_t)&dev2);

    pm_runtime_unregister(&dev1);
    pm_runtime_unregister(&dev2);
}

/* ====================================================================
 *  2. RAPL — MSR domain enumeration and energy counter reading
 * ==================================================================== */

/* Initialize RAPL subsystem. */
static void rapl_init_test(struct kunit *test)
{
    /* RAPL may not be available on non-Intel hardware or in emulation.
     * Accept either success (0) or failure (-1). */
    int ret = rapl_init();

    /* If init succeeds, subsequent calls should see it as initialized */
    if (ret == 0) {
        /* Second init should also succeed (idempotent) */
        ret = rapl_init();
        KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    }

    /* Just verify no crash occurred */
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -1);
}

/* Read RAPL unit information. */
static void rapl_get_units_test(struct kunit *test)
{
    int ret = rapl_init();
    if (ret < 0) {
        /* RAPL not available — skip unit checks */
        KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-1);
        return;
    }

    struct rapl_units units;
    memset(&units, 0xFF, sizeof(units));

    ret = rapl_get_units(&units);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* Unit values should be positive finite doubles */
    KUNIT_EXPECT_TRUE(test, units.energy > 0.0);
    KUNIT_EXPECT_TRUE(test, units.power > 0.0);
    KUNIT_EXPECT_TRUE(test, units.time > 0.0);

    /* Units should be reasonable: energy ~1e-6 to 1e-3 J */
    KUNIT_EXPECT_TRUE(test, units.energy <= 1.0);
    KUNIT_EXPECT_TRUE(test, units.power <= 1.0);
    KUNIT_EXPECT_TRUE(test, units.time <= 1.0);
}

/* Read PKG domain energy counter (if available). */
static void rapl_read_energy_pkg_test(struct kunit *test)
{
    int ret = rapl_init();
    if (ret < 0)
        return;

    struct rapl_domain_energy energy;
    memset(&energy, 0, sizeof(energy));

    ret = rapl_read_energy_pkg(&energy);
    /* May return -1 if the domain is not available on this platform */
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -1);

    if (ret == 0) {
        KUNIT_EXPECT_TRUE(test, energy.energy_joules >= 0.0);
    }
}

/* Read DRAM domain energy counter (if available). */
static void rapl_read_energy_dram_test(struct kunit *test)
{
    int ret = rapl_init();
    if (ret < 0)
        return;

    struct rapl_domain_energy energy;
    memset(&energy, 0, sizeof(energy));

    ret = rapl_read_energy_dram(&energy);
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -1);

    if (ret == 0) {
        KUNIT_EXPECT_TRUE(test, energy.energy_joules >= 0.0);
    }
}

/* Read PP0 (core) domain energy counter (if available). */
static void rapl_read_energy_pp0_test(struct kunit *test)
{
    int ret = rapl_init();
    if (ret < 0)
        return;

    struct rapl_domain_energy energy;
    memset(&energy, 0, sizeof(energy));

    ret = rapl_read_energy_pp0(&energy);
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -1);

    if (ret == 0) {
        KUNIT_EXPECT_TRUE(test, energy.energy_joules >= 0.0);
    }
}

/* RAPL API handles NULL output parameter gracefully. */
static void rapl_null_output(struct kunit *test)
{
    int ret = rapl_init();
    if (ret < 0)
        return;

    /* Passing NULL should return -1 */
    ret = rapl_read_energy_pkg(NULL);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-1);

    ret = rapl_read_energy_dram(NULL);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-1);

    ret = rapl_read_energy_pp0(NULL);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-1);

    ret = rapl_get_units(NULL);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-1);
}

/* ====================================================================
 *  Suite definitions
 * ==================================================================== */

/* ── pm_runtime suite ───────────────────────────────────────────── */
static struct kunit_case pm_runtime_test_cases[] = {
    KUNIT_CASE(pm_runtime_register_get_put),
    KUNIT_CASE(pm_runtime_immediate_suspend_resume),
    KUNIT_CASE(pm_runtime_autosuspend_delay),
    KUNIT_CASE(pm_runtime_unregister_then_ops_fail),
    KUNIT_CASE(pm_runtime_null_ops),
    KUNIT_CASE(pm_runtime_callback_counts),
    KUNIT_CASE(pm_runtime_multiple_devices),
    {0}
};

static struct kunit_suite pm_runtime_test_suite;

/* ── RAPL suite ─────────────────────────────────────────────────── */
static struct kunit_case rapl_test_cases[] = {
    KUNIT_CASE(rapl_init_test),
    KUNIT_CASE(rapl_get_units_test),
    KUNIT_CASE(rapl_read_energy_pkg_test),
    KUNIT_CASE(rapl_read_energy_dram_test),
    KUNIT_CASE(rapl_read_energy_pp0_test),
    KUNIT_CASE(rapl_null_output),
    {0}
};

static struct kunit_suite rapl_test_suite;

/* ====================================================================
 *  Registration
 * ==================================================================== */

#define REGISTER_SUITE(suite_var, cases_arr, suite_name) do {          \
    int __ci = 0;                                                       \
    for (int __i = 0; cases_arr[__i].run != NULL && __i < KUNIT_MAX_CASES - 1; __i++) { \
        suite_var.cases[__ci].name = cases_arr[__i].name;               \
        suite_var.cases[__ci].run  = cases_arr[__i].run;                \
        __ci++;                                                         \
    }                                                                   \
    suite_var.cases[__ci].name = NULL;                                  \
    suite_var.cases[__ci].run  = NULL;                                  \
    suite_var.name     = suite_name;                                    \
    suite_var.setup    = NULL;                                          \
    suite_var.teardown = NULL;                                          \
    kunit_register_suite(&suite_var);                                   \
    kprintf("[KUnit] %s tests registered (%d cases)\\n", suite_name, __ci); \
} while(0)

void kunit_power_register(void)
{
    REGISTER_SUITE(pm_runtime_test_suite,  pm_runtime_test_cases,  "pm_runtime");
    REGISTER_SUITE(rapl_test_suite,        rapl_test_cases,        "rapl");
}

/* ── Stub: kunit_power_init ─────────────────────────────── */
int kunit_power_init(void)
{
    kprintf("[kunit] kunit_power_init: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: kunit_power_test_cpufreq ─────────────────────────────── */
int kunit_power_test_cpufreq(void)
{
    kprintf("[kunit] kunit_power_test_cpufreq: not yet implemented\n");
    return -ENOSYS;
}
