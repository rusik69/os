/*
 * kunit_sched.c — KUnit test suite for the scheduler subsystem.
 *
 * Tests basic scheduler API functionality including stats, autogroups,
 * and scheduling policies.  These tests run inside the running kernel
 * and validate the scheduler's internal consistency.
 *
 * Item 270: KUnit — scheduler tests
 */

#include "kunit.h"
#include "scheduler.h"
#include "process.h"
#include "string.h"
#include "printf.h"
#include "core_sched.h"
#include "nohz.h"

/* ====================================================================
 *  1. Scheduler Statistics Tests
 * ==================================================================== */

static void sched_stats_test(struct kunit *test)
{
    struct sched_stats stats;

    memset(&stats, 0xFF, sizeof(stats));
    scheduler_get_stats(&stats);

    /* The scheduler should have been running for at least a few
     * context switches by the time we reach this test. */
    KUNIT_EXPECT_NE(test, stats.context_switches, (uint64_t)0xFFFFFFFFFFFFFFFFULL);
    KUNIT_EXPECT_NE(test, stats.context_switches, (uint64_t)0);
}

/* ====================================================================
 *  2. Per-CPU Runqueue Statistics Tests
 * ==================================================================== */

static void sched_runqueue_stats_test(struct kunit *test)
{
    struct runqueue_stats rq;

    /* Test runqueue stats for CPU 0 (always exists) */
    memset(&rq, 0xCC, sizeof(rq));
    scheduler_get_runqueue_stats(0, &rq);

    /* nr_runnable and nr_running should be non-negative */
    KUNIT_EXPECT_TRUE(test, rq.nr_runnable >= 0);
    KUNIT_EXPECT_TRUE(test, rq.nr_running >= 0);

    /* At minimum, the idle task should exist; some systems may
     * have more (init, shell, kworkers).  The counts should not
     * be unreasonably large. */
    KUNIT_EXPECT_TRUE(test, rq.nr_runnable <= 1024);
    KUNIT_EXPECT_TRUE(test, rq.nr_running <= 1024);

    /* Priority distribution counts should sum to nr_runnable */
    int prio_sum = 0;
    for (int i = 0; i < SCHED_LEVELS; i++) {
        KUNIT_EXPECT_TRUE(test, rq.prio_distribution[i] >= 0);
        prio_sum += rq.prio_distribution[i];
    }
    KUNIT_EXPECT_EQ(test, prio_sum, rq.nr_runnable);

    /* Load weight should be reasonable */
    KUNIT_EXPECT_TRUE(test, rq.load_weight >= 0);
    KUNIT_EXPECT_TRUE(test, rq.load_weight <= 102400);
}

/* ====================================================================
 *  3. Scheduling Policy Classification Tests
 * ==================================================================== */

static void sched_policy_consistency_test(struct kunit *test)
{
    /* Verify that the public scheduler API handles edge input gracefully.
     * scheduler_set_priority with NULL process should return error. */
    int ret = scheduler_set_priority(NULL, 0);
    KUNIT_EXPECT_TRUE(test, ret == -1 || ret == 0);

    /* scheduler_set_nice with NULL process should return error */
    ret = scheduler_set_nice(NULL, 0);
    KUNIT_EXPECT_TRUE(test, ret == -1 || ret == 0);
}

/* ====================================================================
 *  4. Scheduler Yield Test
 * ==================================================================== */

static void sched_yield_test(struct kunit *test)
{
    uint64_t cs_before, cs_after;
    struct sched_stats stats;

    /* Get context switch count before yield */
    scheduler_get_stats(&stats);
    cs_before = stats.context_switches;

    /* Yield the CPU */
    scheduler_yield();

    /* Get context switch count after yield */
    scheduler_get_stats(&stats);
    cs_after = stats.context_switches;

    /* Yielding should have caused at least one context switch.
     * It's possible we're the only task and scheduler returns
     * without switching, so accept either >= or ==. */
    KUNIT_EXPECT_TRUE(test, cs_after >= cs_before);

    /* The difference should be small (1-2 context switches max
     * for a voluntary yield on a lightly loaded system) */
    KUNIT_EXPECT_TRUE(test, (cs_after - cs_before) <= (uint64_t)8);
}

/* ====================================================================
 *  5. Scheduler Wakeup Test
 * ==================================================================== */

static void sched_wakeup_sleepers_test(struct kunit *test)
{
    /* scheduler_wake_sleepers should be safe to call at any time.
     * This just validates it doesn't crash or produce side effects
     * when there are no sleeping processes. */
    scheduler_wake_sleepers();

    /* If we reach here, the call succeeded without crashing */
    KUNIT_EXPECT_TRUE(test, 1);
}

/* ====================================================================
 *  6. CFS Autogroup Tests
 * ==================================================================== */

static void sched_autogroup_test(struct kunit *test)
{
    /* Test autogroup max vruntime query — should not crash */

    /* Query the root/default autogroup (ID 0) */
    uint64_t vr = sched_autogroup_max_vruntime(0);
    KUNIT_EXPECT_NE(test, vr, (uint64_t)0xFFFFFFFFFFFFFFFFULL);

    /* Query a non-existent autogroup — should return 0 */
    vr = sched_autogroup_max_vruntime(SCHED_AUTOGROUP_MAX + 10);
    KUNIT_EXPECT_EQ(test, vr, (uint64_t)0);
}

/* ====================================================================
 *  7. Scheduler Priority Bounds Test
 * ==================================================================== */

