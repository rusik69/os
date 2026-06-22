/*
 * test_module_alias.c — Host-side tests for kernel module alias matching.
 *
 * Tests module_alias_init, module_alias_register, module_alias_unregister,
 * module_alias_find from src/kernel/module_alias.c.
 *
 * Compile: part of host_libc test suite (via Makefile)
 * Note: module_alias.o must be compiled with -include spinlock.h to shadow
 * the kernel's spinlock.h (contains privileged instructions).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===================================================================
 *  Kernel API declarations
 * =================================================================== */

extern void module_alias_init(void);
extern int  module_alias_register(const char *pattern, const char *module_name);
extern void module_alias_unregister(const char *module_name);
extern const char *module_alias_find(const char *modalias);

/* Constants from kernel module.h (needed for long-pattern tests) */
#define MODULE_ALIAS_MAX_LEN 128

/* ===================================================================
 *  Stubs for kernel symbols
 * =================================================================== */

void vga_putchar(char c)      { (void)c; }
void serial_putchar(char c)   { (void)c; }
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
int console_loglevel = 7;
int default_message_loglevel = 6;

/* Spinlock stubs — these must match what module_alias.c expects.
 * The spinlock.h header provides these, but we include them here
 * as well for the test file (which is compiled with TEST_CFLAGS,
 * not -include spinlock.h). */
typedef volatile int spinlock_t;
#define SPINLOCK_INIT 0
static inline void spinlock_init(spinlock_t *l)          { *l = 0; }
static inline void spinlock_acquire(spinlock_t *l)       { (void)l; }
static inline void spinlock_release(spinlock_t *l)       { (void)l; }
static inline void spinlock_irqsave_acquire(spinlock_t *l, unsigned long long *f) { (void)l; (void)f; }
static inline void spinlock_irqsave_release(spinlock_t *l, unsigned long long f)  { (void)l; (void)f; }

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
 *  Test: module alias — baseline (14 existing tests)
 * =================================================================== */

static void test_module_alias(void)
{
    printf("\n[module_alias — baseline]\n");

    /* 1. Init clears the table */
    module_alias_init();
    TEST("module_alias: init, find returns NULL",
         module_alias_find("anything") == NULL);

    /* 2. Register + find roundtrip (exact match) */
    module_alias_register("pci:v8086d100F*", "e1000");
    const char *found = module_alias_find("pci:v8086d100F*");
    TEST("module_alias: find exact match",
         found != NULL && strcmp(found, "e1000") == 0);

    /* 3. Non-matching find returns NULL */
    found = module_alias_find("usb:v1234");
    TEST("module_alias: non-matching returns NULL", found == NULL);

    /* 4. Register another alias */
    module_alias_register("usb:v1234d5678*", "usb_storage");
    found = module_alias_find("usb:v1234d5678*");
    TEST("module_alias: find second alias",
         found != NULL && strcmp(found, "usb_storage") == 0);

    /* 5. Unregister and verify removed */
    module_alias_unregister("e1000");
    found = module_alias_find("pci:v8086d100F*");
    TEST("module_alias: unregistered alias returns NULL",
         found == NULL);

    /* 6. Glob exact match (without *) */
    module_alias_register("platform:serial8250", "8250");
    found = module_alias_find("platform:serial8250");
    TEST("module_alias: exact glob match",
         found != NULL && strcmp(found, "8250") == 0);

    /* 7. Glob * suffix match */
    module_alias_register("pci:v8086d7111*", "ide_pci");
    found = module_alias_find("pci:v8086d7111sv00000000");
    TEST("module_alias: glob * suffix match",
         found != NULL && strcmp(found, "ide_pci") == 0);

    /* 8. * matches everything (trailing *) */
    found = module_alias_find("pci:v8086d7111anything");
    TEST("module_alias: * suffix matches anything",
         found != NULL && strcmp(found, "ide_pci") == 0);

    /* 9. Register with empty string (should fail) */
    int ret = module_alias_register("", "empty_module");
    TEST("module_alias: register empty pattern fails", ret != 0);
    ret = module_alias_register("some_pattern", "");
    TEST("module_alias: register empty name fails", ret != 0);

    /* 10. Find with NULL returns NULL */
    found = module_alias_find(NULL);
    TEST("module_alias: find(NULL) returns NULL", found == NULL);

    /* 11. Unregister non-existent module (should not crash) */
    module_alias_unregister("nonexistent");
    TEST("module_alias: unregister non-existent no crash", 1);

    /* 12. Multiple registrations for same module */
    module_alias_register("pci:v8086d1000*", "e1000");
    module_alias_register("pci:v8086d1001*", "e1000");
    found = module_alias_find("pci:v8086d1000*");
    TEST("module_alias: re-register works",
         found != NULL && strcmp(found, "e1000") == 0);
    found = module_alias_find("pci:v8086d1001*");
    TEST("module_alias: second alias for same module",
         found != NULL && strcmp(found, "e1000") == 0);

    /* Clean up baseline */
    module_alias_unregister("e1000");
    module_alias_unregister("usb_storage");
    module_alias_unregister("8250");
    module_alias_unregister("ide_pci");
}

