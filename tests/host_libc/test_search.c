/*
 * test_search.c — Host-side tests for kernel search/sort algorithms
 *
 * Tests qsort_ext, bsearch_ext, lfind, lsearch, search_binary, search_linear
 * from src/lib/search.c.  All pure algorithmic — no kernel deps beyond stubs.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===================================================================
 *  Declarations of kernel libc functions being tested
 *  (kernel size_t = uint64_t = unsigned long long)
 * =================================================================== */
typedef unsigned long long kernel_size_t;

extern void qsort_ext(void *base, kernel_size_t nmemb, kernel_size_t size,
                      int (*compar)(const void *, const void *));
extern void *bsearch_ext(const void *key, const void *base,
                         kernel_size_t nmemb, kernel_size_t size,
                         int (*compar)(const void *, const void *));
extern void *lfind(const void *key, const void *base,
                   kernel_size_t *nmemb, kernel_size_t size,
                   int (*compar)(const void *, const void *));
extern void *lsearch(const void *key, void *base,
                     kernel_size_t *nmemb, kernel_size_t size,
                     int (*compar)(const void *, const void *));
extern int search_binary(const void *key, const void *base,
                         kernel_size_t nmemb, kernel_size_t size, void *cmp);
extern int search_linear(const void *key, const void *base,
                         kernel_size_t nmemb, kernel_size_t size, void *cmp);

/* ===================================================================
 *  Stubs for kernel symbols referenced by printf (never called by search.c)
 * =================================================================== */
void vga_putchar(char c)     { (void)c; }
void serial_putchar(char c)  { (void)c; }
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

/* ===================================================================
 *  Comparison helpers for testing
 * =================================================================== */
static int cmp_int_asc(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}
static int cmp_int_desc(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) ? -1 : (x < y) ? 1 : 0;
}
static int cmp_struct(const void *a, const void *b) {
    const int *pa = a, *pb = b;
    return (*pa < *pb) ? -1 : (*pa > *pb) ? 1 : 0;
}
#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ===================================================================
 *  test_qsort_ext
 * =================================================================== */
static void test_qsort_ext(void)
{
    /* 1. Basic sort of ints */
    int a1[] = { 9, 3, 7, 1, 5, 8, 2, 4, 6, 0 };
    qsort_ext(a1, ARRAY_SIZE(a1), sizeof(int), cmp_int_desc);
    int sorted1 = 1;
    for (int i = 1; i < ARRAY_SIZE(a1); i++)
        if (a1[i-1] < a1[i]) { sorted1 = 0; break; }
    TEST("qsort_ext: sorts ints descending", sorted1);

    /* 2. Already sorted (by the ascending comparator) */
    int a2[] = { 0, 1, 2, 3, 4, 5 };
    qsort_ext(a2, ARRAY_SIZE(a2), sizeof(int), cmp_int_asc);
    int ok2 = 1;
    for (int i = 1; i < ARRAY_SIZE(a2); i++)
        if (a2[i-1] > a2[i]) { ok2 = 0; break; }
    TEST("qsort_ext: already sorted preserves order", ok2);

    /* 3. Reverse order */
    int a3[] = { 5, 4, 3, 2, 1, 0 };
    qsort_ext(a3, ARRAY_SIZE(a3), sizeof(int), cmp_int_desc);
    int sorted3 = 1;
    for (int i = 1; i < ARRAY_SIZE(a3); i++)
        if (a3[i-1] < a3[i]) { sorted3 = 0; break; }
    TEST("qsort_ext: reverse sorted", sorted3);

    /* 4. Duplicates */
    int a4[] = { 3, 1, 3, 2, 1, 2 };
    qsort_ext(a4, ARRAY_SIZE(a4), sizeof(int), cmp_int_desc);
    int sorted4 = 1;
    for (int i = 1; i < ARRAY_SIZE(a4); i++)
        if (a4[i-1] < a4[i]) { sorted4 = 0; break; }
    TEST("qsort_ext: handles duplicates", sorted4);

    /* 5. Single element */
    int a5[] = { 42 };
    qsort_ext(a5, 1, sizeof(int), cmp_int_desc);
    TEST("qsort_ext: single element", a5[0] == 42);

    /* 6. Empty array (nmemb=0) */
    int a6[] = { 99 };
    qsort_ext(a6, 0, sizeof(int), cmp_int_desc);
    TEST("qsort_ext: empty array", a6[0] == 99);

    /* 7. Sort structs (keyed on first int) */
    struct pair { int key; int val; } p[] = { {5,50}, {2,20}, {8,80}, {1,10} };
    qsort_ext(p, ARRAY_SIZE(p), sizeof(struct pair), cmp_struct);
    int sorted7 = (p[0].key == 1 && p[1].key == 2 && p[2].key == 5 && p[3].key == 8);
    TEST("qsort_ext: sorts struct by key", sorted7);

    /* 8. Larger array (tests insertion sort threshold and recursion) */
    int a8[64];
    for (int i = 0; i < 64; i++) a8[i] = (i * 7 + 13) % 64;
    qsort_ext(a8, 64, sizeof(int), cmp_int_desc);
    int sorted8 = 1;
    for (int i = 1; i < 64; i++)
        if (a8[i-1] < a8[i]) { sorted8 = 0; break; }
    TEST("qsort_ext: 64-element array", sorted8);

    /* 9. All-equal comparator — should not crash or reorder */
    int a9[] = { 5, 5, 5, 5, 5 };
    qsort_ext(a9, 5, sizeof(int), cmp_int_asc);
    int all_eq = 1;
    for (int i = 0; i < 5; i++) if (a9[i] != 5) { all_eq = 0; break; }
    TEST("qsort_ext: all equal", all_eq);

    /* 10. 1024-element array */
    int a10[1024];
    for (int i = 0; i < 1024; i++) a10[i] = (i * 31 + 17) % 1024;
    qsort_ext(a10, 1024, sizeof(int), cmp_int_asc);
    int sorted10 = 1;
    for (int i = 1; i < 1024; i++)
        if (a10[i-1] > a10[i]) { sorted10 = 0; break; }
    TEST("qsort_ext: 1024 elements", sorted10);
}

