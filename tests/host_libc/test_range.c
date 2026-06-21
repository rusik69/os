/*
 * test_range.c — Host-side tests for kernel range management
 *
 * Tests range_sort, range_add, range_remove, range_contains, range_overlaps
 * from src/kernel/range.c.  Pure algorithmic — no kernel deps beyond stubs.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 *  Kernel types (mirrors src/include/range.h)
 *  uint64_t comes from <stdint.h>
 * =================================================================== */
struct range {
    uint64_t start;
    uint64_t end;   /* inclusive */
};

/* ===================================================================
 *  Kernel function prototypes
 * =================================================================== */
extern void range_sort(struct range *ranges, int *nr);
extern int range_add(struct range *ranges, int *nr, int max_size,
                     uint64_t start, uint64_t end);
extern int range_remove(struct range *ranges, int *nr,
                        uint64_t start, uint64_t end);
extern int range_contains(struct range *ranges, int nr, uint64_t val);
extern int range_overlaps(struct range *ranges, int nr,
                          uint64_t start, uint64_t end);

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

/* Helpers for checking range arrays */
static int ranges_match(struct range *r, int nr, uint64_t *expected, int enr)
{
    if (nr != enr) return 0;
    for (int i = 0; i < nr; i++)
        if (r[i].start != expected[i*2] || r[i].end != expected[i*2+1])
            return 0;
    return 1;
}

/* ===================================================================
 *  test_range_sort
 * =================================================================== */
static void test_range_sort(void)
{
    /* 1. Already sorted */
    struct range r1[] = { {10, 20}, {30, 40}, {50, 60} };
    int nr1 = 3;
    range_sort(r1, &nr1);
    TEST("range_sort: already sorted",
         ranges_match(r1, nr1, (uint64_t[]){10,20,30,40,50,60}, 3));

    /* 2. Reverse order */
    struct range r2[] = { {50, 60}, {30, 40}, {10, 20} };
    int nr2 = 3;
    range_sort(r2, &nr2);
    TEST("range_sort: reverse order",
         ranges_match(r2, nr2, (uint64_t[]){10,20,30,40,50,60}, 3));

    /* 3. Overlapping — merges */
    struct range r3[] = { {10, 30}, {20, 40}, {50, 60} };
    int nr3 = 3;
    range_sort(r3, &nr3);
    TEST("range_sort: overlapping merges",
         ranges_match(r3, nr3, (uint64_t[]){10,40,50,60}, 2));

    /* 4. Adjacent — merges */
    struct range r4[] = { {10, 19}, {20, 29}, {30, 39} };
    int nr4 = 3;
    range_sort(r4, &nr4);
    TEST("range_sort: adjacent merges",
         ranges_match(r4, nr4, (uint64_t[]){10,39}, 1));

    /* 5. Empty (nr=0) */
    struct range r5[1];
    int nr5 = 0;
    range_sort(r5, &nr5);
    TEST("range_sort: empty", nr5 == 0);

    /* 6. Single range */
    struct range r6[] = { {42, 99} };
    int nr6 = 1;
    range_sort(r6, &nr6);
    TEST("range_sort: single range",
         ranges_match(r6, nr6, (uint64_t[]){42,99}, 1));

    /* 7. Unsorted overlapping */
    struct range r7[] = { {100, 200}, {50, 150}, {300, 400} };
    int nr7 = 3;
    range_sort(r7, &nr7);
    TEST("range_sort: unsorted overlapping merges",
         ranges_match(r7, nr7, (uint64_t[]){50,200,300,400}, 2));

    /* 8. Duplicate entries — should merge */
    struct range r8[] = { {10, 30}, {10, 30}, {50, 60} };
    int nr8 = 3;
    range_sort(r8, &nr8);
    TEST("range_sort: duplicate entries merge",
         ranges_match(r8, nr8, (uint64_t[]){10,30,50,60}, 2));

    /* 9. Multiple duplicates */
    struct range r9[] = { {1,5}, {1,5}, {1,5}, {10,20} };
    int nr9 = 4;
    range_sort(r9, &nr9);
    TEST("range_sort: triple duplicates merge",
         ranges_match(r9, nr9, (uint64_t[]){1,5,10,20}, 2));
}

