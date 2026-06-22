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
extern int *__errno_location(void);
extern const char *errno_str(int err);
extern int errno_set(int err);

/* __errno_value is defined in errno_ext.c */
extern int __errno_value;

/* ===================================================================
 *  Stubs
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
 *  test_strerror_more — +12 new assertions (10+ required)
 * =================================================================== */
static void test_strerror_more(void)
{
    /* Use the kernel's errno value constants directly as literals
     * (test is compiled with TEST_CFLAGS, not KERNEL_CFLAGS, so
     *  kernel errno.h is not in the include path). */

    /* 1. EBADF = 9 → "Bad file number" */
    {
        char *s = strerror(9);
        TEST("strerror(EBADF) = Bad file number",
             strcmp(s, "Bad file number") == 0);
    }

    /* 2. EIO = 5 → "I/O error" */
    {
        char *s = strerror(5);
        TEST("strerror(EIO) = I/O error",
             strcmp(s, "I/O error") == 0);
    }

    /* 3. EFAULT = 14 → "Bad address" */
    {
        char *s = strerror(14);
        TEST("strerror(EFAULT) = Bad address",
             strcmp(s, "Bad address") == 0);
    }

    /* 4. ENODEV = 19 → "No such device" */
    {
        char *s = strerror(19);
        TEST("strerror(ENODEV) = No such device",
             strcmp(s, "No such device") == 0);
    }

    /* 5. ENOTDIR = 20 → "Not a directory" */
    {
        char *s = strerror(20);
        TEST("strerror(ENOTDIR) = Not a directory",
             strcmp(s, "Not a directory") == 0);
    }

    /* 6. EISDIR = 21 → "Is a directory" */
    {
        char *s = strerror(21);
        TEST("strerror(EISDIR) = Is a directory",
             strcmp(s, "Is a directory") == 0);
    }

    /* 7. EEXIST = 17 → "File exists" */
    {
        char *s = strerror(17);
        TEST("strerror(EEXIST) = File exists",
             strcmp(s, "File exists") == 0);
    }

    /* 8. EAGAIN = 11 → "Try again" */
    {
        char *s = strerror(11);
        TEST("strerror(EAGAIN) = Try again",
             strcmp(s, "Try again") == 0);
    }

    /* 9. EMLINK = 31 → "Too many links" */
    {
        char *s = strerror(31);
        TEST("strerror(EMLINK) = Too many links",
             strcmp(s, "Too many links") == 0);
    }

    /* 10. EWOULDBLOCK == EAGAIN (11) → "Try again" */
    {
        char *s = strerror(11);
        TEST("strerror(EWOULDBLOCK) = Try again",
             strcmp(s, "Try again") == 0);
    }

    /* 11. strerror(-0) — negative zero is zero in C, should be "Success" */
    {
        char *s = strerror(-0);
        TEST("strerror(-0) = Success",
             strcmp(s, "Success") == 0);
    }

    /* 12. strerror with very large errno value */
    {
        char *s = strerror(2000000);
        TEST("strerror(2000000) = Unknown error 2000000",
             strcmp(s, "Unknown error 2000000") == 0);
    }
}

/* ===================================================================
 *  test_strerror_full — cover all common errno values
 * =================================================================== */
static void test_strerror_full(void)
{
    printf("\n[strerror full]\n");

    /* Additional errno values */
    struct { int num; const char *expected; } errnos[] = {
        {3,    "No such process"},
        {4,    "Interrupted system call"},
        {6,    "No such device or address"},
        {7,    "Argument list too long"},
        {8,    "Exec format error"},
        {10,   "No child processes"},
        {16,   "Device or resource busy"},
        {18,   "Cross-device link"},
        {23,   "File table overflow"},
        {24,   "Too many open files"},
        {25,   "Not a typewriter"},
        {27,   "File too large"},
        {28,   "No space left on device"},
        {29,   "Illegal seek"},
        {30,   "Read-only file system"},
        {32,   "Broken pipe"},
        {34,   "Math result not representable"},
        {35,   "Resource deadlock would occur"},
        {36,   "File name too long"},
        {37,   "No record locks available"},
        {38,   "Function not implemented"},
        {39,   "Directory not empty"},
    };
    int all_match = 1;
    for (size_t i = 0; i < sizeof(errnos)/sizeof(errnos[0]); i++) {
        char *s = strerror(errnos[i].num);
        if (strcmp(s, errnos[i].expected) != 0) {
            printf("  FAIL: strerror(%d) = \"%s\" (expected \"%s\")\n",
                   errnos[i].num, s, errnos[i].expected);
            tests_failed++;
            all_match = 0;
        } else {
            printf("  PASS: strerror(%d) = \"%s\"\n",
                   errnos[i].num, s);
            tests_passed++;
        }
    }
    TEST("strerror: all common errnos match", all_match);
}