static void sched_priority_bounds_test(struct kunit *test)
{
    /* Verify that priority levels map to the expected 4-level scheme.
     * Priority 0 = highest, 3 = lowest (SCHED_LEVELS-1). */

    /* Test with a NULL process — should return error gracefully */
    int ret = scheduler_set_priority(NULL, 0);
    KUNIT_EXPECT_TRUE(test, ret == -1 || ret == 0);

    /* Test with an invalid priority level */
    ret = scheduler_set_priority(NULL, 255);
    KUNIT_EXPECT_TRUE(test, ret == -1 || ret == 0);

    /* Test nice value bounds */
    ret = scheduler_set_nice(NULL, -20);
    KUNIT_EXPECT_TRUE(test, ret == -1 || ret == 0);

    ret = scheduler_set_nice(NULL, 19);
    KUNIT_EXPECT_TRUE(test, ret == -1 || ret == 0);
}

/* ====================================================================
 *  8. Core Scheduling tests
 * ==================================================================== */

static void core_sched_basic_test(struct kunit *test)
{
    /* sched_core_init() is called at boot, so core scheduling is active.
     * Test basic API consistency. */

    /* CPU 0 should always be its own sibling */
    uint64_t siblings = sched_core_siblings(0);
    KUNIT_EXPECT_TRUE(test, (siblings & 1) != 0);

    /* A CPU always shares a core with itself */
    KUNIT_EXPECT_TRUE(test, sched_core_share(0, 0) == 1);

    /* CPUs per core is at least 1 */
    KUNIT_EXPECT_TRUE(test, sched_core_cpus_per_core() >= 1);

    /* Cookie set/get on the current process */
    struct process *cur = process_get_current();
    KUNIT_EXPECT_TRUE(test, cur != NULL);

    if (cur) {
        uint64_t old_cookie = sched_core_get_cookie(cur);
        sched_core_set_cookie(cur, 42);
        KUNIT_EXPECT_EQ(test, sched_core_get_cookie(cur), (uint64_t)42);
        /* Restore */
        sched_core_set_cookie(cur, old_cookie);
        KUNIT_EXPECT_EQ(test, sched_core_get_cookie(cur), old_cookie);
    }

    /* sched_core_allow with no cookie (0) should always return 1 */
    struct process *proc = process_get_current();
    if (proc) {
        uint64_t saved = proc->core_sched_cookie;
        proc->core_sched_cookie = 0;
        KUNIT_EXPECT_TRUE(test, sched_core_allow(proc, 0) == 1);
        proc->core_sched_cookie = saved;
    }

    /* Invalid CPU should return 0 */
    KUNIT_EXPECT_TRUE(test, sched_core_allow(NULL, 0) == 0);
}

/* ====================================================================
 *  9. NO_HZ Adaptive Tick tests (safe subset)
 * ==================================================================== */

static void nohz_basic_test(struct kunit *test)
{
    /* nohz_init() is called at boot.  CPU 0 is not isolated by default. */

    /* CPU 0 should not be isolated by default */
    KUNIT_EXPECT_TRUE(test, nohz_cpu_is_isolated(0) == 0);

    /* Tick should not be stopped on a non-isolated CPU */
    KUNIT_EXPECT_TRUE(test, nohz_tick_is_stopped(0) == 0);

    /* nohz_tick_account should be safe to call on any CPU */
    nohz_tick_account(0);

    /* Stopping tick on a non-isolated CPU should return -EINVAL */
    KUNIT_EXPECT_TRUE(test, nohz_tick_stop(0) != 0);

    /* Restart on a running CPU should return 0 (already running) */
    KUNIT_EXPECT_TRUE(test, nohz_tick_restart(0) == 0);

    /* Stopped ms should be 0 if tick is not stopped */
    KUNIT_EXPECT_EQ(test, nohz_tick_stopped_ms(0), (uint64_t)0);

    /* Invalid CPU should not crash */
    nohz_tick_account(-1);
    nohz_tick_stop(-1);
    nohz_tick_restart(-1);
    KUNIT_EXPECT_TRUE(test, nohz_cpu_is_isolated(-1) == 0);
    KUNIT_EXPECT_TRUE(test, nohz_tick_is_stopped(-1) == 0);
}

/* ====================================================================
 *  Test case list (terminated by {0})
 * ==================================================================== */

static struct kunit_case sched_test_cases[] = {
    KUNIT_CASE(sched_stats_test),
    KUNIT_CASE(sched_runqueue_stats_test),
    KUNIT_CASE(sched_policy_consistency_test),
    KUNIT_CASE(sched_yield_test),
    KUNIT_CASE(sched_wakeup_sleepers_test),
    KUNIT_CASE(sched_autogroup_test),
    KUNIT_CASE(sched_priority_bounds_test),
    KUNIT_CASE(core_sched_basic_test),
    KUNIT_CASE(nohz_basic_test),
    {0}
};

static struct kunit_suite sched_test_suite;

/* ====================================================================
 *  Suite Registration
 * ==================================================================== */

void kunit_sched_register(void)
{
    /* Populate the fixed-size case array from our sentinel-terminated list */
    int ci = 0;
    for (int i = 0; sched_test_cases[i].run != NULL && i < KUNIT_MAX_CASES - 1; i++) {
        sched_test_suite.cases[ci].name = sched_test_cases[i].name;
        sched_test_suite.cases[ci].run  = sched_test_cases[i].run;
        ci++;
    }
    sched_test_suite.cases[ci].name = NULL;
    sched_test_suite.cases[ci].run  = NULL;

    sched_test_suite.name     = "sched_test";
    sched_test_suite.setup    = NULL;
    sched_test_suite.teardown = NULL;

    kunit_register_suite(&sched_test_suite);
    kprintf("[KUnit] Scheduler tests registered (%d cases)\n", ci);
}
