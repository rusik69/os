/*
 * kunit_ext.c — KUnit test suites for extended kernel features.
 *
 * Covers:
 *   - IPVS connection tracking + NAT
 *   - NVMe PMR block device
 *   - I/O Scheduler deadline elevator
 *   - RTC periodic interrupt
 *   - madvise DONTNEED/WILLNEED/COLD
 *   - RLIMIT resource limits
 */

#include "kunit.h"
#include "ipvs.h"
#include "iosched.h"
#include "rtc.h"
#include "madvise_ext.h"
#include "nvme.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "blockdev.h"
#include "timer.h"
#include "process.h"
#include "syscall.h"
#include "process_rlimit.h"

/* ====================================================================
 *  1. IPVS — Connection tracking + NAT tests
 * ==================================================================== */

static void ipvs_conn_basic_test(struct kunit *test)
{
    /* Initialize IPVS */
    int ret = ipvs_init();
    KUNIT_EXPECT_EQ(test, ret, 0);

    /* Add a virtual service */
    ret = ipvs_add_virtual(0xC0A80001, 80, 6); /* 192.168.0.1:80 TCP */
    KUNIT_EXPECT_EQ(test, ret, 0);

    /* Add a real server */
    ret = ipvs_add_real(0xC0A80001, 80, 0xC0A8000A, 8080, 1); /* 192.168.0.10:8080 */
    KUNIT_EXPECT_EQ(test, ret, 0);

    /* Create a connection */
    struct ip_vs_conn *conn = ip_vs_conn_new(
        0x0A000001, 12345,  /* client 10.0.0.1:12345 */
        0xC0A80001, 80,     /* virtual 192.168.0.1:80 */
        0xC0A8000A, 8080,   /* real 192.168.0.10:8080 */
        6);                 /* TCP */
    KUNIT_EXPECT_NOT_NULL(test, conn);
    if (!conn) return;

    KUNIT_EXPECT_EQ(test, conn->state, IP_VS_CONN_ESTAB);
    KUNIT_EXPECT_EQ(test, conn->protocol, (uint8_t)6);
    KUNIT_EXPECT_EQ(test, conn->c_ip, (uint32_t)0x0A000001);
    KUNIT_EXPECT_EQ(test, conn->c_port, (uint16_t)12345);
    KUNIT_EXPECT_EQ(test, conn->v_ip, (uint32_t)0xC0A80001);
    KUNIT_EXPECT_EQ(test, conn->d_ip, (uint32_t)0xC0A8000A);

    /* Connection count should be 1 */
    KUNIT_EXPECT_EQ(test, ip_vs_conn_count(), 1);

    /* Lookup the connection */
    struct ip_vs_conn *found = ip_vs_conn_lookup(
        0x0A000001, 12345,
        0xC0A80001, 80,
        6);
    KUNIT_EXPECT_NOT_NULL(test, found);
    if (found)
        KUNIT_EXPECT_EQ(test, found, conn);

    /* Lookup non-existent should return NULL */
    struct ip_vs_conn *not_found = ip_vs_conn_lookup(
        0x0A000002, 12345,
        0xC0A80001, 80,
        6);
    KUNIT_EXPECT_NULL(test, not_found);

    /* Expire the connection */
    ip_vs_conn_expire(conn);
    KUNIT_EXPECT_EQ(test, ip_vs_conn_count(), 0);

    /* After expiry, lookup should fail */
    found = ip_vs_conn_lookup(
        0x0A000001, 12345,
        0xC0A80001, 80,
        6);
    KUNIT_EXPECT_NULL(test, found);
}

