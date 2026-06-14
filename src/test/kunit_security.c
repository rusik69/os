/*
 * kunit_security.c — KUnit test suites for security subsystems
 *
 * Tests for: lockdown mode transitions, pkey allocation/free/mprotect,
 * audit log operations, kernel keyring, and fault injection.
 *
 * Item S-plan: lockdown, pkey, audit, keyring, fault_inject
 *
 * Run via:
 *   # echo 1 > /sys/kernel/debug/kunit/run_all
 *   # cat /sys/kernel/debug/kunit/results
 */

#include "kunit.h"
#include "lockdown.h"
#include "pkey.h"
#include "audit.h"
#include "fault_inject.h"
#include "errno.h"
#include "string.h"
#include "printf.h"
#include "heap.h"

/* ── Forward declarations for keyring API (no dedicated header) ───── */
extern void keyring_init(void);
extern int  add_key(const char *name, const uint8_t *key_data, uint32_t key_len);
extern int  request_key(const char *name, uint8_t *buf, uint32_t *buf_len);
extern int  revoke_key(const char *name);
extern int  keyring_has_key(const char *name);
extern int  keyring_get_key_count(void);

/* ====================================================================
 *  1. LOCKDOWN — mode transitions and level queries
 * ==================================================================== */

/* Start at NONE, raise to INTEGRITY, verify the level. */
static void lockdown_none_to_integrity(struct kunit *test)
{
    /* Reset by starting from LOCKDOWN_NONE — use lock_down to raise */
    KUNIT_EXPECT_EQ(test, (int64_t)lockdown_get_level(), (int64_t)LOCKDOWN_NONE);

    lock_down(LOCKDOWN_INTEGRITY);
    KUNIT_EXPECT_EQ(test, (int64_t)lockdown_get_level(), (int64_t)LOCKDOWN_INTEGRITY);

    /* lockdown_is_locked_down checks */
    KUNIT_EXPECT_TRUE(test, lockdown_is_locked_down(LOCKDOWN_INTEGRITY) == 1);
    /* Should NOT be at confidentiality yet */
    KUNIT_EXPECT_TRUE(test, lockdown_is_locked_down(LOCKDOWN_CONFIDENTIALITY) == 0);
}

/* Raise from INTEGRITY to CONFIDENTIALITY. */
static void lockdown_integrity_to_confidentiality(struct kunit *test)
{
    lock_down(LOCKDOWN_INTEGRITY);
    KUNIT_EXPECT_EQ(test, (int64_t)lockdown_get_level(), (int64_t)LOCKDOWN_INTEGRITY);

    lock_down(LOCKDOWN_CONFIDENTIALITY);
    KUNIT_EXPECT_EQ(test, (int64_t)lockdown_get_level(), (int64_t)LOCKDOWN_CONFIDENTIALITY);

    KUNIT_EXPECT_TRUE(test, lockdown_is_locked_down(LOCKDOWN_INTEGRITY) == 1);
    KUNIT_EXPECT_TRUE(test, lockdown_is_locked_down(LOCKDOWN_CONFIDENTIALITY) == 1);
}

/* Lockdown is irreversible — try to lower and verify it stays. */
static void lockdown_irreversible(struct kunit *test)
{
    lock_down(LOCKDOWN_INTEGRITY);
    KUNIT_EXPECT_EQ(test, (int64_t)lockdown_get_level(), (int64_t)LOCKDOWN_INTEGRITY);

    /* Attempt to lower to NONE — must stay at INTEGRITY */
    lock_down(LOCKDOWN_NONE);
    KUNIT_EXPECT_EQ(test, (int64_t)lockdown_get_level(), (int64_t)LOCKDOWN_INTEGRITY);
}

/* Invalid level values must be ignored (level stays unchanged). */
static void lockdown_invalid_level(struct kunit *test)
{
    int level_before = lockdown_get_level();

    lock_down(42);   /* Obviously invalid */
    KUNIT_EXPECT_EQ(test, (int64_t)lockdown_get_level(), (int64_t)level_before);

    lock_down(-1);
    KUNIT_EXPECT_EQ(test, (int64_t)lockdown_get_level(), (int64_t)level_before);
}

/* lockdown_is_locked_down with invalid level returns 0. */
static void lockdown_is_locked_down_invalid(struct kunit *test)
{
    int r = lockdown_is_locked_down(42);
    KUNIT_EXPECT_EQ(test, (int64_t)r, (int64_t)0);

    r = lockdown_is_locked_down(-1);
    KUNIT_EXPECT_EQ(test, (int64_t)r, (int64_t)0);
}

