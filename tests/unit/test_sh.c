/*
 * test_sh.c — Host-side unit tests for the POSIX shell (userspace/bin/sh.c)
 *
 * The shell is the D280 userspace centerpiece: tokenizer, arithmetic $(()),
 * parameter expansion, the [ / test builtin, and the line executor. A real OS
 * shell ships with a test suite, and CI must exercise it — but booting QEMU to
 * test a tokenizer is absurdly slow. Instead this harness compiles the *real*
 * sh.c on the host (guard SH_UNIT_TEST drops the freestanding-libc shims and
 * main()), then drives its actual internal functions directly. No behavior in
 * the shipped shell changes.
 *
 * Runs entirely on the host — no kernel dependencies.
 *
 * Compile:  gcc -Wall -Werror -g -O0 -o test_sh test_sh.c
 * Run:      ./test_sh
 */

/* Pull in the shell sources as compiled unit under test. */
#ifndef SH_UNIT_TEST
#define SH_UNIT_TEST
#endif

/* Host-compat prelude: sh.c is written against the OS freestanding libc, which
 * supplies these directly. Under SH_UNIT_TEST we pull them from glibc + the
 * Linux userspace API so the real sh.c compiles unchanged on the host. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/syscall.h>

/* getdents64 is the raw directory-reading syscall the OS libc wraps; glibc has
 * no public getdents64(), so declare it. Signature matches the kernel ABI. */
static inline int host_getdents64(int fd, void *buf, unsigned int count) {
    return (int)syscall(SYS_getdents64, fd, buf, count);
}
#define getdents64 host_getdents64

#include "../../userspace/bin/sh.c"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 *  Test framework
 * =================================================================== */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)  do {                        \
    tests_run++;                                \
    printf("  TEST: %-50s ... ", name);         \
} while (0)

#define PASS()      do {                        \
    tests_passed++;                             \
    printf("PASS\n");                           \
} while (0)

#define FAIL(msg)   do {                        \
    tests_failed++;                             \
    printf("FAIL\n");                           \
    printf("        %s\n", msg);                \
} while (0)

#define CHECK(cond, msg) do {                   \
    if (!(cond)) { FAIL(msg); return; }         \
} while (0)

#define CHECK_INT(got, exp, msg) do {           \
    long _g = (long)(got), _e = (long)(exp);    \
    if (_g != _e) {                             \
        tests_failed++;                         \
        printf("FAIL\n");                       \
        printf("        %s\n", msg);            \
        printf("        Expected: %ld\n", _e);  \
        printf("        Got:      %ld\n", _g);  \
        return;                                 \
    }                                           \
} while (0)

#define CHECK_STR(got, exp, msg) do {           \
    const char *_g = (got), *_e = (exp);        \
    if (_g == NULL || strcmp(_g, _e) != 0) {    \
        tests_failed++;                         \
        printf("FAIL\n");                       \
        printf("        %s\n", msg);            \
        printf("        Expected: \"%s\"\n", _e ? _e : "(null)"); \
        printf("        Got:      \"%s\"\n", _g ? _g : "(null)"); \
        return;                                 \
    }                                           \
} while (0)

/* ===================================================================
 *  Environment: sh_getenv / sh_setenv / sh_unsetenv
 * =================================================================== */

static void test_env(void) {
    TEST("env: set/get/unset round-trips");
    sh_init_env();
    CHECK_STR(sh_getenv("PATH"), "/bin", "default PATH present");
    CHECK(sh_setenv("FOO", "bar") == 0, "setenv returns 0");
    CHECK_STR(sh_getenv("FOO"), "bar", "getenv sees new var");
    CHECK(sh_setenv("FOO", "baz") == 0, "setenv overwrites");
    CHECK_STR(sh_getenv("FOO"), "baz", "getenv sees overwrite");
    CHECK(sh_unsetenv("FOO") == 0, "unset returns 0");
    CHECK(sh_getenv("FOO") == NULL, "getenv empty after unset");
    CHECK(sh_unsetenv("NOPE") != 0, "unset missing returns nonzero");
    /* existing env intact after churn */
    CHECK_STR(sh_getenv("HOME"), "/", "HOME preserved");
    PASS();
}