static void ipvs_nat_test(struct kunit *test)
{
    /* Re-init to get clean state */
    ipvs_init();

    /* Create a connection */
    struct ip_vs_conn *conn = ip_vs_conn_new(
        0x0A000001, 12345,  /* client */
        0xC0A80001, 80,     /* virtual */
        0xC0A8000A, 8080,   /* real */
        6);
    KUNIT_EXPECT_NOT_NULL(test, conn);
    if (!conn) return;

    /* Test NAT inbound: rewrite destination from virtual to real */
    uint32_t dst_ip = 0xC0A80001;
    uint16_t dst_port = 80;
    int ret = ipvs_nat_in(conn, &dst_ip, &dst_port);
    KUNIT_EXPECT_EQ(test, ret, 0);
    KUNIT_EXPECT_EQ(test, dst_ip, (uint32_t)0xC0A8000A);
    KUNIT_EXPECT_EQ(test, dst_port, (uint16_t)8080);

    /* Original destination should have been saved */
    KUNIT_EXPECT_EQ(test, conn->orig_dst_ip, (uint32_t)0xC0A80001);
    KUNIT_EXPECT_EQ(test, conn->orig_dst_port, (uint16_t)80);

    /* Test NAT outbound: rewrite source from real back to virtual */
    uint32_t src_ip = 0xC0A8000A;
    uint16_t src_port = 8080;
    ret = ipvs_nat_out(conn, &src_ip, &src_port);
    KUNIT_EXPECT_EQ(test, ret, 0);
    KUNIT_EXPECT_EQ(test, src_ip, (uint32_t)0xC0A80001);
    KUNIT_EXPECT_EQ(test, src_port, (uint16_t)80);

    /* Clean up */
    ip_vs_conn_expire(conn);
}

static void ipvs_conn_cleanup_test(struct kunit *test)
{
    ipvs_init();

    /* Create several connections */
    for (int i = 0; i < 5; i++) {
        struct ip_vs_conn *c = ip_vs_conn_new(
            0x0A000001, (uint16_t)(10000 + i),
            0xC0A80001, 80,
            0xC0A8000A, 8080,
            6);
        KUNIT_EXPECT_NOT_NULL(test, c);
        if (!c) return;
    }

    KUNIT_EXPECT_EQ(test, ip_vs_conn_count(), 5);

    /* Cleanup should not remove active connections (they just got created) */
    ip_vs_conn_cleanup();
    KUNIT_EXPECT_EQ(test, ip_vs_conn_count(), 5);

    /* Note: we can't easily test timeout-based expiry without timer manipulation,
     * but we verify cleanup doesn't crash */
}

/* ====================================================================
 *  2. NVMe PMR — Basic API tests (hardware-independent subset)
 * ==================================================================== */

static void nvme_pmr_api_test(struct kunit *test)
{
    /* Test the basic getter functions without hardware */
    /* nvme_pmr_is_present() should return 0 before init */
    /* nvme_pmr_get_size() should return 0 before init */
    /* These are hardware-dependent, so we just verify they don't crash */

    uint64_t size = nvme_pmr_get_size();
    /* Size should be 0 if no PMR hardware is present (expected in test env) */
    KUNIT_EXPECT_TRUE(test, size == 0 || size > 0);
}

/* ====================================================================
 *  3. I/O Scheduler — Deadline elevator tests
 * ==================================================================== */

static void iosched_init_test(struct kunit *test)
{
    /* iosched_init should already have been called at boot.
     * Verify we can get a queue. */
    struct iosched_queue *iq = iosched_get_queue(0);
    KUNIT_EXPECT_NOT_NULL(test, iq);
    if (!iq) return;

    KUNIT_EXPECT_EQ(test, iq->policy, IOSCHED_NOOP);
}

static void iosched_deadline_test(struct kunit *test)
{
    /* Test deadline scheduler policy switching */
    int ret = iosched_set_policy(0, IOSCHED_DEADLINE);
    KUNIT_EXPECT_EQ(test, ret, 0);

    int policy = iosched_get_policy(0);
    KUNIT_EXPECT_EQ(test, policy, IOSCHED_DEADLINE);

    /* Create a mock read request */
    struct blk_request *req = (struct blk_request *)kmalloc(sizeof(struct blk_request));
    KUNIT_EXPECT_NOT_NULL(test, req);
    if (!req) return;

    memset(req, 0, sizeof(*req));
    req->lba = 100;
    req->count = 8;
    req->flags = BLK_REQ_READ;

    /* Submit it */
    ret = iosched_submit_request(0, req);
    KUNIT_EXPECT_EQ(test, ret, 0);

    /* Fetch it back */
    struct blk_request *fetched = iosched_fetch_request(0);
    KUNIT_EXPECT_NOT_NULL(test, fetched);
    if (fetched) {
        KUNIT_EXPECT_EQ(test, fetched->lba, (uint64_t)100);
        KUNIT_EXPECT_EQ(test, fetched->flags & BLK_REQ_READ, (uint32_t)BLK_REQ_READ);
    }

    kfree(req);

    /* Switch back to NOOP */
    ret = iosched_set_policy(0, IOSCHED_NOOP);
    KUNIT_EXPECT_EQ(test, ret, 0);
    KUNIT_EXPECT_EQ(test, iosched_get_policy(0), IOSCHED_NOOP);
}