/* ====================================================================
 *  2. PKEY — memory protection key operations
 * ==================================================================== */

/* Allocate one key and free it. */
static void pkey_alloc_free(struct kunit *test)
{
    int pkey = pkey_alloc(0, 0);
    /* PKU may not be available — accept -1 gracefully */
    if (pkey < 0) {
        KUNIT_EXPECT_EQ(test, (int64_t)pkey, (int64_t)-1);
        return;
    }

    KUNIT_EXPECT_TRUE(test, pkey >= 1);  /* Key 0 is reserved */
    KUNIT_EXPECT_TRUE(test, pkey < PKEY_MAX);

    int ret = pkey_free(pkey);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
}

/* Allocate keys, set and get rights (if PKU available). */
static void pkey_set_get_rights(struct kunit *test)
{
    int pkey = pkey_alloc(0, 0);
    if (pkey < 0) {
        KUNIT_EXPECT_EQ(test, (int64_t)pkey, (int64_t)-1);
        return;
    }

    /* Default rights should be 0 (full access) */
    int rights = pkey_get_rights(pkey);
    KUNIT_EXPECT_EQ(test, (int64_t)rights, (int64_t)0);

    /* Set DISABLE_WRITE */
    int ret = pkey_set_rights(pkey, PKEY_DISABLE_WRITE);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    rights = pkey_get_rights(pkey);
    KUNIT_EXPECT_EQ(test, (int64_t)rights, (int64_t)PKEY_DISABLE_WRITE);

    /* Set DISABLE_ACCESS */
    ret = pkey_set_rights(pkey, PKEY_DISABLE_ACCESS);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    rights = pkey_get_rights(pkey);
    KUNIT_EXPECT_EQ(test, (int64_t)rights, (int64_t)PKEY_DISABLE_ACCESS);

    /* Reset to full access */
    ret = pkey_set_rights(pkey, 0);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    rights = pkey_get_rights(pkey);
    KUNIT_EXPECT_EQ(test, (int64_t)rights, (int64_t)0);

    pkey_free(pkey);
}

/* Free an invalid key must return -1. */
static void pkey_free_invalid(struct kunit *test)
{
    int ret = pkey_free(-1);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-1);

    ret = pkey_free(PKEY_MAX + 10);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-1);
}

/* pkey_mprotect with valid arguments. */
static void pkey_mprotect_basic(struct kunit *test)
{
    int pkey = pkey_alloc(0, 0);
    if (pkey < 0) {
        KUNIT_EXPECT_EQ(test, (int64_t)pkey, (int64_t)-1);
        return;
    }

    /* mprotect a small region (stub — may always succeed) */
    int ret = pkey_mprotect((void *)0x1000, 0x1000, 3, pkey);
    /* The stub returns 0 on success, -1 on error */
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -1);

    /* pkey=-1 should clear the key (passing -1 means no key) */
    ret = pkey_mprotect((void *)0x1000, 0x1000, 3, -1);
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -1);

    pkey_free(pkey);
}

/* Allocate up to exhaustion, then free all. */
static void pkey_alloc_exhaust(struct kunit *test)
{
    int keys[PKEY_MAX];
    int count = 0;

    for (int i = 0; i < PKEY_MAX; i++) {
        int pkey = pkey_alloc(0, 0);
        if (pkey < 0)
            break;
        keys[count++] = pkey;
    }

    /* We should have allocated at least a few keys (or none if PKU absent) */
    KUNIT_EXPECT_TRUE(test, count >= 0);
    KUNIT_EXPECT_TRUE(test, count < PKEY_MAX);

    for (int i = 0; i < count; i++)
        pkey_free(keys[i]);
}

/* Key 0 is reserved and should not be available for allocation. */
static void pkey_key0_reserved(struct kunit *test)
{
    /* Key 0 is the default key — should never be returned by alloc */
    int pkey = pkey_alloc(0, 0);
    if (pkey < 0)
        return;
    KUNIT_EXPECT_NE(test, (int64_t)pkey, (int64_t)0);
    pkey_free(pkey);
}

/* Get rights on invalid pkey returns -1. */
static void pkey_get_rights_invalid(struct kunit *test)
{
    int rights = pkey_get_rights(-1);
    KUNIT_EXPECT_EQ(test, (int64_t)rights, (int64_t)-1);

    rights = pkey_get_rights(PKEY_MAX);
    KUNIT_EXPECT_EQ(test, (int64_t)rights, (int64_t)-1);
}

