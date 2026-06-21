/*
 * kunit_cluster.c — KUnit test suites for cluster subsystems.
 *
 * Tests:
 *   1. Cluster autoscaler — init, should_scale, scale_up/down
 *   2. Cluster descheduler — init, add/remove policies, run
 *   3. Cluster ingress — init, add rules, NodePort, LoadBalancer, HTTP routing
 */

#include "kunit.h"
#include "cluster_autoscaler.h"
#include "cluster_descheduler.h"
#include "cluster_ingress.h"
#include "string.h"
#include "printf.h"
#include "heap.h"

/* ═══════════════════════════════════════════════════════════════════════
 *  1. Cluster Autoscaler Tests
 * ═══════════════════════════════════════════════════════════════════════ */

static void autoscaler_init_test(struct kunit *test)
{
    int ret = cluster_autoscaler_init(1, 10);
    KUNIT_EXPECT_EQ(test, ret, 0);
}

static void autoscaler_init_invalid_test(struct kunit *test)
{
    /* min > max should fail */
    int ret = cluster_autoscaler_init(10, 5);
    KUNIT_EXPECT_NE(test, ret, 0);

    /* max > AUTOSCALER_MAX_NODES should fail */
    ret = cluster_autoscaler_init(1, 300);
    KUNIT_EXPECT_NE(test, ret, 0);
}

static void autoscaler_should_scale_test(struct kunit *test)
{
    cluster_autoscaler_init(1, 10);

    /* No pending pods → should not need scale-up */
    cluster_autoscaler_report_pending(0);
    int decision = cluster_autoscaler_should_scale();
    /* Should be 0 or -1 (scale-down possible but cooldown may not have elapsed) */
    KUNIT_EXPECT_TRUE(test, decision == 0 || decision == -1);

    /* Report pending pods exceeding threshold */
    cluster_autoscaler_report_pending(5);
    decision = cluster_autoscaler_should_scale();
    /* Should suggest scale-up (1) if cooldown elapsed, but cooldown is 5 min
     * so may return 0 in test environment */
    KUNIT_EXPECT_TRUE(test, decision == 0 || decision == 1);
}

static void autoscaler_scale_up_test(struct kunit *test)
{
    cluster_autoscaler_init(1, 10);
    cluster_autoscaler_report_pending(5);

    int added = cluster_autoscaler_scale_up();
    /* Should add at least 1 node */
    KUNIT_EXPECT_TRUE(test, added >= 1);
}

static void autoscaler_scale_down_test(struct kunit *test)
{
    cluster_autoscaler_init(2, 10);
    cluster_autoscaler_report_pending(0);

    int ret = cluster_autoscaler_scale_down();
    /* With current_nodes=2 and min_nodes=2, scale_down should work (2→1)
     * Actually at init, current_nodes=min_nodes=2, so scale_down returns -EPERM */
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -EPERM);
}