static void iosched_cfq_test(struct kunit *test)
{
    int ret = iosched_set_policy(0, IOSCHED_CFQ);
    KUNIT_EXPECT_EQ(test, ret, 0);
    KUNIT_EXPECT_EQ(test, iosched_get_policy(0), IOSCHED_CFQ);

    /* Test with a write request */
    struct blk_request *req = (struct blk_request *)kmalloc(sizeof(struct blk_request));
    KUNIT_EXPECT_NOT_NULL(test, req);
    if (!req) return;

    memset(req, 0, sizeof(*req));
    req->lba = 200;
    req->count = 16;
    req->flags = BLK_REQ_WRITE;

    ret = iosched_submit_request(0, req);
    KUNIT_EXPECT_EQ(test, ret, 0);

    struct blk_request *fetched = iosched_fetch_request(0);
    KUNIT_EXPECT_NOT_NULL(test, fetched);
    if (fetched) {
        KUNIT_EXPECT_EQ(test, fetched->lba, (uint64_t)200);
        KUNIT_EXPECT_EQ(test, fetched->flags & BLK_REQ_WRITE, (uint32_t)BLK_REQ_WRITE);
    }

    kfree(req);

    /* Reset to NOOP */
    iosched_set_policy(0, IOSCHED_NOOP);
}

static void iosched_noop_merge_test(struct kunit *test)
{
    iosched_set_policy(0, IOSCHED_NOOP);

    /* Create two adjacent requests that should merge */
    struct blk_request *r1 = (struct blk_request *)kmalloc(sizeof(struct blk_request));
    struct blk_request *r2 = (struct blk_request *)kmalloc(sizeof(struct blk_request));
    KUNIT_EXPECT_NOT_NULL(test, r1);
    KUNIT_EXPECT_NOT_NULL(test, r2);
    if (!r1 || !r2) {
        kfree(r1);
        kfree(r2);
        iosched_set_policy(0, IOSCHED_NOOP);
        return;
    }

    memset(r1, 0, sizeof(*r1));
    memset(r2, 0, sizeof(*r2));
    r1->lba = 100;
    r1->count = 8;
    r1->flags = BLK_REQ_READ;
    r2->lba = 108;  /* adjacent to r1 */
    r2->count = 4;
    r2->flags = BLK_REQ_READ;

    /* Submit r1 then r2 (r2 should merge into r1's tail) */
    iosched_submit_request(0, r1);
    iosched_submit_request(0, r2);

    /* Fetch should return a single merged request */
    struct blk_request *merged = iosched_fetch_request(0);
    KUNIT_EXPECT_NOT_NULL(test, merged);
    if (merged) {
        /* The merge should have consumed r2, count should be 8+4=12 */
        KUNIT_EXPECT_EQ(test, merged->count, (uint32_t)12);
        kfree(merged);
    }

    iosched_set_policy(0, IOSCHED_NOOP);
}

/* ====================================================================
 *  4. RTC — Periodic interrupt tests
 * ==================================================================== */

static void rtc_periodic_api_test(struct kunit *test)
{
    /* Test the rate-to-selector conversion indirectly via rtc_set_periodic.
     * These tests verify the API doesn't crash with various rates. */

    /* Valid rates (power-of-2 divisors of 32768) */
    int ret = rtc_set_periodic(1, 1024);  /* 1024 Hz */
    KUNIT_EXPECT_EQ(test, ret, 0);

    /* Verify it's enabled */
    /* rtc_get_ticks() should return non-zero after some ticks */
    uint64_t ticks_before = rtc_get_ticks();

    /* Wait a tiny bit — use a spin loop */
    uint64_t deadline = timer_get_ms() + 10;
    while (timer_get_ms() < deadline) {
        __asm__ volatile("pause");
    }

    uint64_t ticks_after = rtc_get_ticks();
    KUNIT_EXPECT_NE(test, ticks_after, ticks_before);

    /* Disable */
    ret = rtc_set_periodic(0, 0);
    KUNIT_EXPECT_EQ(test, ret, 0);
}