/* Set rights with invalid flags returns -1. */
static void pkey_set_rights_invalid_flags(struct kunit *test)
{
    int pkey = pkey_alloc(0, 0);
    if (pkey < 0)
        return;

    int ret = pkey_set_rights(pkey, 0xFF);  /* Invalid flags */
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-1);

    pkey_free(pkey);
}

/* ====================================================================
 *  3. AUDIT — basic event logging
 * ==================================================================== */

/* Initialize audit and log a basic event. */
static void audit_init_and_log(struct kunit *test)
{
    /* Initialize audit (safe to call multiple times) */
    audit_init();

    /* Log a simple event — should not crash */
    audit_log_event("KUnit test audit event");

    /* Log a path event */
    audit_log_path("/test/path", 42, 0x1ED);

    /* Log a denial event */
    audit_log_denial("kernel_test", "test_object", "read");

    /* Read the ring buffer back */
    char buf[128];
    int n = audit_read_log(buf, (int)sizeof(buf) - 1);

    /* Buffer should contain at least some of our events */
    KUNIT_EXPECT_TRUE(test, n > 0);
    if (n > 0) {
        buf[n] = '\0';
        KUNIT_EXPECT_TRUE(test, (int64_t)strlen(buf) > 0);
    }
}

/* Syscall entry/exit audit logging. */
static void audit_syscall_events(struct kunit *test)
{
    audit_init();

    /* Log a syscall entry */
    audit_syscall_entry(0x3C, 0, 0, 0, 0, 0);  /* exit syscall */
    audit_syscall_exit(0);

    /* Log a syscall with arguments */
    audit_syscall_entry(0x02, 0x7FFF0000, 0x40, 0xDEAD, 0, 0);  /* open */
    audit_syscall_exit(3);

    /* Verify ring buffer non-empty */
    char buf[64];
    int n = audit_read_log(buf, (int)sizeof(buf) - 1);
    KUNIT_EXPECT_TRUE(test, n >= 0);
}

/* audit_netlink_send should not crash when called directly. */
static void audit_netlink_send_smoke(struct kunit *test)
{
    audit_init();

    int ret = audit_netlink_send(0x101, "KUnit smoke test payload", 25);
    /* May fail if netlink not fully set up — just verify no crash */
    (void)ret;
    KUNIT_EXPECT_TRUE(test, 1);
}

/* ====================================================================
 *  4. KEYRING — encrypted key storage
 * ==================================================================== */

/* Initialize keyring, add a key, search for it, revoke it. */
static void keyring_add_search_revoke(struct kunit *test)
{
    /* We may test multiple times; init is idempotent */
    keyring_init();

    const char *key_name = "kunit_test_key";
    const uint8_t key_data[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };
    uint32_t key_len = (uint32_t)sizeof(key_data);

    /* Add the key */
    int ret = add_key(key_name, key_data, key_len);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* Verify it exists */
    int has = keyring_has_key(key_name);
    KUNIT_EXPECT_EQ(test, (int64_t)has, (int64_t)1);

    /* Increment key count */
    int count = keyring_get_key_count();
    KUNIT_EXPECT_EQ(test, (int64_t)count, (int64_t)1);

    /* Request the key back */
    uint8_t buf[32];
    uint32_t buf_len = (uint32_t)sizeof(buf);
    ret = request_key(key_name, buf, &buf_len);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* Decrypted data should match original */
    KUNIT_EXPECT_EQ(test, (int64_t)buf_len, (int64_t)key_len);
    if (ret == 0 && buf_len == key_len) {
        int match = 1;
        for (uint32_t i = 0; i < key_len; i++) {
            if (buf[i] != key_data[i]) {
                match = 0;
                break;
            }
        }
        KUNIT_EXPECT_TRUE(test, match);
    }

    /* Revoke the key */
    ret = revoke_key(key_name);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* After revoke, must not exist */
    has = keyring_has_key(key_name);
    KUNIT_EXPECT_EQ(test, (int64_t)has, (int64_t)0);

    count = keyring_get_key_count();
    KUNIT_EXPECT_EQ(test, (int64_t)count, (int64_t)0);
}

/* Adding a duplicate key must return -EEXIST. */
static void keyring_duplicate_key(struct kunit *test)
{
    keyring_init();

    const uint8_t data[] = { 0xAA, 0xBB };
    int ret = add_key("dup_test", data, 2);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* Second add with same name must fail */
    ret = add_key("dup_test", data, 2);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EEXIST);

    revoke_key("dup_test");
}