/* ===================================================================
 *  Tokenizer: quote/operator handling
 * =================================================================== */

static void test_tokenize(void) {
    TEST("tokenize: simple words + pipe + redirect");
    struct token toks[64];
    int nt = tokenize("echo hi | cat > out", toks, 64);
    CHECK(nt > 0, "tokenize returns tokens");
    /* echo, hi, |, cat, >, out  = 6 */
    CHECK_INT(nt, 6, "six tokens for 'echo hi | cat > out'");
    CHECK_STR(toks[0].text, "echo", "tok0 = echo");
    CHECK(toks[2].is_op && toks[2].op == '|', "tok2 is pipe op");
    CHECK(toks[4].is_op && toks[4].op == '>' && !toks[4].is_fd2, "tok4 is >");
    PASS();

    TEST("tokenize: single-quoted word is one token");
    nt = tokenize("echo 'a b c'", toks, 64);
    CHECK_INT(nt, 2, "two tokens (echo + quoted)");
    CHECK_STR(toks[1].text, "a b c", "quote content preserved verbatim");
    PASS();

    TEST("tokenize: && || ; & operators classified");
    nt = tokenize("a && b || c ; d &", toks, 64);
    /* a && b || c ; d & = 9 tokens */
    CHECK_INT(nt, 8, "eight tokens for compound line");
    CHECK(toks[1].is_andand, "tok1 is &&");
    CHECK(toks[3].is_oror, "tok3 is ||");
    CHECK(toks[5].is_op && toks[5].op == ';', "tok5 is ;");
    CHECK(toks[7].is_op && toks[7].op == '&', "tok7 is &");
    PASS();

    TEST("tokenize: 2> stderr redirect classified");
    nt = tokenize("cmd 2> err", toks, 64);
    /* cmd, 2> (one operator token), err = 3 tokens */
    CHECK_INT(nt, 3, "three tokens for 'cmd 2> err'");
    CHECK(toks[1].is_op && toks[1].op == '>' && toks[1].is_fd2, "tok1 is 2>");
    PASS();
}

/* ===================================================================
 *  Arithmetic $(()) — arith_eval
 * =================================================================== */

static void test_arith(void) {
    TEST("arith: operator precedence + parens");
    const char *end;
    int ok = 1;
    CHECK_INT(arith_eval("1 + 2 * 3", &end, &ok), 7, "1+2*3 = 7 (precedence)");
    CHECK(ok, "ok set for valid expr");
    CHECK_INT(arith_eval("(1 + 2) * 3", &end, &ok), 9, "(1+2)*3 = 9");
    CHECK_INT(arith_eval("10 / 3", &end, &ok), 3, "integer division 10/3 = 3");
    CHECK_INT(arith_eval("10 % 3", &end, &ok), 1, "modulo 10%3 = 1");
    CHECK_INT(arith_eval("-5 + 2", &end, &ok), -3, "unary minus -5+2 = -3");
    CHECK_INT(arith_eval("2 + 3 == 5", &end, &ok), 1, "comparison truthy = 1");
    CHECK_INT(arith_eval("2 + 3 == 6", &end, &ok), 0, "comparison falsy = 0");
    PASS();

    TEST("arith: variable expansion via $name");
    sh_init_env();
    sh_setenv("N", "4");
    CHECK_INT(arith_eval("N * N", &end, &ok), 16, "N*N with N=4 = 16");
    CHECK_INT(arith_eval("$(N) + 1", &end, &ok), 5, "$(N)+1 = 5");
    PASS();
}

/* ===================================================================
 *  Parameter expansion — expand_word
 * =================================================================== */