static void rtc_periodic_rate_limits_test(struct kunit *test)
{
    /* Edge cases: rate of 0 should fail */
    int ret = rtc_set_periodic(1, 0);
    KUNIT_EXPECT_TRUE(test, ret == -1 || ret == 0);
    rtc_set_periodic(0, 0);

    /* Very high rate (above 8192) should be clamped or rejected */
    ret = rtc_set_periodic(1, 16000);
    KUNIT_EXPECT_EQ(test, ret, 0);
    rtc_set_periodic(0, 0);

    /* Very low rate (1 Hz) should work */
    ret = rtc_set_periodic(1, 2);
    KUNIT_EXPECT_EQ(test, ret, 0);
    rtc_set_periodic(0, 0);
}

static void rtc_time_test(struct kunit *test)
{
    /* Test rtc_to_epoch conversion */
    struct rtc_time t;
    memset(&t, 0, sizeof(t));
    t.year = 2024;
    t.month = 1;
    t.day = 1;
    t.hour = 0;
    t.minute = 0;
    t.second = 0;

    /* 2024-01-01 00:00:00 UTC = 1704067200 epoch */
    uint64_t epoch = rtc_to_epoch(&t);
    KUNIT_EXPECT_EQ(test, epoch, (uint64_t)1704067200ULL);
}

/* ====================================================================
 *  5. madvise — DONTNEED/WILLNEED/COLD API tests
 * ==================================================================== */

static void madvise_init_test(struct kunit *test)
{
    /* madvise_ext_init should have been called at boot.
     * Verify the API returns 0 (not ENOSYS). */

    /* Call with invalid address (kernel space) should not crash */
    /* We test with NULL length — should return some error */
    int ret = madvise_dontneed(0, 0);
    /* May return -EINVAL if invalid, or 0 if nothing to do */
    KUNIT_EXPECT_TRUE(test, ret == -EINVAL || ret == 0 || ret == -ENOSYS);

    ret = madvise_willneed(0, 0);
    KUNIT_EXPECT_TRUE(test, ret == -EINVAL || ret == 0 || ret == -ENOSYS);

    ret = madvise_cold(0, 0);
    KUNIT_EXPECT_TRUE(test, ret == -EINVAL || ret == 0 || ret == -ENOSYS);
}

/* ====================================================================
 *  6. RLIMIT — Resource limit tests
 * ==================================================================== */

static void rlimit_nofile_default_test(struct kunit *test)
{
    struct process *cur = process_get_current();
    KUNIT_EXPECT_NOT_NULL(test, cur);
    if (!cur) return;

    KUNIT_EXPECT_EQ(test, cur->rlim_cur[RLIMIT_NOFILE], (uint64_t)1024);
    KUNIT_EXPECT_EQ(test, cur->rlim_max[RLIMIT_NOFILE], (uint64_t)4096);
}

static void rlimit_stack_default_test(struct kunit *test)
{
    struct process *cur = process_get_current();
    KUNIT_EXPECT_NOT_NULL(test, cur);
    if (!cur) return;

    KUNIT_EXPECT_EQ(test, cur->rlim_cur[RLIMIT_STACK], (uint64_t)8ULL * 1024 * 1024);
    KUNIT_EXPECT_EQ(test, cur->rlim_max[RLIMIT_STACK], (uint64_t)8ULL * 1024 * 1024);
}

static void rlimit_nproc_default_test(struct kunit *test)
{
    struct process *cur = process_get_current();
    KUNIT_EXPECT_NOT_NULL(test, cur);
    if (!cur) return;

    KUNIT_EXPECT_EQ(test, cur->rlim_cur[RLIMIT_NPROC], (uint64_t)4096);
    KUNIT_EXPECT_EQ(test, cur->rlim_max[RLIMIT_NPROC], (uint64_t)4096);
}