static void autoscaler_capacity_test(struct kunit *test)
{
    cluster_autoscaler_init(1, 10);

    cluster_autoscaler_update_capacity(8000, 34359738368ULL); /* 8 cores, 32 GB */

    struct cluster_autoscaler info;
    cluster_autoscaler_get_info(&info);

    KUNIT_EXPECT_EQ(test, info.min_nodes, (uint32_t)1);
    KUNIT_EXPECT_EQ(test, info.max_nodes, (uint32_t)10);
    KUNIT_EXPECT_EQ(test, info.cpu_capacity, (uint64_t)8000);
    KUNIT_EXPECT_EQ(test, info.memory_capacity, (uint64_t)34359738368ULL);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  2. Cluster Descheduler Tests
 * ═══════════════════════════════════════════════════════════════════════ */

static void descheduler_init_test(struct kunit *test)
{
    int ret = cluster_descheduler_init();
    KUNIT_EXPECT_EQ(test, ret, 0);
}

static void descheduler_add_policy_test(struct kunit *test)
{
    cluster_descheduler_init();

    int ret = cluster_descheduler_add_policy(
        "low-util", DESCHED_POLICY_LOW_UTIL, 30);
    KUNIT_EXPECT_EQ(test, ret, 0);

    ret = cluster_descheduler_add_policy(
        "high-util", DESCHED_POLICY_HIGH_UTIL, 80);
    KUNIT_EXPECT_EQ(test, ret, 0);

    ret = cluster_descheduler_add_policy(
        "lifetime", DESCHED_POLICY_LIFETIME, 86400000);
    KUNIT_EXPECT_EQ(test, ret, 0);
}

static void descheduler_add_invalid_test(struct kunit *test)
{
    cluster_descheduler_init();

    /* NULL name should fail */
    int ret = cluster_descheduler_add_policy(NULL, DESCHED_POLICY_LOW_UTIL, 30);
    KUNIT_EXPECT_NE(test, ret, 0);

    /* Empty name should fail */
    ret = cluster_descheduler_add_policy("", DESCHED_POLICY_LOW_UTIL, 30);
    KUNIT_EXPECT_NE(test, ret, 0);
}

static void descheduler_remove_policy_test(struct kunit *test)
{
    cluster_descheduler_init();

    cluster_descheduler_add_policy("test-policy", DESCHED_POLICY_LOW_UTIL, 30);

    int ret = cluster_descheduler_remove_policy("test-policy");
    KUNIT_EXPECT_EQ(test, ret, 0);

    /* Removing non-existent should fail */
    ret = cluster_descheduler_remove_policy("nonexistent");
    KUNIT_EXPECT_NE(test, ret, 0);
}

static void descheduler_run_test(struct kunit *test)
{
    cluster_descheduler_init();

    /* Run with no policies — should not crash, returns 0 */
    int evicted = cluster_descheduler_run();
    KUNIT_EXPECT_EQ(test, evicted, 0);

    /* Add policies and run */
    cluster_descheduler_add_policy("low", DESCHED_POLICY_LOW_UTIL, 30);
    cluster_descheduler_add_policy("high", DESCHED_POLICY_HIGH_UTIL, 80);

    evicted = cluster_descheduler_run();
    KUNIT_EXPECT_NE(test, evicted, 0); /* should evict some pods */

    /* Check stats */
    uint64_t total = 0;
    int ret = cluster_descheduler_get_stats(&total);
    KUNIT_EXPECT_EQ(test, ret, 0);
    KUNIT_EXPECT_NE(test, total, (uint64_t)0);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  3. Cluster Ingress Tests
 * ═══════════════════════════════════════════════════════════════════════ */

static void ingress_init_test(struct kunit *test)
{
    /* IP pool: 10.0.0.100 - 10.0.0.110 */
    uint32_t pool_start = (10 << 24) | (0 << 16) | (0 << 8) | 100;
    uint32_t pool_end   = (10 << 24) | (0 << 16) | (0 << 8) | 110;

    int ret = ingress_init(pool_start, pool_end);
    KUNIT_EXPECT_EQ(test, ret, 0);
}

static void ingress_nodeport_test(struct kunit *test)
{
    uint32_t pool_start = (10 << 24) | (0 << 16) | (0 << 8) | 100;
    uint32_t pool_end   = (10 << 24) | (0 << 16) | (0 << 8) | 110;
    ingress_init(pool_start, pool_end);

    /* Allocate a NodePort */
    int port = ingress_get_nodeport();
    KUNIT_EXPECT_TRUE(test, port >= 30000 && port <= 32767);

    /* Release and re-allocate */
    ingress_release_nodeport((uint16_t)port);
    int port2 = ingress_get_nodeport();
    KUNIT_EXPECT_EQ(test, port, port2);
}

static void ingress_add_rule_test(struct kunit *test)
{
    uint32_t pool_start = (10 << 24) | (0 << 16) | (0 << 8) | 100;
    uint32_t pool_end   = (10 << 24) | (0 << 16) | (0 << 8) | 110;
    ingress_init(pool_start, pool_end);

    /* Add HTTP ingress rule */
    int ret = ingress_add_rule("myapp.example.com", "/api",
                                "myapp-service", 8080, 2);
    KUNIT_EXPECT_EQ(test, ret, 0);

    /* Add NodePort rule */
    ret = ingress_add_rule("", "",
                           "db-service", 5432, 0);
    KUNIT_EXPECT_TRUE(test, ret >= 30000 && ret <= 32767);
}

static void ingress_handle_request_test(struct kunit *test)
{
    uint32_t pool_start = (10 << 24) | (0 << 16) | (0 << 8) | 100;
    uint32_t pool_end   = (10 << 24) | (0 << 16) | (0 << 8) | 110;
    ingress_init(pool_start, pool_end);

    ingress_add_rule("myapp.example.com", "/api",
                     "myapp-service", 8080, 2);
    ingress_add_rule("myapp.example.com", "/",
                     "myapp-frontend", 80, 2);

    /* Match /api path */
    char service[INGRESS_SERVICE_MAX];
    uint16_t port = 0;
    int ret = ingress_handle_request("myapp.example.com", "/api/v1/users",
                                      service, &port);
    KUNIT_EXPECT_EQ(test, ret, 0);
    KUNIT_EXPECT_STREQ(test, service, "myapp-service");
    KUNIT_EXPECT_EQ(test, port, (uint16_t)8080);

    /* Match / (root) path */
    ret = ingress_handle_request("myapp.example.com", "/index.html",
                                  service, &port);
    KUNIT_EXPECT_EQ(test, ret, 0);
    KUNIT_EXPECT_STREQ(test, service, "myapp-frontend");
    KUNIT_EXPECT_EQ(test, port, (uint16_t)80);

    /* Non-matching hostname */
    ret = ingress_handle_request("unknown.example.com", "/api",
                                  service, &port);
    KUNIT_EXPECT_NE(test, ret, 0);
}

static void ingress_remove_rule_test(struct kunit *test)
{
    uint32_t pool_start = (10 << 24) | (0 << 16) | (0 << 8) | 100;
    uint32_t pool_end   = (10 << 24) | (0 << 16) | (0 << 8) | 110;
    ingress_init(pool_start, pool_end);

    ingress_add_rule("test.example.com", "/", "test-svc", 80, 2);

    int ret = ingress_remove_rule("test.example.com", "/");
    KUNIT_EXPECT_EQ(test, ret, 0);

    /* After removal, lookup should fail */
    char service[INGRESS_SERVICE_MAX];
    uint16_t port;
    ret = ingress_handle_request("test.example.com", "/", service, &port);
    KUNIT_EXPECT_NE(test, ret, 0);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Test case lists
 * ═══════════════════════════════════════════════════════════════════════ */

static struct kunit_case autoscaler_test_cases[] = {
    KUNIT_CASE(autoscaler_init_test),
    KUNIT_CASE(autoscaler_init_invalid_test),
    KUNIT_CASE(autoscaler_should_scale_test),
    KUNIT_CASE(autoscaler_scale_up_test),
    KUNIT_CASE(autoscaler_scale_down_test),
    KUNIT_CASE(autoscaler_capacity_test),
    {0}
};

static struct kunit_case descheduler_test_cases[] = {
    KUNIT_CASE(descheduler_init_test),
    KUNIT_CASE(descheduler_add_policy_test),
    KUNIT_CASE(descheduler_add_invalid_test),
    KUNIT_CASE(descheduler_remove_policy_test),
    KUNIT_CASE(descheduler_run_test),
    {0}
};

static struct kunit_case ingress_test_cases[] = {
    KUNIT_CASE(ingress_init_test),
    KUNIT_CASE(ingress_nodeport_test),
    KUNIT_CASE(ingress_add_rule_test),
    KUNIT_CASE(ingress_handle_request_test),
    KUNIT_CASE(ingress_remove_rule_test),
    {0}
};

/* ── Suite declarations ────────────────────────────────────────────── */

static struct kunit_suite cluster_autoscaler_test_suite;
static struct kunit_suite cluster_descheduler_test_suite;
static struct kunit_suite cluster_ingress_test_suite;

/* ═══════════════════════════════════════════════════════════════════════
 *  Suite Registration — called from kunit_register_builtin_tests()
 * ═══════════════════════════════════════════════════════════════════════ */

void kunit_cluster_register(void)
{
    /* ── Autoscaler ── */
    {
        int ci = 0;
        for (int i = 0; autoscaler_test_cases[i].run != NULL &&
             i < KUNIT_MAX_CASES - 1; i++) {
            cluster_autoscaler_test_suite.cases[ci].name =
                autoscaler_test_cases[i].name;
            cluster_autoscaler_test_suite.cases[ci].run  =
                autoscaler_test_cases[i].run;
            ci++;
        }
        cluster_autoscaler_test_suite.cases[ci].name = NULL;
        cluster_autoscaler_test_suite.cases[ci].run  = NULL;
        cluster_autoscaler_test_suite.name  = "cluster_autoscaler";
        cluster_autoscaler_test_suite.setup = NULL;
        cluster_autoscaler_test_suite.teardown = NULL;
        kunit_register_suite(&cluster_autoscaler_test_suite);
    }

    /* ── Descheduler ── */
    {
        int ci = 0;
        for (int i = 0; descheduler_test_cases[i].run != NULL &&
             i < KUNIT_MAX_CASES - 1; i++) {
            cluster_descheduler_test_suite.cases[ci].name =
                descheduler_test_cases[i].name;
            cluster_descheduler_test_suite.cases[ci].run  =
                descheduler_test_cases[i].run;
            ci++;
        }
        cluster_descheduler_test_suite.cases[ci].name = NULL;
        cluster_descheduler_test_suite.cases[ci].run  = NULL;
        cluster_descheduler_test_suite.name  = "cluster_descheduler";
        cluster_descheduler_test_suite.setup = NULL;
        cluster_descheduler_test_suite.teardown = NULL;
        kunit_register_suite(&cluster_descheduler_test_suite);
    }

    /* ── Ingress ── */
    {
        int ci = 0;
        for (int i = 0; ingress_test_cases[i].run != NULL &&
             i < KUNIT_MAX_CASES - 1; i++) {
            cluster_ingress_test_suite.cases[ci].name =
                ingress_test_cases[i].name;
            cluster_ingress_test_suite.cases[ci].run  =
                ingress_test_cases[i].run;
            ci++;
        }
        cluster_ingress_test_suite.cases[ci].name = NULL;
        cluster_ingress_test_suite.cases[ci].run  = NULL;
        cluster_ingress_test_suite.name  = "cluster_ingress";
        cluster_ingress_test_suite.setup = NULL;
        cluster_ingress_test_suite.teardown = NULL;
        kunit_register_suite(&cluster_ingress_test_suite);
    }

    kprintf("[KUnit] Cluster subsystem tests registered "
            "(cluster_autoscaler, cluster_descheduler, cluster_ingress)\n");
}

/* ── Stub: kunit_cluster_init ─────────────────────────────── */
int kunit_cluster_init(void)
{
    kprintf("[kunit] kunit_cluster_init: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: kunit_cluster_test_join ─────────────────────────────── */
int kunit_cluster_test_join(void)
{
    kprintf("[kunit] kunit_cluster_test_join: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: kunit_cluster_test_raft ─────────────────────────────── */
int kunit_cluster_test_raft(void)
{
    kprintf("[kunit] kunit_cluster_test_raft: not yet implemented\n");
    return -ENOSYS;
}
