/*
 * test_errno_ext.c — Host-side tests for kernel errno string functions
 *
 * Tests strerror() from src/lib/errno_ext.c.
 * Compiles against the errno_ext.c source.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 *  Kernel function prototypes
 * =================================================================== */
extern char *strerror(int errnum);
extern void perror(const char *s);

/* ===================================================================
 *  Stubs
 * =================================================================== */
void vga_putchar(char c)      { (void)c; }
void serial_putchar(char c)   { (void)c; }
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
int console_loglevel = 7;
int default_message_loglevel = 6;

/* __errno_value and __errno_location are defined in errno_ext.c */

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
 *  test_strerror
 * =================================================================== */
static void test_strerror(void)
{
    /* 1. strerror(0) == "Success" */
    char *s = strerror(0);
    TEST("strerror(0) = Success", strcmp(s, "Success") == 0);

    /* 2. strerror(EPERM) == "Operation not permitted" */
    s = strerror(1);  /* EPERM = 1 */
    TEST("strerror(EPERM) = Operation not permitted",
         strcmp(s, "Operation not permitted") == 0);

    /* 3. strerror(ENOENT) == "No such file or directory" */
    s = strerror(2);  /* ENOENT = 2 */
    TEST("strerror(ENOENT) = No such file or directory",
         strcmp(s, "No such file or directory") == 0);

    /* 4. strerror(EINVAL) == "Invalid argument" */
    s = strerror(22);  /* EINVAL = 22 */
    TEST("strerror(EINVAL) = Invalid argument",
         strcmp(s, "Invalid argument") == 0);

    /* 5. strerror(ENOMEM) == "Out of memory" */
    s = strerror(12);  /* ENOMEM = 12 */
    TEST("strerror(ENOMEM) = Out of memory",
         strcmp(s, "Out of memory") == 0);

    /* 6. strerror(EACCES) == "Permission denied" */
    s = strerror(13);  /* EACCES = 13 */
    TEST("strerror(EACCES) = Permission denied",
         strcmp(s, "Permission denied") == 0);

    /* 7. strerror(999) == "Unknown error 999" */
    s = strerror(999);
    TEST("strerror(999) = Unknown error 999",
         strcmp(s, "Unknown error 999") == 0);

    /* 8. strerror(-1) == "Unknown error -1" */
    s = strerror(-1);
    TEST("strerror(-1) = Unknown error -1",
         strcmp(s, "Unknown error -1") == 0);

    /* 9. strerror(9999) == "Unknown error 9999" */
    s = strerror(9999);
    TEST("strerror(9999) = Unknown error 9999",
         strcmp(s, "Unknown error 9999") == 0);

    /* 10. strerror(ERANGE) == "Math result not representable" */
    s = strerror(34);  /* ERANGE = 34 */
    TEST("strerror(ERANGE) = Math result not representable",
         strcmp(s, "Math result not representable") == 0);

    /* 11. strerror(ENOSYS) == "Function not implemented" */
    s = strerror(38);  /* ENOSYS = 38 */
    TEST("strerror(ENOSYS) = Function not implemented",
         strcmp(s, "Function not implemented") == 0);

    /* 12. strerror negative large value */
    s = strerror(-9999);
    TEST("strerror(-9999) = Unknown error -9999",
         strcmp(s, "Unknown error -9999") == 0);
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== Errno String Tests ===\n\n");

    printf("--- strerror ---\n");
    test_strerror();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
