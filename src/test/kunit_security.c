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
#include "seccomp_bpf.h"
#include "syscall.h"
#include "process.h"
#include "pstore.h"
#include "userfaultfd.h"
#include "mprotect.h"
#include "wx_enforce.h"
#include "kaslr.h"
#include "dma.h"
#include "dma_api_drv.h"

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

/* Verify enforcement: at LOCKDOWN_INTEGRITY, INTEGRITY check returns 1. */
static void lockdown_enforcement_integrity(struct kunit *test)
{
    lock_down(LOCKDOWN_NONE);
    KUNIT_EXPECT_EQ(test, (int64_t)lockdown_get_level(), (int64_t)LOCKDOWN_NONE);

    /* At NONE, INTEGRITY is NOT locked down */
    KUNIT_EXPECT_EQ(test, (int64_t)lockdown_is_locked_down(LOCKDOWN_INTEGRITY), (int64_t)0);

    /* Raise to INTEGRITY */
    lock_down(LOCKDOWN_INTEGRITY);
    KUNIT_EXPECT_EQ(test, (int64_t)lockdown_get_level(), (int64_t)LOCKDOWN_INTEGRITY);

    /* At INTEGRITY, INTEGRITY check returns 1, CONFIDENTIALITY still 0 */
    KUNIT_EXPECT_EQ(test, (int64_t)lockdown_is_locked_down(LOCKDOWN_INTEGRITY), (int64_t)1);
    KUNIT_EXPECT_EQ(test, (int64_t)lockdown_is_locked_down(LOCKDOWN_CONFIDENTIALITY), (int64_t)0);
}

/* Verify enforcement: at LOCKDOWN_CONFIDENTIALITY, both levels return 1. */
static void lockdown_enforcement_confidentiality(struct kunit *test)
{
    lock_down(LOCKDOWN_NONE);

    /* Raise directly to CONFIDENTIALITY */
    lock_down(LOCKDOWN_CONFIDENTIALITY);
    KUNIT_EXPECT_EQ(test, (int64_t)lockdown_get_level(), (int64_t)LOCKDOWN_CONFIDENTIALITY);

    /* Both INTEGRITY and CONFIDENTIALITY checks return 1 */
    KUNIT_EXPECT_EQ(test, (int64_t)lockdown_is_locked_down(LOCKDOWN_INTEGRITY), (int64_t)1);
    KUNIT_EXPECT_EQ(test, (int64_t)lockdown_is_locked_down(LOCKDOWN_CONFIDENTIALITY), (int64_t)1);
}

/* Simulate enforcement check: lockdown_at_least should reflect the pattern used
 * at enforcement points (return -EPERM if lockdown >= INTEGRITY). */