/* ===================================================================
 *  test_range_add
 * =================================================================== */
static void test_range_add(void)
{
    struct range r[16];
    int nr = 0;

    /* 1. Add first range */
    range_add(r, &nr, 16, 100, 200);
    TEST("range_add: first range",
         ranges_match(r, nr, (uint64_t[]){100,200}, 1));

    /* 2. Add non-overlapping higher */
    range_add(r, &nr, 16, 300, 400);
    TEST("range_add: non-overlapping higher",
         ranges_match(r, nr, (uint64_t[]){100,200,300,400}, 2));

    /* 3. Add non-overlapping lower */
    range_add(r, &nr, 16, 50, 80);
    TEST("range_add: non-overlapping lower",
         ranges_match(r, nr, (uint64_t[]){50,80,100,200,300,400}, 3));

    /* 4. Add overlapping (extends) */
    range_add(r, &nr, 16, 150, 250);
    TEST("range_add: overlap extends",
         ranges_match(r, nr, (uint64_t[]){50,80,100,250,300,400}, 3));

    /* 5. Add inside existing — no change expected */
    int nr5 = 3;
    struct range r5[] = { {100, 200}, {300, 400}, {500, 600} };
    range_add(r5, &nr5, 16, 150, 180);
    TEST("range_add: inside existing is absorbed",
         ranges_match(r5, nr5, (uint64_t[]){100,200,300,400,500,600}, 3));

    /* 6. Start > end returns error */
    int nr6 = 0;
    int ret = range_add(r, &nr6, 16, 200, 100);
    TEST("range_add: start>end returns -1", ret < 0);

    /* 7. Add adjacent (merges) */
    int nr7 = 2;
    struct range r7[] = { {10, 19}, {30, 39} };
    range_add(r7, &nr7, 16, 20, 29);
    TEST("range_add: adjacent merges into one",
         ranges_match(r7, nr7, (uint64_t[]){10,39}, 1));

    /* 8. Merge multiple ranges */
    int nr8 = 4;
    struct range r8[] = { {10,20}, {30,40}, {50,60}, {70,80} };
    range_add(r8, &nr8, 16, 25, 65);
    TEST("range_add: covers multiple, merges all",
         ranges_match(r8, nr8, (uint64_t[]){10,20,25,65,70,80}, 3));

    /* 9. Add single point (start == end) */
    int nr9 = 0;
    range_add(r, &nr9, 16, 42, 42);
    TEST("range_add: single point added",
         ranges_match(r, nr9, (uint64_t[]){42,42}, 1));

    /* 10. Add single point adjacent to existing — merges */
    range_add(r, &nr9, 16, 43, 43);
    TEST("range_add: adjacent point merges",
         ranges_match(r, nr9, (uint64_t[]){42,43}, 1));

    /* 11. Array full — add returns error */
    int nr11 = 0;
    struct range r11[2];
    range_add(r11, &nr11, 2, 10, 20);
    range_add(r11, &nr11, 2, 30, 40);
    int ret11 = range_add(r11, &nr11, 2, 50, 60);
    TEST("range_add: array full returns -1", ret11 < 0);
    TEST("range_add: nr unchanged when full", nr11 == 2);
}

/* ===================================================================
 *  test_range_remove
 * =================================================================== */
