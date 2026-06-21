/*
 * test_cmdline.c — Host-side tests for kernel command-line parsing.
 *
 * Tests cmdline_init, cmdline_has, cmdline_get, cmdline_get_int, cmdline_raw
 * from src/kernel/cmdline.c.
 *
 * Compile: part of host_libc test suite (via Makefile)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===================================================================
 *  Kernel API declarations
 * =================================================================== */

extern void cmdline_init(const char *cmdline);
extern int  cmdline_has(const char *key);
extern const char *cmdline_get(const char *key);
extern int  cmdline_get_int(const char *key, int default_val);
extern const char *cmdline_raw(void);

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
 *  Test: cmdline parsing
 * =================================================================== */

static void test_cmdline(void)
{
    printf("\n[cmdline]\n");

    /* 1. NULL init */
    cmdline_init(NULL);
    TEST("cmdline_raw after NULL init",
         cmdline_raw() != NULL && cmdline_raw()[0] == '\0');
    TEST("cmdline_has after NULL init returns 0",
         cmdline_has("anything") == 0);
    TEST("cmdline_get after NULL init returns NULL",
         cmdline_get("anything") == NULL);
    TEST("cmdline_get_int after NULL init returns default",
         cmdline_get_int("anything", 42) == 42);

    /* 2. Simple key=value */
    cmdline_init("root=/dev/sda1");
    TEST("cmdline: has key 'root'", cmdline_has("root") == 1);
    TEST("cmdline: get 'root' value", cmdline_get("root") != NULL &&
         strcmp(cmdline_get("root"), "/dev/sda1") == 0);
    TEST("cmdline_raw returns raw string",
         cmdline_raw() != NULL &&
         strcmp(cmdline_raw(), "root=/dev/sda1") == 0);

    /* 3. Bool flag (key without =) */
    cmdline_init("quiet");
    TEST("cmdline: bool flag present", cmdline_has("quiet") == 1);
    TEST("cmdline: bool flag value is empty string",
         cmdline_get("quiet") != NULL && cmdline_get("quiet")[0] == '\0');

    /* 4. Multiple params */
    cmdline_init("root=/dev/sda1 quiet mem=2048M debug");
    TEST("cmdline: multi has root", cmdline_has("root") == 1);
    TEST("cmdline: multi has quiet", cmdline_has("quiet") == 1);
    TEST("cmdline: multi has mem", cmdline_has("mem") == 1);
    TEST("cmdline: multi has debug", cmdline_has("debug") == 1);
    TEST("cmdline: multi missing param", cmdline_has("nope") == 0);
    TEST("cmdline: multi get root val",
         strcmp(cmdline_get("root"), "/dev/sda1") == 0);
    TEST("cmdline: multi get mem val",
         strcmp(cmdline_get("mem"), "2048M") == 0);

    /* 5. get_int */
    cmdline_init("timeout=30");
    TEST("cmdline_get_int: timeout=30", cmdline_get_int("timeout", 0) == 30);
    TEST("cmdline_get_int: missing returns default",
         cmdline_get_int("nonexistent", 99) == 99);
    cmdline_init("timeout=-5");
    TEST("cmdline_get_int: negative value",
         cmdline_get_int("timeout", 0) == -5);

    /* 6. get_int with non-numeric value (returns parsed prefix, 0 for no digits) */
    cmdline_init("mode=fast");
    TEST("cmdline_get_int: non-numeric returns 0",
         cmdline_get_int("mode", 1) == 0);

    /* 7. Very long key/value */
    {
        char long_cmdline[1024];
        char long_key[128];
        char long_val[512];
        memset(long_key, 'A', 60);
        long_key[60] = '\0';
        memset(long_val, 'B', 200);
        long_val[200] = '\0';
        snprintf(long_cmdline, sizeof(long_cmdline), "%s=%s", long_key, long_val);
        cmdline_init(long_cmdline);
        TEST("cmdline: very long key parsed",
             cmdline_has(long_key) == 1);
        const char *v = cmdline_get(long_key);
        TEST("cmdline: very long val not NULL", v != NULL);
        if (v) {
            /* Value may be truncated to CMDLINE_MAX_VAL-1 (255) */
            TEST("cmdline: very long val len > 0", strlen(v) > 0);
        }
    }

    /* 8. cmdline_raw after multi-param init */
    cmdline_init("a=1 b=2 c=3");
    TEST("cmdline_raw: multi-param preserves input",
         cmdline_raw() != NULL &&
         strcmp(cmdline_raw(), "a=1 b=2 c=3") == 0);

    /* 9. Empty string init */
    cmdline_init("");
    TEST("cmdline: empty init raw",
         cmdline_raw() != NULL && cmdline_raw()[0] == '\0');
    TEST("cmdline: empty init has any key false", cmdline_has("x") == 0);
}

/* ===================================================================
 *  Main
 * =================================================================== */

int main(void)
{
    printf("=== Kernel Command-Line Parsing Tests ===\n");
    test_cmdline();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