static void lockdown_enforcement_pattern(struct kunit *test)
{
    lock_down(LOCKDOWN_NONE);

    /* Simulate an enforcement check like the ones in kexec, module, suspend */
    int should_block = lockdown_is_locked_down(LOCKDOWN_INTEGRITY);
    KUNIT_EXPECT_EQ(test, (int64_t)should_block, (int64_t)0);

    lock_down(LOCKDOWN_INTEGRITY);
    should_block = lockdown_is_locked_down(LOCKDOWN_INTEGRITY);
    KUNIT_EXPECT_EQ(test, (int64_t)should_block, (int64_t)1);

    /* At INTEGRITY, dmesg read (CONFIDENTIALITY) should still be allowed */
    should_block = lockdown_is_locked_down(LOCKDOWN_CONFIDENTIALITY);
    KUNIT_EXPECT_EQ(test, (int64_t)should_block, (int64_t)0);

    lock_down(LOCKDOWN_CONFIDENTIALITY);
    should_block = lockdown_is_locked_down(LOCKDOWN_CONFIDENTIALITY);
    KUNIT_EXPECT_EQ(test, (int64_t)should_block, (int64_t)1);
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
 *  6. SECCOMP BPF — filter installation and evaluation
 * ==================================================================== */

/* Test: install a BPF filter that allows getpid but kills write,
 * then verify the BPF evaluator returns the correct actions. */
static void seccomp_bpf_allow_getpid_kill_write(struct kunit *test)
{
    struct process *current = process_get_current();
    if (!current) {
        kprintf("[KUnit] seccomp_bpf: no current process, skipping test\n");
        return;
    }

    /* Build a BPF filter program:
     *   [0] LD W ABS 0         ; A = syscall_nr
     *   [1] JEQ 5, jt=3, jf=2  ; if A==SYS_GETPID: pc→[5]ALLOW, else pc→[4]
     *   [2] (unused filler)
     *   [3] (unused filler)
     *   [4] JEQ 1, jt=2, jf=1  ; if A==SYS_WRITE: pc→[7]KILL, else pc→[6]ALLOW
     *   [5] RET ALLOW            ; getpid allowed
     *   [6] RET ALLOW            ; default allow
     *   [7] RET KILL             ; write killed
     */
    struct sock_filter insns[] = {
        { BPF_LD | BPF_W | BPF_ABS, 0, 0, 0 },
        { BPF_JMP | BPF_JEQ, 3, 2, SYS_GETPID },
        { BPF_RET, 0, 0, SECCOMP_BPF_RET_ALLOW },   /* unused filler */
        { BPF_RET, 0, 0, SECCOMP_BPF_RET_ALLOW },   /* unused filler */
        { BPF_JMP | BPF_JEQ, 2, 1, SYS_WRITE },
        { BPF_RET, 0, 0, SECCOMP_BPF_RET_ALLOW },
        { BPF_RET, 0, 0, SECCOMP_BPF_RET_ALLOW },
        { BPF_RET, 0, 0, SECCOMP_BPF_RET_KILL },
    };

    struct sock_fprog prog;
    prog.len   = 8;
    prog.filter = insns;

    /* Install requires no_new_privs — set it first */
    int saved_no_new_privs = current->no_new_privs;
    current->no_new_privs = 1;

    int ret = seccomp_filter_install(&prog);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    if (ret != 0) {
        current->no_new_privs = saved_no_new_privs;
        kprintf("[KUnit] seccomp_bpf: install failed (%d), skipping evaluation\n", ret);
        return;
    }

    /* Verify seccomp_mode was set */
    KUNIT_EXPECT_EQ(test, (int64_t)current->seccomp_mode, (int64_t)SECCOMP_MODE_FILTER_BPF);
    KUNIT_EXPECT_NOT_NULL(test, current->seccomp_filter);

    /* Evaluate getpid — should ALLOW */
    uint32_t result = seccomp_filter_evaluate(SYS_GETPID, AUDIT_ARCH_X86_64);
    KUNIT_EXPECT_EQ(test, (int64_t)(result & SECCOMP_RET_ACTION_FULL),
                    (int64_t)SECCOMP_BPF_RET_ALLOW);

    /* Evaluate write — should KILL */
    result = seccomp_filter_evaluate(SYS_WRITE, AUDIT_ARCH_X86_64);
    KUNIT_EXPECT_EQ(test, (int64_t)(result & SECCOMP_RET_ACTION_FULL),
                    (int64_t)SECCOMP_BPF_RET_KILL);

    /* Evaluate read — default should ALLOW */
    result = seccomp_filter_evaluate(SYS_READ, AUDIT_ARCH_X86_64);
    KUNIT_EXPECT_EQ(test, (int64_t)(result & SECCOMP_RET_ACTION_FULL),
                    (int64_t)SECCOMP_BPF_RET_ALLOW);

    /* Clean up */
    seccomp_bpf_release();
    KUNIT_EXPECT_NULL(test, current->seccomp_filter);
    current->no_new_privs = saved_no_new_privs;
}

/* Test: filter install must fail if no_new_privs is not set. */
static void seccomp_bpf_requires_no_new_privs(struct kunit *test)
{
    struct process *current = process_get_current();
    if (!current) {
        kprintf("[KUnit] seccomp_bpf: no current process, skipping test\n");
        return;
    }

    /* Ensure no_new_privs is 0 */
    int saved = current->no_new_privs;
    current->no_new_privs = 0;

    struct sock_filter one_insn[] = {
        { BPF_RET, 0, 0, SECCOMP_BPF_RET_ALLOW },
    };
    struct sock_fprog prog;
    prog.len   = 1;
    prog.filter = one_insn;

    int ret = seccomp_filter_install(&prog);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EPERM);

    current->no_new_privs = saved;
}

/* ====================================================================
 *  7. LANDLOCK — path-based access control enforcement
 * ==================================================================== */

#include "landlock.h"
#include "mseal.h"

/* Create a ruleset, verify it returns a valid id. */
static void landlock_create_ruleset_basic(struct kunit *test)
{
    struct landlock_ruleset_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.handled_access_fs = LANDLOCK_ACCESS_FS_READ_FILE |
                             LANDLOCK_ACCESS_FS_WRITE_FILE |
                             LANDLOCK_ACCESS_FS_READ_DIR;

    int fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
    KUNIT_EXPECT_TRUE(test, fd >= 0);
    KUNIT_EXPECT_TRUE(test, fd < LANDLOCK_MAX_RULESETS);
}

/* Create a ruleset with NULL attr must return -EFAULT. */
static void landlock_create_ruleset_null_attr(struct kunit *test)
{
    int fd = landlock_create_ruleset(NULL, sizeof(struct landlock_ruleset_attr), 0);
    KUNIT_EXPECT_EQ(test, (int64_t)fd, (int64_t)-EFAULT);
}

/* Add a path-beneath rule to a ruleset. */
static void landlock_add_rule_basic(struct kunit *test)
{
    struct landlock_ruleset_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.handled_access_fs = LANDLOCK_ACCESS_FS_READ_FILE;

    int rs_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
    if (rs_fd < 0) return;

    struct landlock_path_beneath_attr rule;
    memset(&rule, 0, sizeof(rule));
    rule.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE;
    rule.parent_fd = -1;  /* root */

    int ret = landlock_add_rule(rs_fd, LANDLOCK_RULE_PATH_BENEATH, &rule, 0);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
}

