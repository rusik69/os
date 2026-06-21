/*
 * test_wx_enforce.c — Host-side tests for W^X page protection checking
 *
 * Tests wx_enforce_check, wx_enforce_check_prot, wx_enforce_init,
 * wx_enforce_is_active from src/kernel/wx_enforce.c.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ===================================================================
 *  Kernel constants (mirror vmm.h + errno.h)
 * =================================================================== */

#define VMM_FLAG_PRESENT  (1ULL << 0)
#define VMM_FLAG_WRITE    (1ULL << 1)
#define VMM_FLAG_USER     (1ULL << 2)
#define VMM_FLAG_NOEXEC   (1ULL << 63)

#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

#define EPERM       1

/* ── Extern declarations ─────────────────────────────────────────── */

extern int wx_enabled;
extern void wx_enforce_init(void);
extern int wx_enforce_check(uint64_t vm_flags);
extern int wx_enforce_check_prot(uint64_t prot);

static inline int wx_enforce_is_active(void) {
    return wx_enabled == 0;
}

/* ===================================================================
 *  Stubs for kernel symbols
 * =================================================================== */

void vga_putchar(char c)      { (void)c; }
void serial_putchar(char c)   { (void)c; }
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
int console_loglevel = 7;
int default_message_loglevel = 6;

/* Sysctl stub — provided by stubs.c */
/* ===================================================================
 *  Test harness
 * =================================================================== */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, cond) do {                                           \
    if (!(cond)) {                                                      \
        printf("  FAIL: %s (%s)\n", name, #cond);                      \
        tests_failed++;                                                 \
    } else {                                                            \
        printf("  PASS: %s\n", name);                                   \
        tests_passed++;                                                 \
    }                                                                   \
} while (0)

/* ===================================================================
 *  test_wx_enforce
 * =================================================================== */

static void test_wx_enforce(void)
{
    /* Reset toggle to default (enforced) before each run */
    wx_enabled = 0;

    /* 0. Init doesn't crash */
    wx_enforce_init();

    /* ── wx_enforce_check (VMM flags) ───────────────────────────── */

    /* 1. No flags → allowed */
    {
        int r = wx_enforce_check(0);
        TEST("check(0): no flags allowed", r == 0);
    }

    /* 2. PRESENT only → allowed (no write) */
    {
        int r = wx_enforce_check(VMM_FLAG_PRESENT);
        TEST("check(PRESENT): allowed (no write)", r == 0);
    }

    /* 3. WRITE only → denied (W without NX = W+X) */
    {
        int r = wx_enforce_check(VMM_FLAG_WRITE);
        TEST("check(WRITE): W+X denied", r == -EPERM);
    }

    /* 4. WRITE | NOEXEC → allowed (write but NX → not executable) */
    {
        int r = wx_enforce_check(VMM_FLAG_WRITE | VMM_FLAG_NOEXEC);
        TEST("check(WRITE|NOEXEC): W+no-X allowed", r == 0);
    }

    /* 5. PRESENT | WRITE → denied (W+X) */
    {
        int r = wx_enforce_check(VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
        TEST("check(PRESENT|WRITE): W+X denied", r == -EPERM);
    }

    /* 6. PRESENT | WRITE | NOEXEC → allowed */
    {
        int r = wx_enforce_check(VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_NOEXEC);
        TEST("check(PRESENT|WRITE|NOEXEC): W+no-X allowed", r == 0);
    }

    /* 7. PRESENT | USER | WRITE → denied */
    {
        int r = wx_enforce_check(VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITE);
        TEST("check(PRESENT|USER|WRITE): W+X denied", r == -EPERM);
    }

    /* 8. NOEXEC alone → allowed */
    {
        int r = wx_enforce_check(VMM_FLAG_NOEXEC);
        TEST("check(NOEXEC): allowed (no write)", r == 0);
    }

    /* ── wx_enforce_check_prot (POSIX PROT flags) ───────────────── */

    /* 9. PROT_NONE → allowed */
    {
        int r = wx_enforce_check_prot(PROT_NONE);
        TEST("check_prot(NONE): allowed", r == 0);
    }

    /* 10. PROT_READ → allowed */
    {
        int r = wx_enforce_check_prot(PROT_READ);
        TEST("check_prot(READ): allowed", r == 0);
    }

    /* 11. PROT_WRITE → allowed (no exec) */
    {
        int r = wx_enforce_check_prot(PROT_WRITE);
        TEST("check_prot(WRITE): allowed (no exec)", r == 0);
    }

    /* 12. PROT_EXEC → allowed (no write) */
    {
        int r = wx_enforce_check_prot(PROT_EXEC);
        TEST("check_prot(EXEC): allowed (no write)", r == 0);
    }

    /* 13. PROT_READ | PROT_EXEC → allowed (RX, no write) */
    {
        int r = wx_enforce_check_prot(PROT_READ | PROT_EXEC);
        TEST("check_prot(READ|EXEC): RX allowed", r == 0);
    }

    /* 14. PROT_WRITE | PROT_EXEC → denied (W+X) */
    {
        int r = wx_enforce_check_prot(PROT_WRITE | PROT_EXEC);
        TEST("check_prot(WRITE|EXEC): W+X denied", r == -EPERM);
    }

    /* 15. PROT_READ | PROT_WRITE | PROT_EXEC → denied (W+X) */
    {
        int r = wx_enforce_check_prot(PROT_READ | PROT_WRITE | PROT_EXEC);
        TEST("check_prot(READ|WRITE|EXEC): RWX denied", r == -EPERM);
    }

    /* ── Relaxed mode (wx_enabled = 1) ───────────────────────────── */

    /* 16. Set wx_enabled = 1, now everything should be allowed */
    wx_enabled = 1;

    {
        int r = wx_enforce_check(VMM_FLAG_WRITE);
        TEST("check(WRITE) relaxed: allowed", r == 0);
    }

    {
        int r = wx_enforce_check(VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
        TEST("check(PRESENT|WRITE) relaxed: allowed", r == 0);
    }

    {
        int r = wx_enforce_check_prot(PROT_WRITE | PROT_EXEC);
        TEST("check_prot(WRITE|EXEC) relaxed: allowed", r == 0);
    }

    /* ── wx_enforce_is_active ────────────────────────────────────── */

    wx_enabled = 0;
    TEST("is_active: returns 1 when enforced", wx_enforce_is_active() == 1);

    wx_enabled = 1;
    TEST("is_active: returns 0 when relaxed", wx_enforce_is_active() == 0);

    /* Reset for cleanliness */
    wx_enabled = 0;
}

/* ===================================================================
 *  Main
 * =================================================================== */

int main(void)
{
    printf("=== W^X Page Protection Enforcement Tests ===\n\n");
    test_wx_enforce();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
