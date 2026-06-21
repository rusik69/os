/*
 * test_sched_attr.c — Host-side tests for kernel sched_setattr validation.
 *
 * Tests the validation portion of sched_setattr from src/kernel/sched_attr.c
 * — null attr, invalid policy, invalid priority, invalid nice, deadline
 * constraints.
 *
 * Compile: part of host_libc test suite (via Makefile)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===================================================================
 *  Kernel type + constant declarations
 * =================================================================== */

/* Scheduling policies (from scheduler.h) */
#define SCHED_OTHER     0
#define SCHED_FIFO      1
#define SCHED_RR        2
#define SCHED_BATCH     3
#define SCHED_DEADLINE  4
#define SCHED_IDLE      5

struct sched_attr {
    size_t   size;
    int      sched_policy;
    uint64_t sched_flags;
    int      sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime;
    uint64_t sched_deadline;
    uint64_t sched_period;
};

/* Errno values */
#define EFAULT  14
#define EINVAL  22
#define ESRCH   3
#define EBUSY   16

/* ===================================================================
 *  Kernel API declarations
 * =================================================================== */

extern int sched_setattr(uint32_t pid, const struct sched_attr *attr, uint32_t flags);

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

/* ===================================================================
 *  Test: sched_setattr validation
 * =================================================================== */

static void test_sched_attr_validation(void)
{
    printf("\n[sched_attr]\n");

    struct sched_attr attr;
    int ret;

    /* 1. NULL attr → -EFAULT */
    ret = sched_setattr(1, NULL, 0);
    TEST("sched_setattr: NULL attr returns -EFAULT", ret == -EFAULT);

    /* 2. size == 0 → -EINVAL */
    memset(&attr, 0, sizeof(attr));
    attr.size = 0;
    ret = sched_setattr(1, &attr, 0);
    TEST("sched_setattr: size==0 returns -EINVAL", ret == -EINVAL);

    /* 3. size > sizeof(struct sched_attr) → -EINVAL */
    attr.size = sizeof(struct sched_attr) + 4;
    ret = sched_setattr(1, &attr, 0);
    TEST("sched_setattr: size too large returns -EINVAL", ret == -EINVAL);

    /* 4. Invalid policy (below SCHED_OTHER) */
    attr.size = sizeof(struct sched_attr);
    attr.sched_policy = -1;
    ret = sched_setattr(1, &attr, 0);
    TEST("sched_setattr: policy==-1 returns -EINVAL", ret == -EINVAL);

    /* 5. Invalid policy (above SCHED_IDLE) */
    attr.sched_policy = SCHED_IDLE + 1;
    ret = sched_setattr(1, &attr, 0);
    TEST("sched_setattr: policy>SCHED_IDLE returns -EINVAL", ret == -EINVAL);

    /* 6. Valid policies should pass validation (and return -ESRCH
     *    because process_get_by_pid returns NULL) */
    attr.sched_policy = SCHED_OTHER;
    attr.sched_priority = 0;
    attr.sched_nice = 0;
    ret = sched_setattr(1, &attr, 0);
    TEST("sched_setattr: SCHED_OTHER + valid params reaches process lookup",
         ret == -ESRCH);

    attr.sched_policy = SCHED_FIFO;
    attr.sched_priority = 50;
    attr.sched_nice = 0;
    ret = sched_setattr(1, &attr, 0);
    TEST("sched_setattr: SCHED_FIFO reaches process lookup",
         ret == -ESRCH);

    attr.sched_policy = SCHED_RR;
    ret = sched_setattr(1, &attr, 0);
    TEST("sched_setattr: SCHED_RR reaches process lookup",
         ret == -ESRCH);

    /* 7. Invalid priority (> 99) */
    attr.sched_policy = SCHED_FIFO;
    attr.sched_priority = 100;
    ret = sched_setattr(1, &attr, 0);
    TEST("sched_setattr: priority>99 returns -EINVAL", ret == -EINVAL);

    /* 8. Invalid nice value (< -20) */
    attr.sched_policy = SCHED_OTHER;
    attr.sched_priority = 0;
    attr.sched_nice = -21;
    ret = sched_setattr(1, &attr, 0);
    TEST("sched_setattr: nice<-20 returns -EINVAL", ret == -EINVAL);

    /* 9. Invalid nice value (> 19) */
    attr.sched_nice = 20;
    ret = sched_setattr(1, &attr, 0);
    TEST("sched_setattr: nice>19 returns -EINVAL", ret == -EINVAL);

    /* 10. Deadline constraints: runtime == 0 */
    attr.sched_policy = SCHED_DEADLINE;
    attr.sched_priority = 0;
    attr.sched_nice = 0;
    attr.sched_runtime = 0;
    attr.sched_deadline = 1000000;
    attr.sched_period = 2000000;
    ret = sched_setattr(1, &attr, 0);
    TEST("sched_setattr: DL runtime==0 returns -EINVAL", ret == -EINVAL);

    /* 11. Deadline constraints: deadline == 0 */
    attr.sched_runtime = 100000;
    attr.sched_deadline = 0;
    attr.sched_period = 2000000;
    ret = sched_setattr(1, &attr, 0);
    TEST("sched_setattr: DL deadline==0 returns -EINVAL", ret == -EINVAL);

    /* 12. Deadline constraints: runtime > deadline */
    attr.sched_runtime = 1000000;
    attr.sched_deadline = 500000;
    attr.sched_period = 2000000;
    ret = sched_setattr(1, &attr, 0);
    TEST("sched_setattr: DL runtime>deadline returns -EINVAL", ret == -EINVAL);

    /* 13. Deadline constraints: deadline > period */
    attr.sched_runtime = 100000;
    attr.sched_deadline = 3000000;
    attr.sched_period = 2000000;
    ret = sched_setattr(1, &attr, 0);
    TEST("sched_setattr: DL deadline>period returns -EINVAL", ret == -EINVAL);

    /* 14. Valid deadline params should pass validation */
    attr.sched_runtime = 100000;
    attr.sched_deadline = 500000;
    attr.sched_period = 2000000;
    ret = sched_setattr(1, &attr, 0);
    TEST("sched_setattr: valid DL reaches process lookup",
         ret == -ESRCH);
}

/* ===================================================================
 *  Main
 * =================================================================== */

int main(void)
{
    printf("=== Kernel Sched Attribute Validation Tests ===\n");
    test_sched_attr_validation();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