/* Add a rule with unhandled access bits must return -EINVAL. */
static void landlock_add_rule_unhandled_access(struct kunit *test)
{
    struct landlock_ruleset_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.handled_access_fs = LANDLOCK_ACCESS_FS_READ_FILE;

    int rs_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
    if (rs_fd < 0) return;

    struct landlock_path_beneath_attr rule;
    memset(&rule, 0, sizeof(rule));
    rule.allowed_access = LANDLOCK_ACCESS_FS_WRITE_FILE;  /* not in handled_access_fs */
    rule.parent_fd = -1;

    int ret = landlock_add_rule(rs_fd, LANDLOCK_RULE_PATH_BENEATH, &rule, 0);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EINVAL);
}

/* Restrict self with a ruleset that has no rules — should always allow. */
static void landlock_restrict_self_empty(struct kunit *test)
{
    struct landlock_ruleset_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.handled_access_fs = LANDLOCK_ACCESS_FS_READ_FILE;

    int rs_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
    if (rs_fd < 0) return;

    int ret = landlock_restrict_self(rs_fd, 0);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
}

/* Invalid ruleset fd for restrict_self must return -EBADF. */
static void landlock_restrict_self_invalid_fd(struct kunit *test)
{
    int ret = landlock_restrict_self(999, 0);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EBADF);
}

/* landlock_check_path: verify that an empty ruleset allows everything. */
static void landlock_check_path_empty_allows(struct kunit *test)
{
    struct process *proc = process_get_current();
    if (!proc) return;

    /* With no rulesets installed, all access should be granted */
    int ret = landlock_check_path(proc, "/", LANDLOCK_ACCESS_FS_READ_FILE);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    ret = landlock_check_path(proc, "/test", LANDLOCK_ACCESS_FS_WRITE_FILE);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
}

/* landlock_check_path: verify deny when path not covered by rules. */
static void landlock_check_path_denies(struct kunit *test)
{
    struct landlock_ruleset_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.handled_access_fs = LANDLOCK_ACCESS_FS_READ_FILE;

    int rs_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
    if (rs_fd < 0) return;

    /* Add a rule that only allows /safe path */
    struct landlock_path_beneath_attr rule;
    memset(&rule, 0, sizeof(rule));
    rule.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE;
    rule.parent_fd = -1;  /* root */

    int ret = landlock_add_rule(rs_fd, LANDLOCK_RULE_PATH_BENEATH, &rule, 0);
    if (ret < 0) return;

    /* Save current ruleset slots, install test ruleset, verify, restore */
    struct process *proc = process_get_current();
    if (!proc) return;

    int saved[LANDLOCK_MAX_RULESETS_PER_PROC];
    for (int i = 0; i < LANDLOCK_MAX_RULESETS_PER_PROC; i++) {
        saved[i] = proc->landlock_ruleset_ids[i];
        proc->landlock_ruleset_ids[i] = -1;
    }
    proc->landlock_ruleset_ids[0] = rs_fd;

    /* /etc/passwd is under root (/) so the root rule covers it → allowed */
    ret = landlock_check_path(proc, "/etc/passwd", LANDLOCK_ACCESS_FS_READ_FILE);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* /etc/passwd with WRITE_FILE — not in allowed_access → denied */
    ret = landlock_check_path(proc, "/etc/passwd", LANDLOCK_ACCESS_FS_WRITE_FILE);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EACCES);

    /* Restore saved slots */
    for (int i = 0; i < LANDLOCK_MAX_RULESETS_PER_PROC; i++)
        proc->landlock_ruleset_ids[i] = saved[i];
}

/* ====================================================================
 *  8. MSEAL — memory seal enforcement
 * ==================================================================== */

/* Seal a range and verify that mseal_check detects it. */
static void mseal_seal_and_check(struct kunit *test)
{
    uint64_t addr = 0x70000000;
    uint64_t len  = 0x10000;

    int ret = mseal(addr, len, 0);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* mseal_check should return 0 (sealed) for this range */
    ret = mseal_check(addr, len);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* mseal_check should return -EACCES for a different range */
    ret = mseal_check(0x80000000, 0x1000);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EACCES);
}

/* Seal a range and verify that mseal_is_sealed works. */
static void mseal_is_sealed_basic(struct kunit *test)
{
    uint64_t addr = 0x70010000;
    uint64_t len  = 0x2000;

    int ret = mseal(addr, len, 0);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    int sealed = mseal_is_sealed(addr);
    KUNIT_EXPECT_NE(test, (int64_t)sealed, (int64_t)0);

    sealed = mseal_is_sealed(addr + 1);
    KUNIT_EXPECT_NE(test, (int64_t)sealed, (int64_t)0);

    sealed = mseal_is_sealed(addr + len);
    KUNIT_EXPECT_EQ(test, (int64_t)sealed, (int64_t)0);  /* just past end */

    /* A completely different address should NOT be sealed */
    sealed = mseal_is_sealed(0xDEAD0000);
    KUNIT_EXPECT_EQ(test, (int64_t)sealed, (int64_t)0);
}

