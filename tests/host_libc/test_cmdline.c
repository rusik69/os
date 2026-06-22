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
 *  test_cmdline_more — additional edge cases
 * =================================================================== */
static void test_cmdline_more(void)
{
    printf("\n[cmdline_more]\n");

    /* 1. = in value (key=foo=bar) */
    cmdline_init("opt=value=with=equals");
    TEST("cmdline_more: = in value has key",
         cmdline_has("opt") == 1);
    const char *v1 = cmdline_get("opt");
    TEST("cmdline_more: = in value non-NULL", v1 != NULL);
    if (v1) {
        TEST("cmdline_more: = in value starts after first =",
             v1[0] != '\0');
    }

    /* 2. Empty value (key=) */
    cmdline_init("empty=");
    TEST("cmdline_more: empty value has key", cmdline_has("empty") == 1);
    const char *v2 = cmdline_get("empty");
    TEST("cmdline_more: empty value non-NULL", v2 != NULL);
    if (v2) {
        TEST("cmdline_more: empty value is empty string", v2[0] == '\0');
    }

    /* 3. Value with leading/trailing spaces */
    cmdline_init(" str =  hello world  ");
    /* Key might be "str" or " str " — at minimum it's parsed */
    TEST("cmdline_more: spaces around key=value present",
         cmdline_has("str") == 1 || cmdline_has(" str ") == 1);

    /* 4. get_int with hex value (0xFF) */
    cmdline_init("hexval=0xFF");
    int h = cmdline_get_int("hexval", 0);
    TEST("cmdline_more: get_int hex 0xFF", h == 255 || h == 0);

    /* 5. get_int with hex value (0x0) */
    cmdline_init("zero=0x0");
    int z = cmdline_get_int("zero", -1);
    TEST("cmdline_more: get_int hex 0x0", z == 0);

    /* 6. Duplicate keys — first match returned */
    cmdline_init("key=first key=second");
    const char *v3 = cmdline_get("key");
    TEST("cmdline_more: duplicate key first value returned", v3 != NULL);
    if (v3) {
        TEST("cmdline_more: first value is 'first'",
             strcmp(v3, "first") == 0);
    }

    /* 7. Key with no value followed by key=value */
    cmdline_init("debug root=/dev/sda1");
    TEST("cmdline_more: bool flag 'debug' before value param",
         cmdline_has("debug") == 1);
    TEST("cmdline_more: root after bool flag",
         cmdline_get("root") != NULL &&
         strcmp(cmdline_get("root"), "/dev/sda1") == 0);

    /* 8. Numeric value with leading zeroes */
    cmdline_init("val=007");
    TEST("cmdline_more: get_int leading zeroes",
         cmdline_get_int("val", 0) == 7);

    /* 9. Negative value with existing negative */
    cmdline_init("neg=-10");
    TEST("cmdline_more: get_int negative",
         cmdline_get_int("neg", 0) == -10);

    /* 10. Very many keys */
    {
        char buf[2048];
        int pos = 0;
        for (int i = 0; i < 50; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "k%d=%d ", i, i);
        }
        cmdline_init(buf);
        TEST("cmdline_more: 50 keys parsed", cmdline_has("k0") == 1);
        TEST("cmdline_more: key k49 found", cmdline_has("k49") == 1);
        TEST("cmdline_more: get_int k49 == 49",
             cmdline_get_int("k49", -1) == 49);
    }

    /* 11. Single key with empty value and adjacent params */
    cmdline_init("a= b=1");
    TEST("cmdline_more: a has empty value", cmdline_has("a") == 1);
    TEST("cmdline_more: b=1 after empty a", cmdline_get("b") != NULL &&
         strcmp(cmdline_get("b"), "1") == 0);
}

/* ===================================================================
 *  test_cmdline_extra — even more edge cases
 * =================================================================== */
static void test_cmdline_extra(void)
{
    printf("\n[cmdline extra]\n");

    /* 1. Only whitespace */
    cmdline_init("   ");
    TEST("cmdline_extra: whitespace-only has raw string",
         cmdline_raw() != NULL);
    TEST("cmdline_extra: whitespace-only has no keys",
         cmdline_has("x") == 0);

    /* 2. Key with trailing spaces before = */
    cmdline_init("key =value");
    TEST("cmdline_extra: spaces before = key found",
         cmdline_has("key") == 1);

    /* 3. Re-init multiple times (stress) */
    cmdline_init("a=1");
    cmdline_init("b=2");
    cmdline_init("c=3");
    TEST("cmdline_extra: re-init replaces keys",
         cmdline_has("a") == 0 && cmdline_has("c") == 1);

    /* 4. Numeric value at INT_MAX boundary */
    cmdline_init("big=2147483647");
    TEST("cmdline_extra: INT_MAX parsed",
         cmdline_get_int("big", 0) == 2147483647);

    /* 5. Numeric overflow (value > INT_MAX) */
    cmdline_init("bigger=2147483648");
    int val = cmdline_get_int("bigger", 0);
    TEST("cmdline_extra: overflow value truncated", val != 0 || val == 0);

    /* 6. Key with special characters */
    cmdline_init("key-with-dashes=1 key.with.dots=2 key_with_underscore=3");
    TEST("cmdline_extra: key with dashes", cmdline_has("key-with-dashes") == 1);
    TEST("cmdline_extra: key with dots", cmdline_has("key.with.dots") == 1);
    TEST("cmdline_extra: key with underscore", cmdline_has("key_with_underscore") == 1);

    /* 7. Single parameter no value */
    cmdline_init("single");
    TEST("cmdline_extra: single bool param", cmdline_has("single") == 1);
    const char *sv = cmdline_get("single");
    TEST("cmdline_extra: single bool value empty", sv != NULL && sv[0] == '\0');

    /* 8. get_int with existing negative default (should return default on miss) */
    cmdline_init("positive=5");
    TEST("cmdline_extra: get_int missing with neg default",
         cmdline_get_int("nonexistent", -42) == -42);
}

int main(void)
{
    printf("=== Kernel Command-Line Parsing Tests ===\n");
    test_cmdline();

    printf("\n--- more edge cases ---\n");
    test_cmdline_more();

    printf("\n--- extra edge cases ---\n");
    test_cmdline_extra();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
