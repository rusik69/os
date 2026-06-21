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
 *  Test: module alias
 * =================================================================== */

static void test_module_alias(void)
{
    printf("\n[module_alias]\n");

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

    /* Clean up */
    module_alias_unregister("e1000");
    module_alias_unregister("usb_storage");
    module_alias_unregister("8250");
    module_alias_unregister("ide_pci");
}

/* ===================================================================
 *  Main
 * =================================================================== */

int main(void)
{
    printf("=== Kernel Module Alias Tests ===\n");
    test_module_alias();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