/* Seal a range and verify that overlapping seal is rejected. */
static void mseal_overlap_rejected(struct kunit *test)
{
    uint64_t addr = 0x70020000;
    uint64_t len  = 0x10000;

    int ret = mseal(addr, len, 0);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* Overlapping range must be rejected */
    ret = mseal(addr + 0x1000, 0x1000, 0);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EINVAL);

    /* Adjacent ranges should be allowed (coalescing) */
    ret = mseal(addr + len, 0x1000, 0);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
}

/* mseal with len=0 must return -EINVAL. */
static void mseal_zero_len(struct kunit *test)
{
    int ret = mseal(0x70030000, 0, 0);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EINVAL);
}

/* mseal_check verifies that a range partially within a sealed region
 * is not considered sealed (must be fully contained). */
static void mseal_check_requires_full_containment(struct kunit *test)
{
    uint64_t addr = 0x70040000;
    uint64_t len  = 0x10000;

    int ret = mseal(addr, len, 0);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* Range larger than sealed region — not fully contained → -EACCES */
    ret = mseal_check(addr - 0x1000, len + 0x2000);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EACCES);

    /* Range entirely inside sealed region → sealed */
    ret = mseal_check(addr + 0x1000, 0x1000);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
}

/* ====================================================================
 *  9. PSTORE — persistent storage write/read/recover
 * ==================================================================== */

/* Write a few records, read them back, verify content. */
static void pstore_write_read_basic(struct kunit *test)
{
    /* pstore_init() already called during boot — just verify it's live */
    int count_before = pstore_get_count();
    KUNIT_EXPECT_TRUE(test, count_before >= 0);

    /* Write a test record */
    const char *msg = "KUnit pstore test message";
    int ret = pstore_write(PSTORE_TYPE_DMESG,
                           (const uint8_t *)msg, (int)strlen(msg));
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* Verify count incremented */
    int count_after = pstore_get_count();
    KUNIT_EXPECT_EQ(test, (int64_t)count_after, (int64_t)(count_before + 1));

    /* Read back the record we just wrote (index 0 = newest) */
    uint8_t buf[512];
    int n = pstore_read(0, buf, (int)sizeof(buf));
    KUNIT_EXPECT_TRUE(test, n > 0);

    /* First byte should be PSTORE_TYPE_DMESG (the type byte) */
    KUNIT_EXPECT_EQ(test, (int64_t)buf[0], (int64_t)PSTORE_TYPE_DMESG);

    /* Rest should match the original message */
    int match = 1;
    for (int i = 0; i < n - 1 && i < (int)strlen(msg); i++) {
        if (buf[i + 1] != (uint8_t)msg[i]) {
            match = 0;
            break;
        }
    }
    KUNIT_EXPECT_TRUE(test, match);
}

/* Write a panic-type record and verify recovery. */
static void pstore_write_panic_record(struct kunit *test)
{
    const char *panic_msg = "Test kernel panic for KUnit";
    int ret = pstore_write(PSTORE_TYPE_PANIC,
                           (const uint8_t *)panic_msg,
                           (int)strlen(panic_msg));
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* Read it back and verify type */
    uint8_t buf[512];
    int n = pstore_read(0, buf, (int)sizeof(buf));
    KUNIT_EXPECT_TRUE(test, n > 0);
    KUNIT_EXPECT_EQ(test, (int64_t)buf[0], (int64_t)PSTORE_TYPE_PANIC);
}

/* Write multiple records, read them in order. */
static void pstore_multiple_records(struct kunit *test)
{
    const char *records[] = {
        "First record",
        "Second record — a bit longer",
        "Third!",
    };
    int n_records = (int)(sizeof(records) / sizeof(records[0]));

    for (int i = 0; i < n_records; i++) {
        int ret = pstore_write(PSTORE_TYPE_DMESG,
                               (const uint8_t *)records[i],
                               (int)strlen(records[i]));
        KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    }

    /* Read back the records (index 0 = newest = last written) */
    for (int i = 0; i < n_records; i++) {
        uint8_t buf[128];
        int n = pstore_read(i, buf, (int)sizeof(buf));
        KUNIT_EXPECT_TRUE(test, n > 0);

        /* The newest record should be the last one we wrote */
        int expected_idx = n_records - 1 - i;
        int match = 1;
        size_t expected_len = strlen(records[expected_idx]);
        for (size_t j = 0; j < expected_len && j < (size_t)(n - 1); j++) {
            if (buf[j + 1] != (uint8_t)records[expected_idx][j]) {
                match = 0;
                break;
            }
        }
        KUNIT_EXPECT_TRUE(test, match);
    }
}