/* ===================================================================
 *  test_bsearch_ext
 * =================================================================== */
static void test_bsearch_ext(void)
{
    int arr[] = { 1, 3, 5, 7, 9, 11, 13, 15 };

    /* 1. Find existing element */
    int k1 = 7;
    int *r1 = (int *)bsearch_ext(&k1, arr, ARRAY_SIZE(arr), sizeof(int), cmp_int_asc);
    TEST("bsearch_ext: finds existing element", r1 && *r1 == 7);

    /* 2. Find non-existing */
    int k2 = 6;
    int *r2 = (int *)bsearch_ext(&k2, arr, ARRAY_SIZE(arr), sizeof(int), cmp_int_asc);
    TEST("bsearch_ext: returns NULL for missing", r2 == NULL);

    /* 3. First element */
    int k3 = 1;
    int *r3 = (int *)bsearch_ext(&k3, arr, ARRAY_SIZE(arr), sizeof(int), cmp_int_asc);
    TEST("bsearch_ext: finds first element", r3 && *r3 == 1);

    /* 4. Last element */
    int k4 = 15;
    int *r4 = (int *)bsearch_ext(&k4, arr, ARRAY_SIZE(arr), sizeof(int), cmp_int_asc);
    TEST("bsearch_ext: finds last element", r4 && *r4 == 15);

    /* 5. Empty array */
    int k5 = 5;
    int *r5 = (int *)bsearch_ext(&k5, NULL, 0, sizeof(int), cmp_int_asc);
    TEST("bsearch_ext: empty array returns NULL", r5 == NULL);

    /* 6. Single-element array, match */
    int arr1[] = { 42 };
    int k6 = 42;
    int *r6 = (int *)bsearch_ext(&k6, arr1, 1, sizeof(int), cmp_int_asc);
    TEST("bsearch_ext: single element match", r6 && *r6 == 42);

    /* 7. Single-element array, no match */
    int k7 = 99;
    int *r7 = (int *)bsearch_ext(&k7, arr1, 1, sizeof(int), cmp_int_asc);
    TEST("bsearch_ext: single element no match", r7 == NULL);

    /* 8. nmemb=1 exact location */
    int arr1b[] = { 77 };
    int k8 = 77;
    int *r8 = (int *)bsearch_ext(&k8, arr1b, 1, sizeof(int), cmp_int_asc);
    TEST("bsearch_ext: nmemb=1 finds only element", r8 == arr1b && *r8 == 77);
}