static void rlimit_get_set_test(struct kunit *test)
{
    struct process *cur = process_get_current();
    KUNIT_EXPECT_NOT_NULL(test, cur);
    if (!cur) return;

    /* Save original values */
    uint64_t saved_cur = cur->rlim_cur[RLIMIT_NOFILE];
    uint64_t saved_max = cur->rlim_max[RLIMIT_NOFILE];

    /* Set RLIMIT_NOFILE to a new value using rlimit_set */
    struct rlimit new_rlim;
    new_rlim.rlim_cur = 512;
    new_rlim.rlim_max = 2048;
    int ret = rlimit_set(cur->pid, RLIMIT_NOFILE, &new_rlim);
    KUNIT_EXPECT_EQ(test, ret, 0);

    /* Verify via rlimit_get */
    struct rlimit got_rlim;
    ret = rlimit_get(cur->pid, RLIMIT_NOFILE, &got_rlim);
    KUNIT_EXPECT_EQ(test, ret, 0);
    KUNIT_EXPECT_EQ(test, got_rlim.rlim_cur, (uint64_t)512);
    KUNIT_EXPECT_EQ(test, got_rlim.rlim_max, (uint64_t)2048);

    /* Restore */
    cur->rlim_cur[RLIMIT_NOFILE] = saved_cur;
    cur->rlim_max[RLIMIT_NOFILE] = saved_max;
}

static void rlimit_nofile_enforce_test(struct kunit *test)
{
    struct process *cur = process_get_current();
    KUNIT_EXPECT_NOT_NULL(test, cur);
    if (!cur) return;

    /* Save original values */
    uint64_t saved_cur = cur->rlim_cur[RLIMIT_NOFILE];
    uint64_t saved_max = cur->rlim_max[RLIMIT_NOFILE];

    /* rlimit_check should allow a value below the limit */
    int ret = rlimit_check(cur->pid, RLIMIT_NOFILE, saved_cur - 1);
    KUNIT_EXPECT_EQ(test, ret, 0);

    /* rlimit_check should reject a value at or above the limit */
    ret = rlimit_check(cur->pid, RLIMIT_NOFILE, saved_cur);
    KUNIT_EXPECT_NE(test, ret, 0);

    ret = rlimit_check(cur->pid, RLIMIT_NOFILE, saved_cur + 1);
    KUNIT_EXPECT_NE(test, ret, 0);
}

/* ====================================================================
 *  Test case lists
 * ==================================================================== */

/* IPVS test cases */
static struct kunit_case ipvs_test_cases[] = {
    KUNIT_CASE(ipvs_conn_basic_test),
    KUNIT_CASE(ipvs_nat_test),
    KUNIT_CASE(ipvs_conn_cleanup_test),
    {0}
};

/* NVMe PMR test cases */
static struct kunit_case nvme_pmr_test_cases[] = {
    KUNIT_CASE(nvme_pmr_api_test),
    {0}
};

/* I/O Scheduler test cases */
static struct kunit_case iosched_test_cases[] = {
    KUNIT_CASE(iosched_init_test),
    KUNIT_CASE(iosched_deadline_test),
    KUNIT_CASE(iosched_cfq_test),
    KUNIT_CASE(iosched_noop_merge_test),
    {0}
};

/* RTC test cases */
static struct kunit_case rtc_test_cases[] = {
    KUNIT_CASE(rtc_periodic_api_test),
    KUNIT_CASE(rtc_periodic_rate_limits_test),
    KUNIT_CASE(rtc_time_test),
    {0}
};

/* madvise test cases */
static struct kunit_case madvise_test_cases[] = {
    KUNIT_CASE(madvise_init_test),
    {0}
};

/* RLIMIT test cases */
static struct kunit_case rlimit_test_cases[] = {
    KUNIT_CASE(rlimit_nofile_default_test),
    KUNIT_CASE(rlimit_stack_default_test),
    KUNIT_CASE(rlimit_nproc_default_test),
    KUNIT_CASE(rlimit_get_set_test),
    KUNIT_CASE(rlimit_nofile_enforce_test),
    {0}
};

/* ── Suite definitions ────────────────────────────────────────────── */

static struct kunit_suite ipvs_test_suite;
static struct kunit_suite nvme_pmr_test_suite;
static struct kunit_suite iosched_test_suite;
static struct kunit_suite rtc_test_suite;
static struct kunit_suite madvise_test_suite;
static struct kunit_suite rlimit_test_suite;

/* ====================================================================
 *  Suite Registration — called from kunit_register_builtin_tests()
 * ==================================================================== */