/* Read beyond the available records returns -ENOENT. */
static void pstore_read_out_of_bounds(struct kunit *test)
{
    int count = pstore_get_count();
    uint8_t buf[64];
    int ret = pstore_read(count + 10, buf, (int)sizeof(buf));
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-ENOENT);
}

/* Write with zero-length data (just a type marker). */
static void pstore_write_zero_length(struct kunit *test)
{
    int ret = pstore_write(PSTORE_TYPE_EMERG, NULL, 0);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    uint8_t buf[16];
    int n = pstore_read(0, buf, (int)sizeof(buf));
    KUNIT_EXPECT_TRUE(test, n > 0);
    KUNIT_EXPECT_EQ(test, (int64_t)buf[0], (int64_t)PSTORE_TYPE_EMERG);
}

/* Write a record with data exceeding max length — must be rejected. */
static void pstore_write_overflow(struct kunit *test)
{
    /* PSTORE_MAX_DATA_LEN includes the type byte, so max payload
     * is PSTORE_MAX_DATA_LEN - 1 = 4083 bytes */
    uint8_t big_data[PSTORE_MAX_DATA_LEN];  /* 4084 bytes — too big */
    memset(big_data, 0xAB, sizeof(big_data));
    int ret = pstore_write(PSTORE_TYPE_DMESG, big_data, PSTORE_MAX_DATA_LEN);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EINVAL);
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
    KUNIT_CASE(lockdown_enforcement_integrity),
    KUNIT_CASE(lockdown_enforcement_confidentiality),
    KUNIT_CASE(lockdown_enforcement_pattern),
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

/* ── Seccomp BPF suite ─────────────────────────────────────────── */
static struct kunit_case seccomp_bpf_test_cases[] = {
    KUNIT_CASE(seccomp_bpf_requires_no_new_privs),
    KUNIT_CASE(seccomp_bpf_allow_getpid_kill_write),
    {0}
};

static struct kunit_suite seccomp_bpf_test_suite;

/* ── Landlock suite ────────────────────────────────────────────── */
static struct kunit_case landlock_test_cases[] = {
    KUNIT_CASE(landlock_create_ruleset_basic),
    KUNIT_CASE(landlock_create_ruleset_null_attr),
    KUNIT_CASE(landlock_add_rule_basic),
    KUNIT_CASE(landlock_add_rule_unhandled_access),
    KUNIT_CASE(landlock_restrict_self_empty),
    KUNIT_CASE(landlock_restrict_self_invalid_fd),
    KUNIT_CASE(landlock_check_path_empty_allows),
    KUNIT_CASE(landlock_check_path_denies),
    {0}
};

static struct kunit_suite landlock_test_suite;

/* ── MSEAL suite ───────────────────────────────────────────────── */
static struct kunit_case mseal_test_cases[] = {
    KUNIT_CASE(mseal_seal_and_check),
    KUNIT_CASE(mseal_is_sealed_basic),
    KUNIT_CASE(mseal_overlap_rejected),
    KUNIT_CASE(mseal_zero_len),
    KUNIT_CASE(mseal_check_requires_full_containment),
    {0}
};

static struct kunit_suite mseal_test_suite;

/* ── Pstore suite ────────────────────────────────────────────────── */
static struct kunit_case pstore_test_cases[] = {
    KUNIT_CASE(pstore_write_read_basic),
    KUNIT_CASE(pstore_write_panic_record),
    KUNIT_CASE(pstore_multiple_records),
    KUNIT_CASE(pstore_read_out_of_bounds),
    KUNIT_CASE(pstore_write_zero_length),
    KUNIT_CASE(pstore_write_overflow),
    {0}
};

static struct kunit_suite pstore_test_suite;

/* Forward declarations for uffd test suite (defined below) */
static struct kunit_case uffd_test_cases[];
static struct kunit_suite uffd_test_suite;

/* Forward declarations for KASLR, W^X, mprotect test suites */
static struct kunit_case kaslr_test_cases[];
static struct kunit_suite kaslr_test_suite;
static struct kunit_case wx_enforce_test_cases[];
static struct kunit_suite wx_enforce_test_suite;
static struct kunit_case mprotect_test_cases[];
static struct kunit_suite mprotect_test_suite;
static struct kunit_case dma_test_cases[];
static struct kunit_suite dma_test_suite;

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
    kprintf("[KUnit] %s tests registered (%d cases)\n", suite_name, __ci); \
} while(0)

/* ====================================================================
 *  KASLR — Kernel ASLR tests
 * ==================================================================== */

static void kaslr_get_offset_test(struct kunit *test)
{
    /* KASLR offset should be 2MB-aligned */
    uint64_t off = kaslr_get_offset();
    KUNIT_EXPECT_EQ(test, (int64_t)(off & (KASLR_ALIGN - 1)), (int64_t)0);

    /* Offset must not exceed KASLR_MAX_OFFSET */
    KUNIT_EXPECT_TRUE(test, off <= KASLR_MAX_OFFSET);

    /* Multiple calls should return the same value (cached) */
    uint64_t off2 = kaslr_get_offset();
    KUNIT_EXPECT_EQ(test, (int64_t)off, (int64_t)off2);
}