/* ===================================================================
 *  test_lfind
 * =================================================================== */
static int cmp_char(const void *a, const void *b) {
    return *(const char *)a - *(const char *)b;
}

static void test_lfind(void)
{
    char arr[] = { 'a', 'b', 'c', 'd', 'e' };
    kernel_size_t nmemb = 5;

    /* 1. Find existing */
    char k1 = 'c';
    char *r1 = (char *)lfind(&k1, arr, &nmemb, 1, cmp_char);
    TEST("lfind: finds existing element", r1 && *r1 == 'c');

    /* 2. Find non-existing */
    char k2 = 'z';
    char *r2 = (char *)lfind(&k2, arr, &nmemb, 1, cmp_char);
    TEST("lfind: returns NULL for missing", r2 == NULL);

    /* 3. Empty array */
    char k3 = 'x';
    kernel_size_t empty_nmemb = 0;
    char *r3 = (char *)lfind(&k3, arr, &empty_nmemb, 1, cmp_char);
    TEST("lfind: empty array returns NULL", r3 == NULL);

    /* 4. First element */
    char k4 = 'a';
    char *r4 = (char *)lfind(&k4, arr, &nmemb, 1, cmp_char);
    TEST("lfind: finds first element", r4 && *r4 == 'a');

    /* 5. Last element */
    char k5 = 'e';
    char *r5 = (char *)lfind(&k5, arr, &nmemb, 1, cmp_char);
    TEST("lfind: finds last element", r5 && *r5 == 'e');

    /* 6. Every position in array */
    int all_positions_ok = 1;
    char expected[] = {'a','b','c','d','e'};
    for (size_t i = 0; i < 5; i++) {
        char *r = (char *)lfind(&expected[i], arr, &nmemb, 1, cmp_char);
        if (!r || *r != expected[i]) { all_positions_ok = 0; break; }
    }
    TEST("lfind: finds at every position", all_positions_ok);
}

/* ===================================================================
 *  test_lsearch
 * =================================================================== */
static void test_lsearch(void)
{
    char buf[32] = { 'x', 'y', 'z' };
    kernel_size_t nmemb = 3;

    /* 1. Find existing */
    char k1 = 'y';
    char *r1 = (char *)lsearch(&k1, buf, &nmemb, 1, cmp_char);
    TEST("lsearch: finds existing", r1 && *r1 == 'y' && nmemb == 3);

    /* 2. Not found — appends */
    char k2 = 'w';
    kernel_size_t nmemb2 = 3;
    char buf2[32] = { 'x', 'y', 'z' };
    char *r2 = (char *)lsearch(&k2, buf2, &nmemb2, 1, cmp_char);
    TEST("lsearch: appends on not found", r2 && *r2 == 'w' && nmemb2 == 4);
    TEST("lsearch: insertion at correct position", buf2[3] == 'w');

    /* 3. Empty — appends */
    char k3 = 'a';
    kernel_size_t nmemb3 = 0;
    char buf3[32];
    char *r3 = (char *)lsearch(&k3, buf3, &nmemb3, 1, cmp_char);
    TEST("lsearch: empty array appends", r3 && *r3 == 'a' && nmemb3 == 1);

    /* 4. Multiple appends */
    kernel_size_t nmemb4 = 0;
    char buf4[32];
    char keys4[] = { 'p', 'q', 'r' };
    for (int i = 0; i < 3; i++)
        lsearch(&keys4[i], buf4, &nmemb4, 1, cmp_char);
    TEST("lsearch: multiple appends", nmemb4 == 3 && buf4[0]=='p' && buf4[1]=='q' && buf4[2]=='r');
}

/* ===================================================================
 *  test_search_binary
 * =================================================================== */