/* ===================================================================
 *  test_errno_api — test errno_set, errno_str, __errno_location, perror
 * =================================================================== */
static void test_errno_api(void)
{
    printf("\n[errno API]\n");

    /* Reset errno to known state */
    __errno_value = 0;

    /* 1. __errno_location returns valid pointer */
    {
        int *loc = __errno_location();
        TEST("__errno_location: returns non-NULL", loc != NULL);
        TEST("__errno_location: points to __errno_value", loc == &__errno_value);
    }

    /* 2. __errno_location returns same pointer each time */
    {
        int *loc1 = __errno_location();
        int *loc2 = __errno_location();
        TEST("__errno_location: consistent pointer", loc1 == loc2);
    }

    /* 3. errno_set(0) sets __errno_value to 0 */
    {
        __errno_value = 42;
        errno_set(0);
        TEST("errno_set(0) sets errno to 0", __errno_value == 0);
    }

    /* 4. errno_set(EPERM) sets to 1 */
    {
        errno_set(1);
        TEST("errno_set(EPERM) sets errno to 1", __errno_value == 1);
    }

    /* 5. errno_set(EINVAL) sets to 22 */
    {
        errno_set(22);
        TEST("errno_set(EINVAL) sets errno to 22", __errno_value == 22);
    }

    /* 6. errno_set returns 0 */
    {
        int ret = errno_set(0);
        TEST("errno_set returns 0", ret == 0);
    }

    /* 7. errno_str(0) = "Success" */
    {
        const char *s = errno_str(0);
        TEST("errno_str(0) = Success", strcmp(s, "Success") == 0);
    }

    /* 8. errno_str(EPERM) = "Operation not permitted" */
    {
        const char *s = errno_str(1);
        TEST("errno_str(EPERM) correct", strcmp(s, "Operation not permitted") == 0);
    }

    /* 9. errno_str(EINTR) = "Interrupted system call" */
    {
        const char *s = errno_str(4);
        TEST("errno_str(EINTR) correct", strcmp(s, "Interrupted system call") == 0);
    }

    /* 10. errno_str(999) = "Unknown error 999" */
    {
        const char *s = errno_str(999);
        TEST("errno_str(999) = Unknown error 999", strcmp(s, "Unknown error 999") == 0);
    }

    /* 11. errno_str(-7) = "Unknown error -7" */
    {
        const char *s = errno_str(-7);
        TEST("errno_str(-7) = Unknown error -7", strcmp(s, "Unknown error -7") == 0);
    }

    /* 12. perror with non-empty prefix (just check no crash) */
    {
        __errno_value = 2; /* ENOENT */
        perror("test_prefix");
        TEST("perror: non-empty prefix no crash", __errno_value == 2);
    }

    /* 13. perror with NULL prefix (no crash) */
    {
        __errno_value = 5; /* EIO */
        perror(NULL);
        TEST("perror: NULL prefix no crash", __errno_value == 5);
    }

    /* 14. perror with empty string prefix (no crash) */
    {
        __errno_value = 22;
        perror("");
        TEST("perror: empty prefix no crash", __errno_value == 22);
    }

    /* 15. errno_set then errno_str matches */
    {
        errno_set(1);  /* EPERM */
        const char *s = errno_str(__errno_value);
        TEST("errno_set + errno_str: EPERM", strcmp(s, "Operation not permitted") == 0);
    }

    /* 16. errno_set with large number, no crash */
    {
        errno_set(2000000);
        TEST("errno_set(2000000) no crash", 1);
        const char *s = errno_str(__errno_value);
        TEST("errno_str after errno_set(2000000)", strcmp(s, "Unknown error 2000000") == 0);
    }

    /* 17. errno_set(0) resets to Success */
    {
        errno_set(1);
        errno_set(0);
        const char *s = errno_str(__errno_value);
        TEST("errno_set(0) → errno_str = Success", strcmp(s, "Success") == 0);
    }

    /* 18. Chain: errno_set → __errno_location reads back same */
    {
        errno_set(13); /* EACCES */
        int *loc = __errno_location();
        TEST("__errno_location reads errno_set value", *loc == 13);
    }
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== Errno String Tests ===\n\n");

    printf("--- strerror ---\n");
    test_strerror();

    printf("\n--- strerror more ---\n");
    test_strerror_more();

    printf("\n--- strerror full ---\n");
    test_strerror_full();

    printf("\n--- errno API ---\n");
    test_errno_api();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