static void test_range_remove(void)
{
    /* 1. Remove from middle — splits */
    struct range r1[] = { {100, 200} };
    int nr1 = 1;
    range_remove(r1, &nr1, 125, 175);
    TEST("range_remove: splits into two fragments",
         ranges_match(r1, nr1, (uint64_t[]){100,124,176,200}, 2));

    /* 2. Remove left portion */
    struct range r2[] = { {100, 200} };
    int nr2 = 1;
    range_remove(r2, &nr2, 50, 150);
    TEST("range_remove: removes left portion",
         ranges_match(r2, nr2, (uint64_t[]){151,200}, 1));

    /* 3. Remove right portion */
    struct range r3[] = { {100, 200} };
    int nr3 = 1;
    range_remove(r3, &nr3, 150, 250);
    TEST("range_remove: removes right portion",
         ranges_match(r3, nr3, (uint64_t[]){100,149}, 1));

    /* 4. Remove entire range */
    struct range r4[] = { {100, 200} };
    int nr4 = 1;
    range_remove(r4, &nr4, 100, 200);
    TEST("range_remove: removes entire range", nr4 == 0);

    /* 5. No overlap */
    struct range r5[] = { {100, 200}, {300, 400} };
    int nr5 = 2;
    range_remove(r5, &nr5, 500, 600);
    TEST("range_remove: no overlap — unchanged",
         ranges_match(r5, nr5, (uint64_t[]){100,200,300,400}, 2));

    /* 6. Remove from one of multiple */
    struct range r6[] = { {100, 200}, {300, 400} };
    int nr6 = 2;
    range_remove(r6, &nr6, 150, 350);
    TEST("range_remove: spans two ranges",
         ranges_match(r6, nr6, (uint64_t[]){100,149,351,400}, 2));

    /* 7. Remove from middle of one, no split at boundaries */
    struct range r7[] = { {100, 200} };
    int nr7 = 1;
    range_remove(r7, &nr7, 100, 100);
    TEST("range_remove: single start point",
         ranges_match(r7, nr7, (uint64_t[]){101,200}, 1));

    /* 8. Empty array */
    struct range r8[1];
    int nr8 = 0;
    int ret = range_remove(r8, &nr8, 100, 200);
    TEST("range_remove: empty array returns 0", ret == 0 && nr8 == 0);
}

/* ===================================================================
 *  test_range_contains
 * =================================================================== */
static void test_range_contains(void)
{
    struct range r[] = { {100, 200}, {300, 400}, {500, 600} };
    int nr = 3;

    TEST("range_contains: exact start match",  range_contains(r, nr, 100));
    TEST("range_contains: exact end match",    range_contains(r, nr, 200));
    TEST("range_contains: middle of range",    range_contains(r, nr, 150));
    TEST("range_contains: below first range",  !range_contains(r, nr, 50));
    TEST("range_contains: between ranges",     !range_contains(r, nr, 250));
    TEST("range_contains: above last range",   !range_contains(r, nr, 700));
    TEST("range_contains: in second range",    range_contains(r, nr, 350));
    TEST("range_contains: in last range",      range_contains(r, nr, 550));

    /* Empty array */
    struct range e[1];
    TEST("range_contains: empty array", !range_contains(e, 0, 100));

    /* Single-element range with inverted bounds (start>end) — undefined but shouldn't crash */
    struct range inv[] = { {200, 100} };
    int r_inv = range_contains(inv, 1, 150);
    TEST("range_contains: inverted range handled", r_inv == 0 || r_inv == 1);
}

/* ===================================================================
 *  test_range_overlaps
 * =================================================================== */
static void test_range_overlaps(void)
{
    struct range r[] = { {100, 200}, {300, 400} };
    int nr = 2;

    TEST("range_overlaps: exact overlap",    range_overlaps(r, nr, 100, 200));
    TEST("range_overlaps: inside range",     range_overlaps(r, nr, 150, 180));
    TEST("range_overlaps: partial overlap",  range_overlaps(r, nr, 50, 150));
    TEST("range_overlaps: no overlap below", !range_overlaps(r, nr, 0, 50));
    TEST("range_overlaps: no overlap between", !range_overlaps(r, nr, 201, 299));
    TEST("range_overlaps: no overlap above", !range_overlaps(r, nr, 500, 600));
    TEST("range_overlaps: spans gap",        range_overlaps(r, nr, 150, 350));
    TEST("range_overlaps: adjacent — no",    !range_overlaps(r, nr, 201, 299));
    TEST("range_overlaps: single point",     range_overlaps(r, nr, 150, 150));

    /* Empty array */
    struct range e[1];
    TEST("range_overlaps: empty array", !range_overlaps(e, 0, 100, 200));

    /* Query with start>end */
    int r_inv = range_overlaps(r, nr, 400, 300);
    TEST("range_overlaps: inverted query", r_inv == 0 || r_inv == 1);
}

/* ===================================================================
 *  test_range_more — additional edge cases
 * =================================================================== */