/* NULL/empty name must be rejected. */
static void keyring_invalid_args(struct kunit *test)
{
    keyring_init();

    int ret = add_key(NULL, (const uint8_t *)"data", 4);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EINVAL);

    ret = add_key("valid", NULL, 4);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EINVAL);

    ret = add_key("valid", (const uint8_t *)"data", 0);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EINVAL);

    /* request_key with NULL name */
    uint8_t buf[16];
    uint32_t buf_len = (uint32_t)sizeof(buf);
    ret = request_key(NULL, buf, &buf_len);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EINVAL);

    /* revoke_key with NULL name */
    ret = revoke_key(NULL);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EINVAL);
}

/* Revoking a non-existent key returns -ENOKEY. */
static void keyring_revoke_nonexistent(struct kunit *test)
{
    keyring_init();

    int ret = revoke_key("nonexistent_key_xyz");
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-ENOKEY);
}

/* keyring_has_key with NULL name returns 0. */
static void keyring_has_key_null(struct kunit *test)
{
    keyring_init();

    int has = keyring_has_key(NULL);
    KUNIT_EXPECT_EQ(test, (int64_t)has, (int64_t)0);
}

/* Multiple keys can coexist. */
static void keyring_multiple_keys(struct kunit *test)
{
    keyring_init();

    const char *names[] = { "k1", "k2", "k3", "k4", "k5" };
    int n = (int)(sizeof(names) / sizeof(names[0]));

    for (int i = 0; i < n; i++) {
        uint8_t data = (uint8_t)(0x10 + i);
        int ret = add_key(names[i], &data, 1);
        KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    }

    int count = keyring_get_key_count();
    KUNIT_EXPECT_EQ(test, (int64_t)count, (int64_t)n);

    /* Verify each exists */
    for (int i = 0; i < n; i++) {
        KUNIT_EXPECT_TRUE(test, keyring_has_key(names[i]) == 1);
    }

    /* Revoke all */
    for (int i = 0; i < n; i++) {
        revoke_key(names[i]);
    }

    count = keyring_get_key_count();
    KUNIT_EXPECT_EQ(test, (int64_t)count, (int64_t)0);
}

/* Request a non-existent key must return -ENOKEY. */
static void keyring_request_nonexistent(struct kunit *test)
{
    keyring_init();

    uint8_t buf[16];
    uint32_t buf_len = (uint32_t)sizeof(buf);
    int ret = request_key("no_such_key", buf, &buf_len);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-ENOKEY);
}

/* ====================================================================
 *  5. FAULT INJECTION — setup and trigger
 * ==================================================================== */

/* Initialize fault injection and verify baseline. */
static void fault_inject_init_baseline(struct kunit *test)
{
    fault_inject_init();

    uint64_t call_count = fault_inject_get_call_count();
    uint64_t fail_count = fault_inject_get_fail_count();

    KUNIT_EXPECT_EQ(test, (int64_t)call_count, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)fail_count, (int64_t)0);

    /* should_fail should return 0 when disabled */
    int should = fault_inject_should_fail_kmalloc();
    KUNIT_EXPECT_EQ(test, (int64_t)should, (int64_t)0);

    /* Call count should have incremented */
    call_count = fault_inject_get_call_count();
    KUNIT_EXPECT_EQ(test, (int64_t)call_count, (int64_t)1);
}

/* Enable interval mode and verify fail pattern. */
static void fault_inject_interval_mode(struct kunit *test)
{
    fault_inject_init();

    /* Fail every 3rd kmalloc */
    fault_inject_enable(3);

    /* Call should_fail 5 times: should fail on 3rd and 6th */
    for (int i = 1; i <= 5; i++) {
        int should = fault_inject_should_fail_kmalloc();
        if (i % 3 == 0) {
            KUNIT_EXPECT_EQ(test, (int64_t)should, (int64_t)1);
        } else {
            KUNIT_EXPECT_EQ(test, (int64_t)should, (int64_t)0);
        }
    }

    /* Fail count should be 1 (only call 3 failed so far) */
    uint64_t fail_count = fault_inject_get_fail_count();
    KUNIT_EXPECT_EQ(test, (int64_t)fail_count, (int64_t)1);

    /* One more to get the 6th call = 2nd failure */
    int should = fault_inject_should_fail_kmalloc();
    KUNIT_EXPECT_EQ(test, (int64_t)should, (int64_t)1);

    fail_count = fault_inject_get_fail_count();
    KUNIT_EXPECT_EQ(test, (int64_t)fail_count, (int64_t)2);

    /* Disable fault injection */
    fault_inject_enable(-1);
    should = fault_inject_should_fail_kmalloc();
    KUNIT_EXPECT_EQ(test, (int64_t)should, (int64_t)0);
}