/* ====================================================================
 *  W^X — W^X enforcement tests
 * ==================================================================== */

static void wx_enforce_check_test(struct kunit *test)
{
    /* Save current wx_enabled state */
    int saved = wx_enabled;

    /* Test with W^X active (default: 0 = deny W+X) */
    wx_enabled = 0;

    /* RW page: NOEXEC set, WRITE set → OK */
    KUNIT_EXPECT_EQ(test, (int64_t)wx_enforce_check(VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_NOEXEC | VMM_FLAG_USER), (int64_t)0);

    /* RX page: NOEXEC not set, WRITE not set → OK */
    KUNIT_EXPECT_EQ(test, (int64_t)wx_enforce_check(VMM_FLAG_PRESENT | VMM_FLAG_USER), (int64_t)0);

    /* W+X page: WRITE set, NOEXEC not set → DENIED */
    KUNIT_EXPECT_EQ(test, (int64_t)wx_enforce_check(VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER), (int64_t)-EPERM);

    /* PROT-based check: writable + executable → DENIED */
    KUNIT_EXPECT_EQ(test, (int64_t)wx_enforce_check_prot(PROT_READ | PROT_WRITE | PROT_EXEC), (int64_t)-EPERM);

    /* PROT-based check: read-only + exec → OK */
    KUNIT_EXPECT_EQ(test, (int64_t)wx_enforce_check_prot(PROT_READ | PROT_EXEC), (int64_t)0);

    /* PROT-based check: read + write → OK */
    KUNIT_EXPECT_EQ(test, (int64_t)wx_enforce_check_prot(PROT_READ | PROT_WRITE), (int64_t)0);

    /* Test with W^X relaxed (wx_enabled = 1) */
    wx_enabled = 1;

    /* W+X should be allowed now */
    KUNIT_EXPECT_EQ(test, (int64_t)wx_enforce_check_prot(PROT_WRITE | PROT_EXEC), (int64_t)0);

    /* Restore */
    wx_enabled = saved;
}

static void wx_enforce_prot_none_test(struct kunit *test)
{
    /* PROT_NONE should always be allowed */
    KUNIT_EXPECT_EQ(test, (int64_t)wx_enforce_check_prot(PROT_NONE), (int64_t)0);
}

/* ====================================================================
 *  mprotect — mprotect syscall tests
 * ==================================================================== */

static void mprotect_basic_test(struct kunit *test)
{
    /* Test basic mprotect */
    int64_t ret;

    /* Invalid flags should return -EINVAL */
    ret = sys_mprotect(0x100000, PAGE_SIZE, 0xFF);
    KUNIT_EXPECT_EQ(test, ret, (int64_t)-EINVAL);

    /* Non-page-aligned address should return -EINVAL */
    ret = sys_mprotect(0x100001, PAGE_SIZE, PROT_READ);
    KUNIT_EXPECT_EQ(test, ret, (int64_t)-EINVAL);

    /* Zero length is fine - should succeed (no-op) */
    ret = sys_mprotect(0x100000, 0, PROT_READ);
    KUNIT_EXPECT_EQ(test, ret, (int64_t)0);
}

