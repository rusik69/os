/*
 * test_sort_ext.c — Host-side tests for kernel sort extension
 *
 * Tests sort, swap_bytes, cmp_int, cmp_long, cmp_str from src/kernel/sort_ext.c.
 * Pure algorithmic — no kernel deps beyond stubs.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===================================================================
 *  Kernel function prototypes (kernel size_t = unsigned long long)
 * =================================================================== */
typedef unsigned long long kernel_size_t;

extern void sort(void *base, kernel_size_t nmemb, kernel_size_t size,
                 int (*cmp)(const void *, const void *),
                 void (*swap)(void *, void *, kernel_size_t));
extern void swap_bytes(void *a, void *b, kernel_size_t size);
extern int cmp_int(const void *a, const void *b);
extern int cmp_long(const void *a, const void *b);
extern int cmp_str(const void *a, const void *b);

/* ===================================================================
 *  Stubs for kernel symbols
 * =================================================================== */
void vga_putchar(char c)      { (void)c; }
void serial_putchar(char c)   { (void)c; }
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
int console_loglevel = 7;
int default_message_loglevel = 6;

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

#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int is_sorted_int(int *arr, int n, int ascending)
{
    for (int i = 1; i < n; i++) {
        if (ascending && arr[i-1] > arr[i]) return 0;
        if (!ascending && arr[i-1] < arr[i]) return 0;
    }
    return 1;
}

/* Custom swap that counts calls */
static int swap_count = 0;
static void counting_swap(void *a, void *b, kernel_size_t size) {
    swap_bytes(a, b, size);
    swap_count++;
}

/* Reversed comparison for descending sort */
static int cmp_int_desc(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) ? -1 : (x < y) ? 1 : 0;
}

/* ===================================================================
 *  test_swap_bytes
 * =================================================================== */
static void test_swap_bytes(void)
{
    /* 1. Swap ints */
    int a = 42, b = 99;
    swap_bytes(&a, &b, sizeof(int));
    TEST("swap_bytes: swaps int values", a == 99 && b == 42);

    /* 2. Swap chars */
    char c1 = 'x', c2 = 'y';
    swap_bytes(&c1, &c2, 1);
    TEST("swap_bytes: swaps chars", c1 == 'y' && c2 == 'x');

    /* 3. Swap large struct */
    struct { char buf[8]; } s1 = {"hello"}, s2 = {"world"};
    swap_bytes(&s1, &s2, 8);
    TEST("swap_bytes: swaps 8 bytes", memcmp(s1.buf, "world", 5) == 0);

    /* 4. Swap zero size — no crash */
    int d = 1, e = 2;
    swap_bytes(&d, &e, 0);
    TEST("swap_bytes: zero size no-op", d == 1 && e == 2);

    /* 5. Swap overlapping same — no crash */
    int f = 42;
    swap_bytes(&f, &f, sizeof(int));
    TEST("swap_bytes: self-swap no-op", f == 42);
}

/* ===================================================================
 *  test_sort_int
 * =================================================================== */
static void test_sort_int(void)
{
    /* 1. Basic ascending sort */
    int a1[] = { 9, 3, 7, 1, 5, 8, 2, 4, 6, 0 };
    sort(a1, ARRAY_SIZE(a1), sizeof(int), cmp_int, NULL);
    TEST("sort: ints ascending", is_sorted_int(a1, ARRAY_SIZE(a1), 1));
    int all_present = 1;
    for (int i = 0; i < 10; i++) {
        int found = 0;
        for (int j = 0; j < 10; j++) { if (a1[j] == i) { found = 1; break; } }
        if (!found) { all_present = 0; break; }
    }
    TEST("sort: all elements preserved", all_present);

    /* 2. Already sorted */
    int a2[] = { 0, 1, 2, 3, 4, 5 };
    sort(a2, ARRAY_SIZE(a2), sizeof(int), cmp_int, NULL);
    TEST("sort: already sorted preserves order", is_sorted_int(a2, 6, 1));

    /* 3. Reverse order */
    int a3[] = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
    sort(a3, ARRAY_SIZE(a3), sizeof(int), cmp_int, NULL);
    TEST("sort: reverses correctly", is_sorted_int(a3, 10, 1));

    /* 4. Single element */
    int a4[] = { 42 };
    sort(a4, 1, sizeof(int), cmp_int, NULL);
    TEST("sort: single element", a4[0] == 42);

    /* 5. Empty array */
    int a5[] = { 99 };
    sort(a5, 0, sizeof(int), cmp_int, NULL);
    TEST("sort: empty array", a5[0] == 99);

    /* 6. Duplicates */
    int a6[] = { 3, 1, 4, 1, 5, 9, 2, 6, 5, 3 };
    sort(a6, ARRAY_SIZE(a6), sizeof(int), cmp_int, NULL);
    TEST("sort: handles duplicates", is_sorted_int(a6, ARRAY_SIZE(a6), 1));

    /* 7. Descending via custom cmp */
    int a7[] = { 1, 5, 3, 9, 7 };
    sort(a7, ARRAY_SIZE(a7), sizeof(int), cmp_int_desc, NULL);
    TEST("sort: descending order via cmp", is_sorted_int(a7, ARRAY_SIZE(a7), 0));

    /* 8. NULL base — no crash */
    sort(NULL, 0, sizeof(int), cmp_int, NULL);
    TEST("sort: NULL base no crash", 1);
}