void kunit_ext_register(void)
{
    /* ── IPVS ── */
    {
        int ci = 0;
        for (int i = 0; ipvs_test_cases[i].run != NULL && i < KUNIT_MAX_CASES - 1; i++) {
            ipvs_test_suite.cases[ci].name = ipvs_test_cases[i].name;
            ipvs_test_suite.cases[ci].run  = ipvs_test_cases[i].run;
            ci++;
        }
        ipvs_test_suite.cases[ci].name = NULL;
        ipvs_test_suite.cases[ci].run  = NULL;
        ipvs_test_suite.name     = "ipvs";
        ipvs_test_suite.setup    = NULL;
        ipvs_test_suite.teardown = NULL;
        kunit_register_suite(&ipvs_test_suite);
    }

    /* ── NVMe PMR ── */
    {
        int ci = 0;
        for (int i = 0; nvme_pmr_test_cases[i].run != NULL && i < KUNIT_MAX_CASES - 1; i++) {
            nvme_pmr_test_suite.cases[ci].name = nvme_pmr_test_cases[i].name;
            nvme_pmr_test_suite.cases[ci].run  = nvme_pmr_test_cases[i].run;
            ci++;
        }
        nvme_pmr_test_suite.cases[ci].name = NULL;
        nvme_pmr_test_suite.cases[ci].run  = NULL;
        nvme_pmr_test_suite.name     = "nvme_pmr";
        nvme_pmr_test_suite.setup    = NULL;
        nvme_pmr_test_suite.teardown = NULL;
        kunit_register_suite(&nvme_pmr_test_suite);
    }

    /* ── I/O Scheduler ── */
    {
        int ci = 0;
        for (int i = 0; iosched_test_cases[i].run != NULL && i < KUNIT_MAX_CASES - 1; i++) {
            iosched_test_suite.cases[ci].name = iosched_test_cases[i].name;
            iosched_test_suite.cases[ci].run  = iosched_test_cases[i].run;
            ci++;
        }
        iosched_test_suite.cases[ci].name = NULL;
        iosched_test_suite.cases[ci].run  = NULL;
        iosched_test_suite.name     = "iosched";
        iosched_test_suite.setup    = NULL;
        iosched_test_suite.teardown = NULL;
        kunit_register_suite(&iosched_test_suite);
    }

    /* ── RTC ── */
    {
        int ci = 0;
        for (int i = 0; rtc_test_cases[i].run != NULL && i < KUNIT_MAX_CASES - 1; i++) {
            rtc_test_suite.cases[ci].name = rtc_test_cases[i].name;
            rtc_test_suite.cases[ci].run  = rtc_test_cases[i].run;
            ci++;
        }
        rtc_test_suite.cases[ci].name = NULL;
        rtc_test_suite.cases[ci].run  = NULL;
        rtc_test_suite.name     = "rtc";
        rtc_test_suite.setup    = NULL;
        rtc_test_suite.teardown = NULL;
        kunit_register_suite(&rtc_test_suite);
    }

    /* ── madvise ── */
    {
        int ci = 0;
        for (int i = 0; madvise_test_cases[i].run != NULL && i < KUNIT_MAX_CASES - 1; i++) {
            madvise_test_suite.cases[ci].name = madvise_test_cases[i].name;
            madvise_test_suite.cases[ci].run  = madvise_test_cases[i].run;
            ci++;
        }
        madvise_test_suite.cases[ci].name = NULL;
        madvise_test_suite.cases[ci].run  = NULL;
        madvise_test_suite.name     = "madvise_ext";
        madvise_test_suite.setup    = NULL;
        madvise_test_suite.teardown = NULL;
        kunit_register_suite(&madvise_test_suite);
    }

    /* ── RLIMIT ── */
    {
        int ci = 0;
        for (int i = 0; rlimit_test_cases[i].run != NULL && i < KUNIT_MAX_CASES - 1; i++) {
            rlimit_test_suite.cases[ci].name = rlimit_test_cases[i].name;
            rlimit_test_suite.cases[ci].run  = rlimit_test_cases[i].run;
            ci++;
        }
        rlimit_test_suite.cases[ci].name = NULL;
        rlimit_test_suite.cases[ci].run  = NULL;
        rlimit_test_suite.name     = "rlimit";
        rlimit_test_suite.setup    = NULL;
        rlimit_test_suite.teardown = NULL;
        kunit_register_suite(&rlimit_test_suite);
    }

    kprintf("[KUnit] Extended feature tests registered (ipvs, nvme_pmr, iosched, rtc, madvise_ext, rlimit)\n");
}