static void test_range_more(void)
{
    /* 1. range_add: single point merges into adjacent left range */
    {
        int nr = 2;
        struct range r[16] = { {10, 19}, {30, 39} };
        range_add(r, &nr, 16, 20, 20);
        /* [20,20] adjacent to [10,19] → merges to [10,20].
         * [10,20] NOT adjacent to [30,39] (gap 21-29) → stays separate */
        TEST("range_add: single point merges adjacent left",
             ranges_match(r, nr, (uint64_t[]){10,20,30,39}, 2));
    }

    /* 2. range_add: merge many overlapping ranges into one */
    {
        int nr = 5;
        struct range r[16] = { {10,20}, {30,40}, {50,60}, {70,80}, {90,100} };
        range_add(r, &nr, 16, 15, 95);
        /* [15,95] overlaps all five — merges into one big [10,100] */
        TEST("range_add: merges many overlapping ranges",
             ranges_match(r, nr, (uint64_t[]){10,100}, 1));
    }

    /* 3. range_add: array near-capacity but merge reduces count */
    {
        int nr = 3;
        struct range r[8] = { {10,20}, {30,40}, {50,60} };
        range_add(r, &nr, 8, 5, 65);
        /* [5,65] overlaps all three → merges into [5,65] then sort+merge = single range */
        TEST("range_add: near-capacity but merge reduces",
             ranges_match(r, nr, (uint64_t[]){5,65}, 1));
    }

    /* 4. range_add: full array, no overlap — fails */
    {
        int nr = 2;
        struct range r[2] = { {10,20}, {30,40} };
        int ret = range_add(r, &nr, 2, 50, 60);
        TEST("range_add: full non-overlap returns -1", ret < 0);
        TEST("range_add: nr unchanged on full non-overlap", nr == 2);
    }

    /* 5. range_remove: remove single point from middle */
    {
        int nr = 1;
        struct range r[4] = { {100, 200} };
        range_remove(r, &nr, 150, 150);
        TEST("range_remove: single point splits into two",
             ranges_match(r, nr, (uint64_t[]){100,149,151,200}, 2));
    }

    /* 6. range_remove: start>end returns error */
    {
        int nr = 1;
        struct range r[4] = { {100, 200} };
        int ret = range_remove(r, &nr, 200, 100);
        TEST("range_remove: start>end returns <0", ret < 0);
        TEST("range_remove: nr unchanged after invalid", nr == 1);
    }

    /* 7. range_contains: boundary values */
    {
        struct range r[] = { {0, 0}, {100, 100}, {UINT64_MAX, UINT64_MAX} };
        int nr = 3;
        TEST("range_contains: point 0",     range_contains(r, nr, 0));
        TEST("range_contains: point 100",   range_contains(r, nr, 100));
        TEST("range_contains: UINT64_MAX",  range_contains(r, nr, UINT64_MAX));
        TEST("range_contains: 1 not in range", !range_contains(r, nr, 1));
        TEST("range_contains: 99 not in range", !range_contains(r, nr, 99));
    }

    /* 8. range_overlaps: single boundary point queries */
    {
        struct range r[] = { {100, 200}, {300, 400} };
        int nr = 2;
        TEST("range_overlaps: start point query", range_overlaps(r, nr, 100, 100));
        TEST("range_overlaps: end point query",   range_overlaps(r, nr, 200, 200));
        TEST("range_overlaps: exact above no overlap", !range_overlaps(r, nr, 201, 201));
        TEST("range_overlaps: exact below no overlap", !range_overlaps(r, nr, 99, 99));
    }

    /* 9. range_add: huge end (UINT64_MAX) */
    {
        int nr = 0;
        struct range r[16];
        range_add(r, &nr, 16, UINT64_MAX, UINT64_MAX);
        TEST("range_add: single point at UINT64_MAX",
             ranges_match(r, nr, (uint64_t[]){UINT64_MAX, UINT64_MAX}, 1));
    }
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== Range Management Tests ===\n\n");

    printf("--- range_sort ---\n");
    test_range_sort();

    printf("\n--- range_add ---\n");
    test_range_add();

    printf("\n--- range_remove ---\n");
    test_range_remove();

    printf("\n--- range_contains ---\n");
    test_range_contains();

    printf("\n--- range_overlaps ---\n");
    test_range_overlaps();

    printf("\n--- more edge cases ---\n");
    test_range_more();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
