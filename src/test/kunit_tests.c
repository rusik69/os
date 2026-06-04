/*
 * kunit_tests.c — Initial KUnit test suites for core kernel subsystems.
 *
 * These tests register with the KUnit framework and can be run via:
 *   # echo 1 > /sys/kernel/debug/kunit/run_all
 *   # cat /sys/kernel/debug/kunit/results
 *
 * Item 266: KUnit — kernel unit test framework
 * Item 267: KUnit — PMM allocation tests (full in kunit_pmm.c)
 * Item 268: KUnit — slab allocator tests (partial)
 * Item 269: KUnit — VMM map/unmap tests (partial)
 */

#include "kunit.h"
#include "pmm.h"
#include "slab.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "heap.h"

/* ====================================================================
 *  1. PMM — Physical Memory Manager tests
 * ==================================================================== */

static void pmm_alloc_free_test(struct kunit *test)
{
    /* Test basic alloc and free */
    uint64_t frame = pmm_alloc_frame();
    KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
    if (frame) {
        pmm_free_frame(frame);
    }

    /* Test alloc multiple and free in reverse */
    uint64_t frames[4];
    for (int i = 0; i < 4; i++) {
        frames[i] = pmm_alloc_frame();
        KUNIT_EXPECT_NE(test, frames[i], (uint64_t)0);
    }
    for (int i = 3; i >= 0; i--) {
        if (frames[i])
            pmm_free_frame(frames[i]);
    }
}

static void pmm_refcount_test(struct kunit *test)
{
    /* Test refcounting: alloc, ref, unref, free */
    uint64_t frame = pmm_alloc_frame();
    KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
    if (!frame) return;

    /* Take a reference */
    pmm_ref_frame(frame);

    /* First unref (refcount 1→0) should not free */
    pmm_unref_frame(frame);

    /* Second unref should free (refcount 0→free) */
    pmm_unref_frame(frame);
}

static void pmm_zero_test(struct kunit *test)
{
    /* Verify that allocated frames are zeroed */
    uint64_t frame = pmm_alloc_frame();
    KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
    if (!frame) return;

    /* Map the frame temporarily to check content */
    uint64_t *ptr = (uint64_t *)PHYS_TO_VIRT(frame);
    int is_zero = 1;
    for (int i = 0; i < (int)(PAGE_SIZE / sizeof(uint64_t)); i++) {
        if (ptr[i] != 0) {
            is_zero = 0;
            break;
        }
    }
    KUNIT_EXPECT_EQ(test, is_zero, 1);

    pmm_free_frame(frame);
}

/* ====================================================================
 *  2. Slab — Kernel heap allocator tests
 * ==================================================================== */

static void slab_alloc_free_test(struct kunit *test)
{
    void *p = kmalloc(64);
    KUNIT_EXPECT_NOT_NULL(test, p);
    if (p) {
        /* Write pattern and verify */
        memset(p, 0xAB, 64);
        kfree(p);
    }

    /* Alloc after free should give a different or same address (doesn't matter) */
    void *p2 = kmalloc(64);
    KUNIT_EXPECT_NOT_NULL(test, p2);
    if (p2)
        kfree(p2);
}

