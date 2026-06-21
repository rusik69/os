/*
 * test_signal_validate.c — Host-side tests for kernel signal validation.
 *
 * Tests signal_validate_siginfo from src/kernel/signal_validate.c.
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

/* Signal numbers (from signal.h) */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGSYS    31
#define SIGRTMIN  32
#define SIGRTMAX  64
#define SIG_MAX   65

/* si_code values */
#define SI_USER     0
#define SI_KERNEL   1
#define SI_QUEUE    2
#define SI_TKILL    3

/* Signal-specific si_code values */
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define BUS_ADRERR  3
#define CLD_EXITED     1
#define CLD_KILLED     2
#define CLD_DUMPED     3
#define CLD_TRAPPED    4
#define CLD_STOPPED    5
#define CLD_CONTINUED  6

struct siginfo {
    int      si_signo;
    int      si_errno;
    int      si_code;
    uint32_t si_pid;
    uint32_t si_uid;
    void    *si_addr;
    int      si_status;
};

/* Errno */
#define EPERM   1
#define EINVAL  22

/* ===================================================================
 *  Kernel API declarations
 * =================================================================== */

extern int signal_validate_siginfo(struct siginfo *info, int is_from_userspace);

/* ===================================================================
 *  Stubs for kernel symbols
 * =================================================================== */

void vga_putchar(char c)      { (void)c; }
void serial_putchar(char c)   { (void)c; }
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
int console_loglevel = 7;
int default_message_loglevel = 6;

/* kptr_restrict stub (signal_validate.c references this header) */
int kptr_restrict = 1;  /* restricted by default */

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
 *  Test: signal_validate_siginfo
 * =================================================================== */

static void test_signal_validate(void)
{
    printf("\n[signal_validate]\n");

    struct siginfo info;
    int ret;

    /* 1. NULL info → returns 0 */
    ret = signal_validate_siginfo(NULL, 0);
    TEST("signal_validate: NULL info returns 0", ret == 0);
    ret = signal_validate_siginfo(NULL, 1);
    TEST("signal_validate: NULL info (userspace) returns 0", ret == 0);

    /* 2. Invalid signo (0) → -EINVAL */
    memset(&info, 0, sizeof(info));
    info.si_signo = 0;
    info.si_code = SI_USER;
    ret = signal_validate_siginfo(&info, 0);
    TEST("signal_validate: signo==0 returns -EINVAL", ret == -EINVAL);

    /* 3. Invalid signo (>= SIG_MAX) → -EINVAL */
    info.si_signo = SIG_MAX;
    ret = signal_validate_siginfo(&info, 0);
    TEST("signal_validate: signo>=SIG_MAX returns -EINVAL", ret == -EINVAL);

    /* 4. Valid signo with SI_USER from kernel → OK (0) */
    info.si_signo = SIGTERM;
    info.si_code = SI_USER;
    info.si_pid = 0;
    info.si_uid = 0;
    ret = signal_validate_siginfo(&info, 0);
    TEST("signal_validate: SI_USER from kernel returns 0", ret == 0);

    /* 5. SI_USER from userspace → -EPERM */
    info.si_signo = SIGTERM;
    info.si_code = SI_USER;
    ret = signal_validate_siginfo(&info, 1);
    TEST("signal_validate: SI_USER from userspace returns -EPERM",
         ret == -EPERM);

    /* 6. SIGSEGV with unknown si_code > 0 → clamped to SEGV_MAPERR */
    info.si_signo = SIGSEGV;
    info.si_code = 99;  /* unknown positive si_code */
    info.si_addr = (void *)(uintptr_t)0x1234;
    ret = signal_validate_siginfo(&info, 0);
    TEST("signal_validate: SIGSEGV unknown si_code clamped",
         ret == 0 && info.si_code == SEGV_MAPERR);

    /* 7. SIGSEGV with si_code=0 (SI_USER) is OK */
    info.si_signo = SIGSEGV;
    info.si_code = SI_USER;
    ret = signal_validate_siginfo(&info, 0);
    TEST("signal_validate: SIGSEGV SI_USER stays as-is",
         ret == 0 && info.si_code == SI_USER);

    /* 8. SIGCHLD with invalid si_code → clamped to CLD_EXITED */
    info.si_signo = SIGCHLD;
    info.si_code = 99;
    ret = signal_validate_siginfo(&info, 0);
    TEST("signal_validate: SIGCHLD unknown si_code clamped",
         ret == 0 && info.si_code == CLD_EXITED);

    /* 9. SIGCHLD with valid si_code (CLD_KILLED) stays */
    info.si_signo = SIGCHLD;
    info.si_code = CLD_KILLED;
    ret = signal_validate_siginfo(&info, 0);
    TEST("signal_validate: SIGCHLD CLD_KILLED stays",
         ret == 0 && info.si_code == CLD_KILLED);

    /* 10. SIGBUS with unknown si_code → clamped to BUS_ADRERR */
    info.si_signo = SIGBUS;
    info.si_code = 99;
    ret = signal_validate_siginfo(&info, 0);
    TEST("signal_validate: SIGBUS unknown si_code clamped",
         ret == 0 && info.si_code == BUS_ADRERR);

    /* 11. SI_TKILL from userspace is allowed */
    info.si_signo = SIGTERM;
    info.si_code = SI_TKILL;
    ret = signal_validate_siginfo(&info, 1);
    TEST("signal_validate: SI_TKILL from userspace OK",
         ret == 0);

    /* 12. SI_QUEUE from userspace is allowed */
    info.si_signo = SIGTERM;
    info.si_code = SI_QUEUE;
    ret = signal_validate_siginfo(&info, 1);
    TEST("signal_validate: SI_QUEUE from userspace OK",
         ret == 0);
}

/* ===================================================================
 *  Main
 * =================================================================== */

int main(void)
{
    printf("=== Kernel Signal Validation Tests ===\n");
    test_signal_validate();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