/* ===================================================================
 *  Test: module alias — extended (+10 new assertions)
 * =================================================================== */

static void test_module_alias_extended(void)
{
    printf("\n[module_alias — extended]\n");

    /* Start fresh */
    module_alias_init();

    /* 1. Overwrite: unregister module, register new one with same pattern */
    {
        module_alias_register("pci:vDEAD*", "old_mod");
        const char *f = module_alias_find("pci:vDEADbeef");
        TEST("module_alias: first registration found",
             f != NULL && strcmp(f, "old_mod") == 0);

        module_alias_unregister("old_mod");
        f = module_alias_find("pci:vDEADbeef");
        TEST("module_alias: after unregister, pattern gone", f == NULL);

        module_alias_register("pci:vDEAD*", "new_mod");
        f = module_alias_find("pci:vDEADbeef");
        TEST("module_alias: new module for same pattern found",
             f != NULL && strcmp(f, "new_mod") == 0);

        module_alias_unregister("new_mod");
    }

    /* 2. Wildcard '?' matching — single-char wildcard */
    {
        module_alias_register("pci:????", "four_char");
        const char *f = module_alias_find("pci:abcd");
        TEST("module_alias: '?' wildcard matches exactly one char each",
             f != NULL && strcmp(f, "four_char") == 0);

        f = module_alias_find("pci:abc");
        TEST("module_alias: '?' too short — no match", f == NULL);

        f = module_alias_find("pci:abcde");
        TEST("module_alias: '?' too long — no match", f == NULL);

        module_alias_unregister("four_char");
    }

    /* 3. Wildcard '*' prefix matching */
    {
        module_alias_register("*storage", "storage_drv");
        const char *f = module_alias_find("usb_storage");
        TEST("module_alias: prefix * matches usb_storage",
             f != NULL && strcmp(f, "storage_drv") == 0);

        f = module_alias_find("ata_storage");
        TEST("module_alias: prefix * matches ata_storage",
             f != NULL && strcmp(f, "storage_drv") == 0);

        f = module_alias_find("storage");
        TEST("module_alias: prefix * matches exact suffix",
             f != NULL && strcmp(f, "storage_drv") == 0);

        f = module_alias_find("something_else");
        TEST("module_alias: prefix * non-matching returns NULL", f == NULL);

        module_alias_unregister("storage_drv");
    }

    /* 4. Find with no matches after partial unregister */
    {
        module_alias_register("pci:AAA*", "mod_a");
        module_alias_register("pci:BBB*", "mod_b");
        TEST("module_alias: both registered findable",
             module_alias_find("pci:AAAxxxx") != NULL &&
             module_alias_find("pci:BBBxxxx") != NULL);

        module_alias_unregister("mod_a");
        TEST("module_alias: after unregister A, B still findable",
             module_alias_find("pci:BBBxxxx") != NULL);
        TEST("module_alias: after unregister A, A not findable",
             module_alias_find("pci:AAAxxxx") == NULL);

        module_alias_unregister("mod_b");
    }

    /* 5. Register many aliases (stress at scale) */
    {
        int count = 15;
        int ok = 1;
        for (int i = 0; i < count; i++) {
            char pat[32], mod[32];
            snprintf(pat, sizeof(pat), "alias:%d*", i);
            snprintf(mod, sizeof(mod), "mod_%d", i);
            if (module_alias_register(pat, mod) != 0)
                ok = 0;
        }
        TEST("module_alias: register %d aliases all succeed", ok);

        ok = 1;
        for (int i = 0; i < count; i++) {
            char pat[32], needle[64];
            snprintf(pat, sizeof(pat), "alias:%d*", i);
            snprintf(needle, sizeof(needle), "alias:%dXYZ", i);
            const char *f = module_alias_find(needle);
            if (!f) { ok = 0; break; }
        }
        TEST("module_alias: all %d aliases findable via * suffix", ok);

        /* Clean up */
        for (int i = 0; i < count; i++) {
            char mod[32];
            snprintf(mod, sizeof(mod), "mod_%d", i);
            module_alias_unregister(mod);
        }
    }

    /* 6. Double-unregister (same module twice) */
    {
        module_alias_register("pci:DOUBLE*", "double_mod");
        module_alias_unregister("double_mod");
        module_alias_unregister("double_mod");
        TEST("module_alias: double-unregister no crash", 1);
    }

    /* 7. Empty pattern in find — should return NULL */
    {
        const char *f = module_alias_find("");
        TEST("module_alias: find(empty string) returns NULL", f == NULL);
    }

    /* 8. Very long pattern (near MODULE_ALIAS_MAX_LEN boundary) */
    {
        /* Build a pattern just under the max length */
        char long_pat[MODULE_ALIAS_MAX_LEN];
        memset(long_pat, 'A', sizeof(long_pat) - 2);
        long_pat[0] = 'p';
        long_pat[1] = ':';
        long_pat[sizeof(long_pat) - 2] = '*';
        long_pat[sizeof(long_pat) - 1] = '\0';

        int ret = module_alias_register(long_pat, "long_mod");
        TEST("module_alias: very long pattern registers OK", ret == 0);

        /* Construct a matching input (same length minus '*') */
        char needle[MODULE_ALIAS_MAX_LEN];
        memcpy(needle, long_pat, sizeof(long_pat) - 2);
        needle[sizeof(long_pat) - 2] = '\0';

        const char *f = module_alias_find(needle);
        TEST("module_alias: very long pattern matches", f != NULL && strcmp(f, "long_mod") == 0);

        module_alias_unregister("long_mod");
    }

    /* 9. Pattern with '?' in the middle */
    {
        module_alias_register("usb:v????d????*", "wildcard_usb");
        const char *f = module_alias_find("usb:v1234d5678extra");
        TEST("module_alias: '?' in middle matches",
             f != NULL && strcmp(f, "wildcard_usb") == 0);

        f = module_alias_find("usb:v12d56extra");
        TEST("module_alias: '?' in middle too short — no match", f == NULL);

        module_alias_unregister("wildcard_usb");
    }

    /* 10. '*' in the middle of a pattern */
    {
        module_alias_register("pci:*rev*", "revision_drv");
        const char *f = module_alias_find("pci:v8086d100Frev01");
        TEST("module_alias: '*' in middle matches",
             f != NULL && strcmp(f, "revision_drv") == 0);

        f = module_alias_find("pci:rev");
        TEST("module_alias: '*' matches zero chars too",
             f != NULL && strcmp(f, "revision_drv") == 0);

        f = module_alias_find("pci:XYZrevised");
        TEST("module_alias: '*' in middle matches long prefix then rev",
             f != NULL && strcmp(f, "revision_drv") == 0);

        f = module_alias_find("nopci_at_all");
        TEST("module_alias: prefix doesn't match — NULL",
             f == NULL);

        module_alias_unregister("revision_drv");
    }
}

/* ===================================================================
 *  Main
 * =================================================================== */

int main(void)
{
    printf("=== Kernel Module Alias Tests ===\n");
    test_module_alias();
    test_module_alias_extended();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