static void test_expand(void) {
    TEST("expand: plain var + ${var:-default}");
    sh_init_env();
    sh_setenv("GREET", "hi");
    int fail = 0;
    char *r = expand_word("x${GREET}y", &fail);
    CHECK_STR(r, "xhiy", "${GREET} substituted inline");
    free(r);

    /* unset var with :- default */
    r = expand_word("${MISSING:-fallback}", &fail);
    CHECK_STR(r, "fallback", "${MISSING:-fallback} uses default");
    free(r);

    /* set var with :- default keeps value */
    sh_setenv("SET", "real");
    r = expand_word("${SET:-fallback}", &fail);
    CHECK_STR(r, "real", "${SET:-fallback} keeps value");
    free(r);

    /* ${#var} length */
    r = expand_word("${#GREET}", &fail);
    CHECK_STR(r, "2", "${#GREET} length = 2");
    free(r);
    PASS();
}

/* ===================================================================
 *  test / [ builtin — eval_test_binary
 * =================================================================== */

static void test_eval_test(void) {
    TEST("test: string and integer comparisons");
    CHECK(eval_test_binary("=", "abc", "abc") == 1, "'=' equal true");
    CHECK(eval_test_binary("=", "abc", "abd") == 0, "'=' diff false");
    CHECK(eval_test_binary("!=", "a", "b") == 1, "'!=' true");
    CHECK(eval_test_binary("-eq", "5", "5") == 1, "-eq true");
    CHECK(eval_test_binary("-eq", "5", "6") == 0, "-eq false");
    CHECK(eval_test_binary("-lt", "3", "9") == 1, "-lt true");
    CHECK(eval_test_binary("-gt", "9", "3") == 1, "-gt true");
    CHECK(eval_test_binary("-le", "3", "3") == 1, "-le equal true");
    CHECK(eval_test_binary("-ge", "3", "3") == 1, "-ge equal true");
    CHECK(eval_test_binary("-ne", "1", "2") == 1, "-ne true");
    PASS();
}

/* ===================================================================
 *  End-to-end line execution (real run_line)
 * =================================================================== */

static void test_run_line(void) {
    TEST("run_line: export + $() + arithmetic assignment");
    sh_init_env();
    int rc = run_line("export FOO=42");
    CHECK_INT(rc, 0, "export returns 0");
    CHECK_STR(sh_getenv("FOO"), "42", "FOO exported");

    /* arithmetic assignment into a var, then use it */
    rc = run_line("BAR=$((FOO + 8))");
    CHECK_INT(rc, 0, "arith assign returns 0");
    CHECK_STR(sh_getenv("BAR"), "50", "BAR = 42+8 = 50");

    /* default expansion in a command context via env */
    rc = run_line("export MIA=${NOPE:-def}");
    CHECK_INT(rc, 0, "export with default returns 0");
    CHECK_STR(sh_getenv("MIA"), "def", "MIA defaulted to 'def'");

    /* [ ] builtin truthy -> exit 0 */
    rc = run_line("[ 5 -eq 5 ]");
    CHECK_INT(rc, 0, "[ 5 -eq 5 ] exit 0");

    /* [ ] builtin falsy -> exit 1 */
    rc = run_line("[ 5 -eq 6 ]");
    CHECK_INT(rc, 1, "[ 5 -eq 6 ] exit 1");
    PASS();

    TEST("run_line: unknown builtin/command returns nonzero");
    rc = run_line("this_is_not_a_command_abc");
    /* external exec will fail (not found) -> 127, or builtin path fails */
    CHECK(rc != 0, "missing command does not return 0");
    PASS();
}

/* ===================================================================
 *  main
 * =================================================================== */

int main(void) {
    printf("=== POSIX shell (D280) unit tests ===\n\n");
    test_env();
    test_tokenize();
    test_arith();
    test_expand();
    test_eval_test();
    test_run_line();

    printf("\n=== shell unit test summary: %d run, %d passed, %d failed ===\n",
           tests_run, tests_passed, tests_failed);
    if (tests_failed != 0) {
        printf("SOME TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