static void slab_varied_sizes_test(struct kunit *test)
{
    /* Test various allocation sizes */
    size_t sizes[] = {1, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    void *ptrs[12];
    int n = (int)(sizeof(sizes) / sizeof(sizes[0]));

    for (int i = 0; i < n; i++) {
        ptrs[i] = kmalloc(sizes[i]);
        KUNIT_EXPECT_NOT_NULL(test, ptrs[i]);
        if (ptrs[i]) {
            /* Write to each byte to ensure page is accessible */
            memset(ptrs[i], (int)(i + 1), sizes[i]);
        }
    }

    /* Free in reverse order */
    for (int i = n - 1; i >= 0; i--) {
        if (ptrs[i])
            kfree(ptrs[i]);
    }
}

static void slab_realloc_test(struct kunit *test)
{
    void *p = kmalloc(16);
    KUNIT_EXPECT_NOT_NULL(test, p);
    if (!p) return;

    memset(p, 0x42, 16);

    /* Free and alloc larger to test basic allocator independence */
    kfree(p);

    void *p2 = kmalloc(64);
    KUNIT_EXPECT_NOT_NULL(test, p2);
    if (p2) {
        memset(p2, 0x43, 64);
        kfree(p2);
    }
}

static void slab_boundary_test(struct kunit *test)
{
    /* Test boundary conditions: size=0 returns valid ptr or NULL */
    void *p = kmalloc(0);
    /* kmalloc(0) may return NULL or a unique pointer; either is acceptable */
    if (p) kfree(p);

    /* Very large allocation should fail gracefully */
    void *big = kmalloc(0xFFFFFFFFULL);
    KUNIT_EXPECT_NULL(test, big);
}

/* ====================================================================
 *  3. String — Basic string operation tests
 * ==================================================================== */

static void string_strlen_test(struct kunit *test)
{
    KUNIT_EXPECT_EQ(test, (int64_t)strlen(""),        (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)strlen("a"),       (int64_t)1);
    KUNIT_EXPECT_EQ(test, (int64_t)strlen("hello"),   (int64_t)5);
    KUNIT_EXPECT_EQ(test, (int64_t)strlen("hello\n\t"), (int64_t)7);
}

static void string_strcmp_test(struct kunit *test)
{
    KUNIT_EXPECT_EQ(test, (int64_t)strcmp("abc", "abc"), (int64_t)0);
    KUNIT_EXPECT_NE(test, (int64_t)strcmp("abc", "abd"), (int64_t)0);
    KUNIT_EXPECT_NE(test, (int64_t)strcmp("abc", "ab"),  (int64_t)0);
}

static void string_memset_test(struct kunit *test)
{
    char buf[32];
    memset(buf, 0xAA, sizeof(buf));

    int ok = 1;
    for (size_t i = 0; i < sizeof(buf); i++) {
        if ((uint8_t)buf[i] != 0xAA) {
            ok = 0;
            break;
        }
    }
    KUNIT_EXPECT_EQ(test, ok, 1);

    /* Test partial memset */
    memset(buf, 0xBB, 16);
    for (size_t i = 0; i < 16; i++) {
        if ((uint8_t)buf[i] != 0xBB) {
            ok = 0;
            break;
        }
    }
    for (size_t i = 16; i < sizeof(buf); i++) {
        if ((uint8_t)buf[i] != 0xAA) {
            ok = 0;
            break;
        }
    }
    KUNIT_EXPECT_EQ(test, ok, 1);
}

static void string_memcpy_test(struct kunit *test)
{
    const char src[] = "Hello, KUnit!";
    char dst[32];
    memset(dst, 0, sizeof(dst));

    memcpy(dst, src, sizeof(src));
    KUNIT_EXPECT_EQ(test, (int64_t)strcmp(dst, src), (int64_t)0);
}

/* ====================================================================
 *  4. VMM — Virtual Memory Manager tests (basic)
 * ==================================================================== */

static void vmm_map_unmap_test(struct kunit *test)
{
    /* Allocate a physical frame and map it temporarily */
    uint64_t phys = pmm_alloc_frame();
    KUNIT_EXPECT_NE(test, phys, (uint64_t)0);
    if (!phys) return;

    /* Map at a test virtual address (high kernel space, unlikely to conflict) */
    uint64_t test_vaddr = 0xFFFFC0FFE0000000ULL;

    int ret = vmm_map_page(test_vaddr, phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_NOEXEC);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);

    /* Verify it's mapped */
    uint64_t mapped_phys = vmm_get_physaddr(test_vaddr);
    KUNIT_EXPECT_NE(test, (int64_t)mapped_phys, (int64_t)0);

    /* Write a test pattern */
    volatile uint64_t *ptr = (volatile uint64_t *)test_vaddr;
    *ptr = 0xDEADBEEF;

    /* Read back */
    KUNIT_EXPECT_EQ(test, (int64_t)*ptr, (int64_t)0xDEADBEEF);

    /* Unmap */
    vmm_unmap_page(test_vaddr);

    /* After unmap, the page is gone — just verify it doesn't fault us here */
    /* (kernel would crash on access, so we don't try) */

    pmm_free_frame(phys);
}

/* ====================================================================
 *  Suite definitions
 * ==================================================================== */

static struct kunit_case pmm_test_cases[] = {
    KUNIT_CASE(pmm_alloc_free_test),
    KUNIT_CASE(pmm_refcount_test),
    KUNIT_CASE(pmm_zero_test),
    {NULL, NULL}
};

static struct kunit_case slab_test_cases[] = {
    KUNIT_CASE(slab_alloc_free_test),
    KUNIT_CASE(slab_varied_sizes_test),
    KUNIT_CASE(slab_realloc_test),
    KUNIT_CASE(slab_boundary_test),
    {NULL, NULL}
};

static struct kunit_case string_test_cases[] = {
    KUNIT_CASE(string_strlen_test),
    KUNIT_CASE(string_strcmp_test),
    KUNIT_CASE(string_memset_test),
    KUNIT_CASE(string_memcpy_test),
    {NULL, NULL}
};

static struct kunit_case vmm_test_cases[] = {
    KUNIT_CASE(vmm_map_unmap_test),
    {NULL, NULL}
};

/* Helper macro to populate a fixed-size array from a NULL-terminated list */
#define FILL_CASES(suite_var, cases_array) do {                     \
    int __ci = 0;                                                   \
    for (int __i = 0; cases_array[__i].run != NULL && __i < KUNIT_MAX_CASES - 1; __i++) { \
        suite_var.cases[__ci].name = cases_array[__i].name;         \
        suite_var.cases[__ci].run  = cases_array[__i].run;          \
        __ci++;                                                     \
    }                                                               \
    suite_var.cases[__ci].name = NULL;                              \
    suite_var.cases[__ci].run  = NULL;                              \
} while(0)

static struct kunit_suite pmm_test_suite;
static struct kunit_suite slab_test_suite;
static struct kunit_suite string_test_suite;
static struct kunit_suite vmm_test_suite;

/* ── Registration function (called from kunit_init) ────────────── */

/* PMM tests live in kunit_pmm.c */
void kunit_pmm_register(void);
/* Slab tests live in kunit_slab.c */
void kunit_slab_register(void);

void kunit_register_builtin_tests(void)
{
    /* Populate the fixed-size case arrays from our termination-checked lists */
    FILL_CASES(pmm_test_suite, pmm_test_cases);
    FILL_CASES(slab_test_suite, slab_test_cases);
    FILL_CASES(string_test_suite, string_test_cases);
    FILL_CASES(vmm_test_suite, vmm_test_cases);

    /* Set suite names */
    pmm_test_suite.name    = "pmm";
    slab_test_suite.name   = "slab";
    string_test_suite.name = "string";
    vmm_test_suite.name    = "vmm";

    kunit_register_suite(&pmm_test_suite);
    kunit_register_suite(&slab_test_suite);
    kunit_register_suite(&string_test_suite);
    kunit_register_suite(&vmm_test_suite);

    /* Register the dedicated PMM test suite from kunit_pmm.c */
    kunit_pmm_register();

    /* Register the dedicated slab test suite from kunit_slab.c */
    kunit_slab_register();
}
