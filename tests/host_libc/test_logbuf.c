/*
 * test_logbuf.c — Host-side tests for kernel log buffer.
 *
 * Tests logbuf_write, logbuf_read, logbuf_available from src/kernel/logbuf.c.
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

extern void     logbuf_write(const char *msg, uint32_t len);
extern uint32_t logbuf_read(char *buf, uint32_t max);
extern uint32_t logbuf_available(void);

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
 *  Test: logbuf
 * =================================================================== */

static void test_logbuf(void)
{
    printf("\n[logbuf]\n");
    char buf[65536];

    /* 1. Initially empty */
    {
        uint32_t avail = logbuf_available();
        TEST("logbuf: initially available == 0", avail == 0);
        uint32_t n = logbuf_read(buf, sizeof(buf));
        TEST("logbuf: initially read returns 0", n == 0);
    }

    /* 2. Write "hello" → available should be 5 */
    {
        logbuf_write("hello", 5);
        uint32_t avail = logbuf_available();
        TEST("logbuf: available == 5 after write 'hello'", avail == 5);
    }

    /* 3. Write then read back */
    {
        memset(buf, 0, sizeof(buf));
        uint32_t n = logbuf_read(buf, sizeof(buf));
        /* logbuf_read may append a newline */
        TEST("logbuf: read 'hello' returns >= 5", n >= 5);
        TEST("logbuf: read contains 'hello'",
             memcmp(buf, "hello", 5) == 0);
        TEST("logbuf: empty after reading all",
             logbuf_available() == 0);
    }

    /* 4. Write more than buffer wraps (logbuf is 32768) */
    {
        char big[40000];
        memset(big, 'A', 40000);
        logbuf_write(big, 40000);
        /* Log truncates to LOGBUF_SIZE/2 = 16384 per write */
        uint32_t avail = logbuf_available();
        TEST("logbuf: large write truncated to <= 16384",
             avail <= 16384 && avail > 0);
        /* Read it back */
        uint32_t n = logbuf_read(buf, sizeof(buf));
        TEST("logbuf: large write readable", n > 0);
        TEST("logbuf: empty after reading large write",
             logbuf_available() == 0);
    }

    /* 5. Write NULL (should be no-op) */
    {
        logbuf_write(NULL, 5);
        TEST("logbuf: available still 0 after NULL write",
             logbuf_available() == 0);
    }

    /* 6. Write with len=0 (should be no-op) */
    {
        logbuf_write("hello", 0);
        TEST("logbuf: available still 0 after len=0 write",
             logbuf_available() == 0);
    }

    /* 7. 1-byte-at-a-time, 100 writes */
    {
        for (int i = 0; i < 100; i++) {
            char c = (char)('a' + (i % 26));
            logbuf_write(&c, 1);
        }
        uint32_t avail = logbuf_available();
        TEST("logbuf: 100 one-byte writes available == 100",
             avail == 100);
        memset(buf, 0, sizeof(buf));
        uint32_t n = logbuf_read(buf, sizeof(buf));
        TEST("logbuf: read after 100 writes returns >= 100", n >= 100);
        /* Verify content */
        int ok = 1;
        for (int i = 0; i < 100 && i < (int)n; i++) {
            char expected = (char)('a' + (i % 26));
            if (buf[i] != expected) { ok = 0; break; }
        }
        TEST("logbuf: 100 writes content correct", ok == 1);
        TEST("logbuf: empty after reading 100 writes",
             logbuf_available() == 0);
    }

    /* 8. Write-read-write-read interleaved */
    {
        logbuf_write("AB", 2);
        TEST("logbuf: available 2 after 'AB'", logbuf_available() == 2);
        char tmp[4];
        uint32_t n = logbuf_read(tmp, 1);
        TEST("logbuf: partial read returns 1", n >= 1);
        /* remaining: 1 byte, but read appends newline */
        logbuf_write("CD", 2);
        /* After partial read (1 byte consumed) + newline appended
         * by read, we wrote 2 more bytes. */
        uint32_t avail = logbuf_available();
        TEST("logbuf: interleaved non-zero available", avail > 0);
        /* Drain */
        logbuf_read(buf, sizeof(buf));
        TEST("logbuf: drained", logbuf_available() == 0);
    }

    /* 9. Write max size message */
    {
        char big[16384];
        memset(big, 'X', 16384);
        logbuf_write(big, 16384);
        uint32_t avail = logbuf_available();
        TEST("logbuf: max size write available > 0", avail > 0);
        TEST("logbuf: max size write <= 16384", avail <= 16384);
        logbuf_read(buf, sizeof(buf));
        TEST("logbuf: drained after max write",
             logbuf_available() == 0);
    }

    /* 10. Write multiple small messages, ensure no data corruption */
    {
        logbuf_write("abc", 3);
        logbuf_write("def", 3);
        uint32_t n = logbuf_read(buf, sizeof(buf));
        /* Should contain both messages */
        int has_abc = (n >= 3 && memcmp(buf, "abc", 3) == 0);
        int has_def = 0;
        for (uint32_t i = 0; i + 3 <= n; i++) {
            if (memcmp(&buf[i], "def", 3) == 0) { has_def = 1; break; }
        }
        TEST("logbuf: multi-write contains 'abc'", has_abc);
        TEST("logbuf: multi-write contains 'def'", has_def);
        TEST("logbuf: drained after multi-write",
             logbuf_available() == 0);
    }
}

/* ===================================================================
 *  Main
 * =================================================================== */

int main(void)
{
    printf("=== Kernel Log Buffer Tests ===\n");
    test_logbuf();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