static void test_search_binary(void)
{
    int arr[] = { 10, 20, 30, 40, 50 };

    /* 1. Find existing */
    int k1 = 30;
    int r1 = search_binary(&k1, arr, ARRAY_SIZE(arr), sizeof(int), cmp_int_asc);
    TEST("search_binary: finds existing index", r1 == 2);

    /* 2. Not found */
    int k2 = 25;
    int r2 = search_binary(&k2, arr, ARRAY_SIZE(arr), sizeof(int), cmp_int_asc);
    TEST("search_binary: returns -1 for missing", r2 == -1);

    /* 3. Null key/base/cmp */
    int r3 = search_binary(NULL, arr, ARRAY_SIZE(arr), sizeof(int), cmp_int_asc);
    TEST("search_binary: NULL key returns -1", r3 == -1);

    int k4 = 10;
    int r4 = search_binary(&k4, NULL, ARRAY_SIZE(arr), sizeof(int), cmp_int_asc);
    TEST("search_binary: NULL base returns -1", r4 == -1);
}

/* ===================================================================
 *  test_search_linear
 * =================================================================== */
static void test_search_linear(void)
{
    int arr[] = { 100, 200, 300, 400, 500 };

    /* 1. Find existing */
    int k1 = 300;
    int r1 = search_linear(&k1, arr, ARRAY_SIZE(arr), sizeof(int), cmp_int_asc);
    TEST("search_linear: finds existing index", r1 == 2);

    /* 2. Not found */
    int k2 = 250;
    int r2 = search_linear(&k2, arr, ARRAY_SIZE(arr), sizeof(int), cmp_int_asc);
    TEST("search_linear: returns -1 for missing", r2 == -1);

    /* 3. First element */
    int k3 = 100;
    int r3 = search_linear(&k3, arr, ARRAY_SIZE(arr), sizeof(int), cmp_int_asc);
    TEST("search_linear: finds first element", r3 == 0);

    /* 4. Last element */
    int k4 = 500;
    int r4 = search_linear(&k4, arr, ARRAY_SIZE(arr), sizeof(int), cmp_int_asc);
    TEST("search_linear: finds last element", r4 == 4);

    /* 5. Empty array (nmemb=0) */
    int k5 = 100;
    int r5 = search_linear(&k5, arr, 0, sizeof(int), cmp_int_asc);
    TEST("search_linear: empty array returns -1", r5 == -1);
}

/* ===================================================================
 *  test_search_extras — additional edge cases
 * =================================================================== */
static void test_search_extras(void)
{
    int arr[] = { 10, 20, 30, 40, 50 };

    /* bsearch_ext: key below min */
    int k_low = 5;
    int *r_low = (int *)bsearch_ext(&k_low, arr, ARRAY_SIZE(arr), sizeof(int), cmp_int_asc);
    TEST("bsearch_ext: key below min returns NULL", r_low == NULL);

    /* bsearch_ext: key above max */
    int k_high = 99;
    int *r_high = (int *)bsearch_ext(&k_high, arr, ARRAY_SIZE(arr), sizeof(int), cmp_int_asc);
    TEST("bsearch_ext: key above max returns NULL", r_high == NULL);

    /* bsearch_ext: duplicates in array */
    int dup_arr[] = { 1, 2, 2, 2, 3, 4, 5 };
    int k_dup = 2;
    int *r_dup = (int *)bsearch_ext(&k_dup, dup_arr, ARRAY_SIZE(dup_arr), sizeof(int), cmp_int_asc);
    TEST("bsearch_ext: finds element in duplicated array", r_dup && *r_dup == 2);

    /* bsearch_ext: descending comparator on descending sorted array */
    int desc_arr[] = { 50, 40, 30, 20, 10 };
    int k_desc = 30;
    int *r_desc = (int *)bsearch_ext(&k_desc, desc_arr, ARRAY_SIZE(desc_arr), sizeof(int), cmp_int_desc);
    TEST("bsearch_ext: desc comparator finds element", r_desc && *r_desc == 30);

    /* bsearch_ext: desc comparator miss */
    int k_desc_miss = 25;
    int *r_desc_miss = (int *)bsearch_ext(&k_desc_miss, desc_arr, ARRAY_SIZE(desc_arr), sizeof(int), cmp_int_desc);
    TEST("bsearch_ext: desc comparator miss returns NULL", r_desc_miss == NULL);

    /* lfind: single element, found */
    char single_arr[] = { 'q' };
    kernel_size_t single_nmemb = 1;
    char k_single = 'q';
    char *r_single = (char *)lfind(&k_single, single_arr, &single_nmemb, 1, cmp_char);
    TEST("lfind: single element found", r_single && *r_single == 'q');

    /* lfind: single element, not found */
    char k_single_miss = 'z';
    kernel_size_t single_nmemb2 = 1;
    char arr_single2[] = { 'q' };
    char *r_single_miss = (char *)lfind(&k_single_miss, arr_single2, &single_nmemb2, 1, cmp_char);
    TEST("lfind: single element not found returns NULL", r_single_miss == NULL);

    /* search_binary: NULL cmp */
    int sb_k = 2;
    int sb_r = search_binary(&sb_k, arr, ARRAY_SIZE(arr), sizeof(int), NULL);
    TEST("search_binary: NULL cmp returns -1", sb_r == -1);

    /* search_linear: empty array (nmemb=0) */
    int sl_k = 42;
    int sl_r = search_linear(&sl_k, arr, 0, sizeof(int), cmp_int_asc);
    TEST("search_linear: empty array returns -1", sl_r == -1);

    /* search_linear: NULL cmp */
    int sl_r2 = search_linear(&sl_k, arr, ARRAY_SIZE(arr), sizeof(int), NULL);
    TEST("search_linear: NULL cmp returns -1", sl_r2 == -1);
}