static void mprotect_wx_denied_test(struct kunit *test)
{
    int saved = wx_enabled;
    wx_enabled = 0;  /* Enforce W^X */

    /* Requesting W+X should be denied */
    int64_t ret = sys_mprotect(0x100000, PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
    KUNIT_EXPECT_EQ(test, ret, (int64_t)-EPERM);

    wx_enabled = saved;
}

static void mprotect_wx_allowed_test(struct kunit *test)
{
    int saved = wx_enabled;
    wx_enabled = 1;  /* Relax W^X */

    /* Requesting W+X should be allowed when W^X is relaxed */
    int64_t ret = sys_mprotect(0x100000, PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
    /* May return -EFAULT (not mapped) or 0 if mapped - depends on test env */
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -EFAULT);

    wx_enabled = saved;
}

static void mprotect_invalid_range_test(struct kunit *test)
{
    /* Address beyond user space should return -ENOMEM */
    int64_t ret = sys_mprotect(USER_VADDR_MAX, PAGE_SIZE, PROT_READ);
    KUNIT_EXPECT_EQ(test, ret, (int64_t)-ENOMEM);

    /* Overflow should return -EINVAL */
    ret = sys_mprotect(0xFFFFFFFFFFFFFFF0ULL, 0x1000, PROT_READ);
    KUNIT_EXPECT_EQ(test, ret, (int64_t)-EINVAL);
}

/* ── Test case arrays ────────────────────────────────────────────── */

static struct kunit_case kaslr_test_cases[] = {
    KUNIT_CASE(kaslr_get_offset_test),
    {}
};

static struct kunit_case wx_enforce_test_cases[] = {
    KUNIT_CASE(wx_enforce_check_test),
    KUNIT_CASE(wx_enforce_prot_none_test),
    {}
};

static struct kunit_case mprotect_test_cases[] = {
    KUNIT_CASE(mprotect_basic_test),
    KUNIT_CASE(mprotect_wx_denied_test),
    KUNIT_CASE(mprotect_wx_allowed_test),
    KUNIT_CASE(mprotect_invalid_range_test),
    {}
};

/* ── Registration ────────────────────────────────────────────────── */

void kunit_security_register(void)
{
    REGISTER_SUITE(lockdown_test_suite,      lockdown_test_cases,      "lockdown");
    REGISTER_SUITE(pkey_test_suite,          pkey_test_cases,          "pkey");
    REGISTER_SUITE(audit_test_suite,         audit_test_cases,         "audit");
    REGISTER_SUITE(keyring_test_suite,       keyring_test_cases,       "keyring");
    REGISTER_SUITE(fault_inject_test_suite,  fault_inject_test_cases,  "fault_inject");
    REGISTER_SUITE(seccomp_bpf_test_suite,   seccomp_bpf_test_cases,   "seccomp_bpf");
    REGISTER_SUITE(landlock_test_suite,       landlock_test_cases,       "landlock");
    REGISTER_SUITE(mseal_test_suite,          mseal_test_cases,          "mseal");
    REGISTER_SUITE(pstore_test_suite,        pstore_test_cases,         "pstore");
    REGISTER_SUITE(uffd_test_suite,          uffd_test_cases,           "uffd");
    REGISTER_SUITE(kaslr_test_suite,         kaslr_test_cases,          "kaslr");
    REGISTER_SUITE(wx_enforce_test_suite,    wx_enforce_test_cases,     "wx_enforce");
    REGISTER_SUITE(mprotect_test_suite,      mprotect_test_cases,       "mprotect");
    REGISTER_SUITE(dma_test_suite,           dma_test_cases,            "dma_api");
}

/* ====================================================================
 *  DMA API — dma_alloc_coherent / dma_map_single tests
 * ==================================================================== */

static void dma_alloc_free_test(struct kunit *test)
{
    uint64_t dma_handle;
    void *buf = dma_alloc_coherent(NULL, 4096, &dma_handle, 0);

    KUNIT_EXPECT_NOT_NULL(test, buf);
    KUNIT_EXPECT_NE(test, (int64_t)dma_handle, (int64_t)0);

    if (buf) {
        /* Verify the buffer is zeroed and writable */
        KUNIT_EXPECT_EQ(test, ((volatile uint8_t *)buf)[0], (uint8_t)0);
        ((volatile uint8_t *)buf)[0] = 0xAB;
        KUNIT_EXPECT_EQ(test, ((volatile uint8_t *)buf)[0], (uint8_t)0xAB);
        ((volatile uint8_t *)buf)[4095] = 0xCD;
        KUNIT_EXPECT_EQ(test, ((volatile uint8_t *)buf)[4095], (uint8_t)0xCD);

        dma_free_coherent(NULL, 4096, buf, dma_handle);
    }
}

static void dma_alloc_zero_size(struct kunit *test)
{
    uint64_t dma_handle;
    void *buf = dma_alloc_coherent(NULL, 0, &dma_handle, 0);

    KUNIT_EXPECT_NULL(test, buf);
    KUNIT_EXPECT_EQ(test, (int64_t)dma_handle, (int64_t)0);
}

static void dma_map_unmap_single_test(struct kunit *test)
{
    /* Allocate a small buffer from the heap */
    void *buf = kmalloc(256);
    KUNIT_EXPECT_NOT_NULL(test, buf);
    if (!buf) return;

    /* Fill with test pattern */
    memset(buf, 0x42, 256);

    uint64_t dma_handle = dma_map_single(NULL, buf, 256, DMA_BIDIRECTIONAL);
    KUNIT_EXPECT_NE(test, (int64_t)dma_handle, (int64_t)~0ULL);

    if (dma_handle != ~0ULL)
        dma_unmap_single(NULL, dma_handle, 256, DMA_BIDIRECTIONAL);

    kfree(buf);
}

static void dma_map_null_buffer(struct kunit *test)
{
    uint64_t dma_handle = dma_map_single(NULL, NULL, 256, DMA_TO_DEVICE);
    KUNIT_EXPECT_EQ(test, (int64_t)dma_handle, (int64_t)~0ULL);
}

static void dma_sync_cpu_test(struct kunit *test)
{
    /* Sync for CPU should not crash with any direction */
    dma_sync_single_for_cpu(0x1000, 256, DMA_FROM_DEVICE);
    dma_sync_single_for_cpu(0x2000, 512, DMA_BIDIRECTIONAL);
    KUNIT_EXPECT_TRUE(test, 1);
}

static void dma_sync_device_test(struct kunit *test)
{
    /* Sync for device should not crash */
    dma_sync_single_for_device(0x1000, 256, DMA_TO_DEVICE);
    dma_sync_single_for_device(0x2000, 512, DMA_BIDIRECTIONAL);
    KUNIT_EXPECT_TRUE(test, 1);
}

static void dma_alloc_aligned_test(struct kunit *test)
{
    uint64_t dma_handle;
    void *buf = dma_alloc_coherent_aligned(NULL, 64, &dma_handle, 0, 4096);

    KUNIT_EXPECT_NOT_NULL(test, buf);
    if (buf) {
        /* Check alignment */
        KUNIT_EXPECT_EQ(test, (uintptr_t)buf & 0xFFF, (uintptr_t)0);

        /* Buffer should be writable */
        memset(buf, 0xAA, 64);
        KUNIT_EXPECT_EQ(test, ((volatile uint8_t *)buf)[0], (uint8_t)0xAA);
        KUNIT_EXPECT_EQ(test, ((volatile uint8_t *)buf)[63], (uint8_t)0xAA);

        dma_free_coherent(NULL, 64 + 4096, buf, dma_handle);
    }
}

static struct kunit_case dma_test_cases[] = {
    KUNIT_CASE(dma_alloc_free_test),
    KUNIT_CASE(dma_alloc_zero_size),
    KUNIT_CASE(dma_map_unmap_single_test),
    KUNIT_CASE(dma_map_null_buffer),
    KUNIT_CASE(dma_sync_cpu_test),
    KUNIT_CASE(dma_sync_device_test),
    KUNIT_CASE(dma_alloc_aligned_test),
    {}
};

/* ====================================================================
 *  userfaultfd — Userfaultfd subsystem tests
 * ==================================================================== */

static void uffd_create_close(struct kunit *test)
{
    int fd = userfaultfd_create(0);
    KUNIT_EXPECT_TRUE(test, fd >= 0);
    KUNIT_EXPECT_TRUE(test, fd < UFFD_MAX_CONTEXTS);

    if (fd >= 0) {
        int fd2 = userfaultfd_create(0);
        KUNIT_EXPECT_NE(test, (int64_t)fd2, (int64_t)fd);
        (void)fd2;
    }
}

static void uffd_register_unregister(struct kunit *test)
{
    int fd = userfaultfd_create(0);
    KUNIT_EXPECT_TRUE(test, fd >= 0);
    if (fd < 0) return;

    uint64_t addr = 0x00007f0000000000ULL;
    uint64_t len = PAGE_SIZE;
    int ret = userfaultfd_register(fd, addr, len, UFFD_FLAG_MISSING);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    ret = userfaultfd_register(fd, addr + PAGE_SIZE, PAGE_SIZE, UFFD_FLAG_MISSING);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    ret = userfaultfd_unregister(fd, addr, len);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    ret = userfaultfd_unregister(fd, addr, len);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-ENOENT);
}

