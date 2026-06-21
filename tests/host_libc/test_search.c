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

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