/* ===================================================================
 *  test_sort_strings
 * =================================================================== */
static void test_sort_strings(void)
{
    const char *arr[] = { "banana", "apple", "cherry", "date", "elderberry" };
    sort(arr, ARRAY_SIZE(arr), sizeof(const char *), cmp_str, NULL);
    int sorted = 1;
    for (int i = 1; i < ARRAY_SIZE(arr); i++)
        if (strcmp(arr[i-1], arr[i]) > 0) { sorted = 0; break; }
    TEST("sort: strings ascending", sorted);

    /* Check all elements present */
    int has_apple = 0, has_banana = 0;
    for (int i = 0; i < ARRAY_SIZE(arr); i++) {
        if (strcmp(arr[i], "apple") == 0) has_apple = 1;
        if (strcmp(arr[i], "banana") == 0) has_banana = 1;
    }
    TEST("sort: strings all present", has_apple && has_banana);
}

/* ===================================================================
 *  test_sort_custom_swap
 * =================================================================== */
static void test_sort_custom_swap(void)
{
    /* Use counting swap to verify custom swap is called */
    int a[] = { 5, 3, 1, 4, 2 };
    swap_count = 0;
    sort(a, ARRAY_SIZE(a), sizeof(int), cmp_int, counting_swap);
    TEST("sort: custom swap called", swap_count > 0);
    TEST("sort: custom swap produces correct order",
         is_sorted_int(a, ARRAY_SIZE(a), 1));
}

/* ===================================================================
 *  test_cmp_int
 * =================================================================== */
static void test_cmp_int(void)
{
    int a = 10, b = 20, c = 10;
    TEST("cmp_int: a < b",  cmp_int(&a, &b) < 0);
    TEST("cmp_int: a > b",  cmp_int(&b, &a) > 0);
    TEST("cmp_int: a == b", cmp_int(&a, &c) == 0);
}

/* ===================================================================
 *  test_cmp_long
 * =================================================================== */
static void test_cmp_long(void)
{
    long a = -100L, b = 100L, c = -100L;
    TEST("cmp_long: a < b",  cmp_long(&a, &b) < 0);
    TEST("cmp_long: a > b",  cmp_long(&b, &a) > 0);
    TEST("cmp_long: a == b", cmp_long(&a, &c) == 0);
}

/* ===================================================================
 *  test_cmp_str
 * =================================================================== */
static void test_cmp_str(void)
{
    const char *a = "apple", *b = "banana", *c = "apple";
    TEST("cmp_str: apple < banana",  cmp_str(&a, &b) < 0);
    TEST("cmp_str: banana > apple",  cmp_str(&b, &a) > 0);
    TEST("cmp_str: apple == apple",  cmp_str(&a, &c) == 0);
    TEST("cmp_str: same length diff", cmp_str(&a, &b) != 0);
}

/* ===================================================================
 *  test_sort_large
 * =================================================================== */
static void test_sort_large(void)
{
    /* Test the quicksort path (nmemb > 16) */
    int a[64];
    for (int i = 0; i < 64; i++) a[i] = (i * 17 + 31) % 64;
    sort(a, 64, sizeof(int), cmp_int, NULL);
    TEST("sort: 64 elements (quicksort path)", is_sorted_int(a, 64, 1));

    /* Verify all elements */
    int all = 1;
    int seen[64] = {0};
    for (int i = 0; i < 64; i++) seen[a[i]] = 1;
    for (int i = 0; i < 64; i++) if (!seen[i]) { all = 0; break; }
    TEST("sort: 64 elements all present", all);

    /* Test insertion sort boundary (nmemb=16) */
    int b[16];
    for (int i = 0; i < 16; i++) b[i] = (i * 13 + 7) % 16;
    sort(b, 16, sizeof(int), cmp_int, NULL);
    TEST("sort: 16 elements (insertion sort boundary)", is_sorted_int(b, 16, 1));
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== Sort Extension Tests ===\n\n");

    printf("--- swap_bytes ---\n");
    test_swap_bytes();

    printf("\n--- cmp_int ---\n");
    test_cmp_int();

    printf("\n--- cmp_long ---\n");
    test_cmp_long();

    printf("\n--- cmp_str ---\n");
    test_cmp_str();

    printf("\n--- sort (ints) ---\n");
    test_sort_int();

    printf("\n--- sort (strings) ---\n");
    test_sort_strings();

    printf("\n--- sort (custom swap) ---\n");
    test_sort_custom_swap();

    printf("\n--- sort (large) ---\n");
    test_sort_large();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