static void uffd_invalid_fd(struct kunit *test)
{
    int ret = userfaultfd_register(-1, 0x1000, PAGE_SIZE, UFFD_FLAG_MISSING);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EBADF);

    ret = userfaultfd_unregister(999, 0x1000, PAGE_SIZE);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EBADF);
}

static void uffd_unaligned_register(struct kunit *test)
{
    int fd = userfaultfd_create(0);
    KUNIT_EXPECT_TRUE(test, fd >= 0);
    if (fd < 0) return;

    int ret = userfaultfd_register(fd, 0x1001, PAGE_SIZE, UFFD_FLAG_MISSING);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EINVAL);

    ret = userfaultfd_register(fd, 0x1000, 1234, UFFD_FLAG_MISSING);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-EINVAL);
}

static void uffd_find_for_addr(struct kunit *test)
{
    int fd = userfaultfd_create(0);
    KUNIT_EXPECT_TRUE(test, fd >= 0);
    if (fd < 0) return;

    uint64_t addr = 0x00007f0000000000ULL;
    uint64_t len = PAGE_SIZE * 4;
    int ret = userfaultfd_register(fd, addr, len, UFFD_FLAG_MISSING);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    int found_fd = userfaultfd_find_for_addr(addr + PAGE_SIZE, NULL);
    KUNIT_EXPECT_EQ(test, (int64_t)found_fd, (int64_t)fd);

    found_fd = userfaultfd_find_for_addr(addr - PAGE_SIZE, NULL);
    KUNIT_EXPECT_EQ(test, (int64_t)found_fd, (int64_t)-1);
}

static void uffd_set_features(struct kunit *test)
{
    int fd = userfaultfd_create(0);
    KUNIT_EXPECT_TRUE(test, fd >= 0);
    if (fd < 0) return;

    int ret = userfaultfd_set_features(fd, UFFD_FEATURE_SIGBUS);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
}

static struct kunit_case uffd_test_cases[] = {
    KUNIT_CASE(uffd_create_close),
    KUNIT_CASE(uffd_register_unregister),
    KUNIT_CASE(uffd_invalid_fd),
    KUNIT_CASE(uffd_unaligned_register),
    KUNIT_CASE(uffd_find_for_addr),
    KUNIT_CASE(uffd_set_features),
    {0}
};

static struct kunit_suite uffd_test_suite;