/* ===================================================================
 *  test_search_more — additional edge-case tests
 * =================================================================== */
static void test_search_more(void)
{
    /* 1. qsort_ext: all elements identical large array */
    {
        int a[100];
        for (int i = 0; i < 100; i++) a[i] = 42;
        qsort_ext(a, 100, sizeof(int), cmp_int_asc);
        int ok = 1;
        for (int i = 0; i < 100; i++) if (a[i] != 42) { ok = 0; break; }
        TEST("qsort_ext: 100 identical elements sorted (all 42)", ok);
    }

    /* 2. qsort_ext: two elements */
    {
        int a[] = { 99, 1 };
        qsort_ext(a, 2, sizeof(int), cmp_int_asc);
        TEST("qsort_ext: two elements ascending", a[0]==1 && a[1]==99);
    }

    /* 3. qsort_ext: two elements descending */
    {
        int a[] = { 1, 99 };
        qsort_ext(a, 2, sizeof(int), cmp_int_desc);
        TEST("qsort_ext: two elements descending", a[0]==99 && a[1]==1);
    }

    /* 4. qsort_ext: already sorted with odd number of elements */
    {
        int a[] = { 1, 2, 3, 4, 5, 6, 7 };
        qsort_ext(a, 7, sizeof(int), cmp_int_asc);
        int ok = 1;
        for (int i = 1; i < 7; i++) if (a[i-1] > a[i]) { ok = 0; break; }
        TEST("qsort_ext: 7 already sorted preserves order", ok);
    }

    /* 5. bsearch_ext: array with all identical elements */
    {
        int a[10];
        for (int i = 0; i < 10; i++) a[i] = 5;
        int key = 5;
        int *r = (int *)bsearch_ext(&key, a, 10, sizeof(int), cmp_int_asc);
        TEST("bsearch_ext: all identical, finds key", r && *r == 5);
        int missing = 3;
        int *r2 = (int *)bsearch_ext(&missing, a, 10, sizeof(int), cmp_int_asc);
        TEST("bsearch_ext: all identical, missing returns NULL", r2 == NULL);
    }

    /* 6. bsearch_ext: key at odd positions (boundary checks) */
    {
        int a[] = { 2, 4, 6, 8, 10, 12, 14, 16 };
        int key = 8;
        int *r = (int *)bsearch_ext(&key, a, 8, sizeof(int), cmp_int_asc);
        TEST("bsearch_ext: key at exact middle (index 3)", r && *r == 8);
        key = 12;
        r = (int *)bsearch_ext(&key, a, 8, sizeof(int), cmp_int_asc);
        TEST("bsearch_ext: key at index 5", r && *r == 12);
    }

    /* 7. bsearch_ext: large array (100 elements) */
    {
        int a[100];
        for (int i = 0; i < 100; i++) a[i] = i * 10;
        int key = 0;
        int *r = (int *)bsearch_ext(&key, a, 100, sizeof(int), cmp_int_asc);
        TEST("bsearch_ext: large array, first element found", r && *r == 0);
        key = 990;
        r = (int *)bsearch_ext(&key, a, 100, sizeof(int), cmp_int_asc);
        TEST("bsearch_ext: large array, last element found", r && *r == 990);
        key = 555;
        r = (int *)bsearch_ext(&key, a, 100, sizeof(int), cmp_int_asc);
        TEST("bsearch_ext: large array, missing returns NULL", r == NULL);
    }

    /* 8. lfind: array with duplicates — finds first occurrence */
    {
        char arr[] = { 'a', 'b', 'a', 'c', 'a' };
        kernel_size_t nm = 5;
        char key = 'a';
        char *r = (char *)lfind(&key, arr, &nm, 1, cmp_char);
        TEST("lfind: finds first 'a' in duplicate array", r && *r == 'a' && r == &arr[0]);
    }

    /* 9. lfind: search in 0-length array always NULL */
    {
        char arr[] = { 'x' };
        kernel_size_t nm = 0;
        char key = 'x';
        char *r = (char *)lfind(&key, arr, &nm, 1, cmp_char);
        TEST("lfind: nmemb=0 always returns NULL", r == NULL);
    }

    /* 10. lsearch: append when array is 0-length */
    {
        char buf[32];
        kernel_size_t nm = 0;
        char key = 'z';
        char *r = (char *)lsearch(&key, buf, &nm, 1, cmp_char);
        TEST("lsearch: append to 0-length array", r && *r == 'z' && nm == 1);
        TEST("lsearch: first element is key", buf[0] == 'z');
    }

    /* 11. lsearch: existing element does NOT change nmemb */
    {
        char buf[32] = { 'a', 'b' };
        kernel_size_t nm = 2;
        char key = 'a';
        char *r = (char *)lsearch(&key, buf, &nm, 1, cmp_char);
        TEST("lsearch: existing element, nmemb unchanged", r && *r == 'a' && nm == 2);
    }

    /* 12. search_binary: value at every index in array */
    {
        int a[] = { 10, 20, 30, 40, 50, 60 };
        TEST("search_binary: index 0", search_binary(&a[0], a, 6, sizeof(int), cmp_int_asc) == 0);
        TEST("search_binary: index 2", search_binary(&a[2], a, 6, sizeof(int), cmp_int_asc) == 2);
        TEST("search_binary: index 5", search_binary(&a[5], a, 6, sizeof(int), cmp_int_asc) == 5);
        int miss = 15;
        TEST("search_binary: missing returns -1", search_binary(&miss, a, 6, sizeof(int), cmp_int_asc) == -1);
    }

    /* 13. search_linear: array with one element */
    {
        int a[] = { 42 };
        int key = 42;
        int r = search_linear(&key, a, 1, sizeof(int), cmp_int_asc);
        TEST("search_linear: single element found", r == 0);
        int miss = 99;
        r = search_linear(&miss, a, 1, sizeof(int), cmp_int_asc);
        TEST("search_linear: single element miss", r == -1);
    }

    /* 14. search_linear: duplicates — returns first index */
    {
        int a[] = { 7, 3, 7, 5, 7 };
        int key = 7;
        int r = search_linear(&key, a, 5, sizeof(int), cmp_int_asc);
        TEST("search_linear: duplicates, finds first (index 0)", r == 0);
    }

    /* 15. search_binary: negative values */
    {
        int a[] = { -10, -5, 0, 5, 10 };
        int k = -5;
        int r = search_binary(&k, a, 5, sizeof(int), cmp_int_asc);
        TEST("search_binary: negative key found", r == 1);
        k = -7;
        r = search_binary(&k, a, 5, sizeof(int), cmp_int_asc);
        TEST("search_binary: negative key missing", r == -1);
    }
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== Search/Sort Algorithm Tests ===\n\n");

    printf("--- qsort_ext ---\n");
    test_qsort_ext();

    printf("\n--- bsearch_ext ---\n");
    test_bsearch_ext();

    printf("\n--- lfind ---\n");
    test_lfind();

    printf("\n--- lsearch ---\n");
    test_lsearch();

    printf("\n--- search_binary ---\n");
    test_search_binary();

    printf("\n--- search_linear ---\n");
    test_search_linear();

    printf("\n--- extras ---\n");
    test_search_extras();

    printf("\n--- more edge cases ---\n");
    test_search_more();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