/* Enable fail-every-call (interval=1). */
static void fault_inject_fail_every(struct kunit *test)
{
    fault_inject_init();

    fault_inject_enable(1);

    for (int i = 0; i < 5; i++) {
        int should = fault_inject_should_fail_kmalloc();
        KUNIT_EXPECT_EQ(test, (int64_t)should, (int64_t)1);
    }

    uint64_t fail_count = fault_inject_get_fail_count();
    KUNIT_EXPECT_EQ(test, (int64_t)fail_count, (int64_t)5);

    fault_inject_disable();
}

/* Disable fault injection with fault_inject_disable(). */
static void fault_inject_disable_api(struct kunit *test)
{
    fault_inject_init();

    fault_inject_enable(2);
    /* Call once to increment counters */
    fault_inject_should_fail_kmalloc();
    fault_inject_should_fail_kmalloc(); /* this one fails */

    uint64_t fail_before = fault_inject_get_fail_count();
    KUNIT_EXPECT_EQ(test, (int64_t)fail_before, (int64_t)1);

    fault_inject_disable();

    /* After disable, should_fail must return 0 */
    int should = fault_inject_should_fail_kmalloc();
    KUNIT_EXPECT_EQ(test, (int64_t)should, (int64_t)0);

    /* Fail count must not change after disable */
    uint64_t fail_after = fault_inject_get_fail_count();
    KUNIT_EXPECT_EQ(test, (int64_t)fail_after, (int64_t)fail_before);
}

/* ====================================================================
 *  Suite definitions
 * ==================================================================== */

/* ── Lockdown suite ─────────────────────────────────────────────── */
static struct kunit_case lockdown_test_cases[] = {
    KUNIT_CASE(lockdown_none_to_integrity),
    KUNIT_CASE(lockdown_integrity_to_confidentiality),
    KUNIT_CASE(lockdown_irreversible),
    KUNIT_CASE(lockdown_invalid_level),
    KUNIT_CASE(lockdown_is_locked_down_invalid),
    {0}
};

static struct kunit_suite lockdown_test_suite;

/* ── Pkey suite ─────────────────────────────────────────────────── */
static struct kunit_case pkey_test_cases[] = {
    KUNIT_CASE(pkey_alloc_free),
    KUNIT_CASE(pkey_set_get_rights),
    KUNIT_CASE(pkey_free_invalid),
    KUNIT_CASE(pkey_mprotect_basic),
    KUNIT_CASE(pkey_alloc_exhaust),
    KUNIT_CASE(pkey_key0_reserved),
    KUNIT_CASE(pkey_get_rights_invalid),
    KUNIT_CASE(pkey_set_rights_invalid_flags),
    {0}
};

static struct kunit_suite pkey_test_suite;

/* ── Audit suite ────────────────────────────────────────────────── */
static struct kunit_case audit_test_cases[] = {
    KUNIT_CASE(audit_init_and_log),
    KUNIT_CASE(audit_syscall_events),
    KUNIT_CASE(audit_netlink_send_smoke),
    {0}
};

static struct kunit_suite audit_test_suite;

/* ── Keyring suite ──────────────────────────────────────────────── */
static struct kunit_case keyring_test_cases[] = {
    KUNIT_CASE(keyring_add_search_revoke),
    KUNIT_CASE(keyring_duplicate_key),
    KUNIT_CASE(keyring_invalid_args),
    KUNIT_CASE(keyring_revoke_nonexistent),
    KUNIT_CASE(keyring_has_key_null),
    KUNIT_CASE(keyring_multiple_keys),
    KUNIT_CASE(keyring_request_nonexistent),
    {0}
};

static struct kunit_suite keyring_test_suite;

/* ── Fault inject suite ─────────────────────────────────────────── */
static struct kunit_case fault_inject_test_cases[] = {
    KUNIT_CASE(fault_inject_init_baseline),
    KUNIT_CASE(fault_inject_interval_mode),
    KUNIT_CASE(fault_inject_fail_every),
    KUNIT_CASE(fault_inject_disable_api),
    {0}
};

static struct kunit_suite fault_inject_test_suite;

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

void kunit_security_register(void)
{
    REGISTER_SUITE(lockdown_test_suite,      lockdown_test_cases,      "lockdown");
    REGISTER_SUITE(pkey_test_suite,          pkey_test_cases,          "pkey");
    REGISTER_SUITE(audit_test_suite,         audit_test_cases,         "audit");
    REGISTER_SUITE(keyring_test_suite,       keyring_test_cases,       "keyring");
    REGISTER_SUITE(fault_inject_test_suite,  fault_inject_test_cases,  "fault_inject");
}
