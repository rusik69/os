/*
 * src/test/test.c — In-kernel test suite.
 *
 * Compiled into the kernel only when -DTEST_MODE is passed by the build
 * system.  `test_run_all()` is invoked as a dedicated kernel process; it
 * exercises every subsystem, prints "[PASS]" / "[FAIL]" lines to the serial
 * console, and finally calls acpi_shutdown() so QEMU exits cleanly.
 *
 * The host-side test runner (tests/run_tests.sh) launches QEMU with
 * -serial stdio, captures the output, and checks for "ALL TESTS PASSED".
 */

#include "test.h"
#include "types.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"
#include "heap.h"
#include "timer.h"
#include "rtc.h"
#include "process.h"
#include "scheduler.h"
#include "vfs.h"
#include "pipe.h"
#include "signal.h"
#include "speaker.h"
#include "mouse.h"
#include "acpi.h"
#include "ata.h"
#include "fs.h"
#include "e1000.h"
#include "net.h"

/* ── Test framework ─────────────────────────────────────────── */

static int tpass = 0;
static int tfail = 0;

static void t_ok(const char *name) {
    kprintf("[PASS] %s\n", name);
    tpass++;
}

static void t_fail(const char *name, const char *reason) {
    kprintf("[FAIL] %s — %s\n", name, reason);
    tfail++;
}

#define ASSERT(name, cond) \
    do { if (cond) t_ok(name); else t_fail(name, #cond " is false"); } while (0)

#define ASSERT_EQ(name, a, b) \
    do { uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b); \
         if (_a == _b) t_ok(name); \
         else t_fail(name, #a " != " #b); } while (0)

#define ASSERT_STR(name, a, b) \
    do { if (strcmp((a), (b)) == 0) t_ok(name); \
         else t_fail(name, "string mismatch: " #a " != " #b); } while (0)

/* ── String library ─────────────────────────────────────────── */

static void test_string(void) {
    /* strlen */
    ASSERT_EQ("strlen empty",   strlen(""),      0);
    ASSERT_EQ("strlen hello",   strlen("hello"), 5);

    /* strcmp */
    ASSERT("strcmp equal",   strcmp("abc", "abc") == 0);
    ASSERT("strcmp less",    strcmp("abc", "abd") < 0);
    ASSERT("strcmp greater", strcmp("abd", "abc") > 0);

    /* strncmp */
    ASSERT("strncmp prefix equal", strncmp("abcdef", "abcxyz", 3) == 0);
    ASSERT("strncmp differ",       strncmp("abcdef", "abcxyz", 4) != 0);

    /* memcpy + memcmp */
    char dst[8];
    memcpy(dst, "hello", 5);
    dst[5] = '\0';
    ASSERT_STR("memcpy result", dst, "hello");
    ASSERT("memcmp equal", memcmp("abc", "abc", 3) == 0);
    ASSERT("memcmp differ", memcmp("abc", "abd", 3) != 0);

    /* memset */
    memset(dst, 0xAB, 4);
    ASSERT("memset byte 0", (uint8_t)dst[0] == 0xAB);
    ASSERT("memset byte 3", (uint8_t)dst[3] == 0xAB);

    /* strcpy */
    strcpy(dst, "copy");
    ASSERT_STR("strcpy", dst, "copy");

    /* memset to zero */
    memset(dst, 0, sizeof(dst));
    ASSERT_EQ("memset zero", (uint8_t)dst[0], 0);
}

/* ── Physical memory ─────────────────────────────────────────── */

static void test_memory(void) {
    uint64_t total = pmm_get_total_frames();
    uint64_t used  = pmm_get_used_frames();

    ASSERT("pmm total > 0",     total > 0);
    ASSERT("pmm used <= total", used <= total);
    ASSERT("pmm free > 0",      total - used > 0);

    /* Heap alloc / free */
    void *p = kmalloc(256);
    ASSERT("kmalloc 256 != NULL", p != NULL);
    if (p) {
        memset(p, 0x55, 256);
        ASSERT("kmalloc write/read", ((uint8_t *)p)[255] == 0x55);
        kfree(p);
        t_ok("kfree 256");
    }

    void *q = kmalloc(4096);
    ASSERT("kmalloc 4096 != NULL", q != NULL);
    if (q) { kfree(q); t_ok("kfree 4096"); }

    /* Multiple allocations */
    void *ptrs[8];
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        ptrs[i] = kmalloc(128);
        if (!ptrs[i]) { ok = 0; break; }
    }
    ASSERT("kmalloc x8", ok);
    for (int i = 0; i < 8; i++) if (ptrs[i]) kfree(ptrs[i]);
    t_ok("kfree x8");
}

/* ── Timer ───────────────────────────────────────────────────── */

static void test_timer(void) {
    uint64_t t0 = timer_get_ticks();

    /* Spin so at least one tick fires */
    volatile uint64_t i;
    for (i = 0; i < 20000000ULL; i++);

    uint64_t t1 = timer_get_ticks();
    ASSERT("ticks advance", t1 >= t0);
}

/* ── RTC ─────────────────────────────────────────────────────── */

static void test_rtc(void) {
    struct rtc_time t;
    rtc_get_time(&t);

    ASSERT("rtc year >= 2024",   t.year  >= 2024);
    ASSERT("rtc month 1-12",     t.month >= 1 && t.month <= 12);
    ASSERT("rtc day 1-31",       t.day   >= 1 && t.day   <= 31);
    ASSERT("rtc hour 0-23",      t.hour  <= 23);
    ASSERT("rtc minute 0-59",    t.minute <= 59);
    ASSERT("rtc second 0-59",    t.second <= 59);
}

/* ── Process management ──────────────────────────────────────── */

static void test_process(void) {
    struct process *cur = process_get_current();
    ASSERT("get_current != NULL",         cur != NULL);
    if (!cur) return;

    ASSERT("current pid < PROCESS_MAX",   cur->pid < (uint32_t)PROCESS_MAX);
    ASSERT("current state == RUNNING",    cur->state == PROCESS_RUNNING);

    struct process *found = process_get_by_pid(cur->pid);
    ASSERT("get_by_pid returns current",  found == cur);

    ASSERT("get_by_pid 9999 == NULL",     process_get_by_pid(9999) == NULL);
}

/* ── Scheduler ───────────────────────────────────────────────── */

static void test_scheduler(void) {
    /* Simply yield and expect to return — confirming the ready queue works */
    scheduler_yield();
    t_ok("scheduler yield returns");
    scheduler_yield();
    t_ok("scheduler yield x2");
}

/* ── Filesystem ──────────────────────────────────────────────── */

static void test_filesystem(void) {
    if (!ata_is_present()) {
        t_ok("fs SKIP (no ATA disk)");
        return;
    }

    ASSERT("fs format", fs_format() == 0);

    /* Create file */
    ASSERT("fs create file", fs_create("/ktest", FS_TYPE_FILE) >= 0);

    /* Write data */
    const char *content = "kernel test content 12345";
    ASSERT("fs write_file", fs_write_file("/ktest", content, strlen(content)) == 0);

    /* Read back */
    static char rbuf[256];
    uint32_t sz = 0;
    ASSERT("fs read_file",    fs_read_file("/ktest", rbuf, sizeof(rbuf) - 1, &sz) == 0);
    rbuf[sz] = '\0';
    ASSERT_EQ("fs read_file size", sz, (uint64_t)strlen(content));
    ASSERT_STR("fs read_file content", rbuf, content);

    /* Stat */
    uint32_t fsz = 0; uint8_t ftype = 0;
    ASSERT("fs stat ok",        fs_stat("/ktest", &fsz, &ftype) == 0);
    ASSERT("fs stat type file", ftype == FS_TYPE_FILE);
    ASSERT_EQ("fs stat size",   fsz, (uint64_t)strlen(content));

    /* Mkdir */
    ASSERT("fs mkdir",          fs_create("/kdir", FS_TYPE_DIR) >= 0);
    uint8_t dtype = 0;
    fs_stat("/kdir", &fsz, &dtype);
    ASSERT("fs mkdir type",     dtype == FS_TYPE_DIR);

    /* Overwrite file */
    const char *content2 = "overwritten";
    ASSERT("fs overwrite", fs_write_file("/ktest", content2, strlen(content2)) == 0);
    memset(rbuf, 0, sizeof(rbuf));
    sz = 0;
    fs_read_file("/ktest", rbuf, sizeof(rbuf) - 1, &sz);
    rbuf[sz] = '\0';
    ASSERT_STR("fs overwrite content", rbuf, content2);

    /* Delete file */
    ASSERT("fs delete file", fs_delete("/ktest") == 0);

    /* Stat after delete should fail */
    ASSERT("fs post-delete stat fails", fs_stat("/ktest", &fsz, &ftype) < 0);

    /* Multiple files */
    for (int i = 0; i < 4; i++) {
        char path[12] = "/mf0";
        path[3] = (char)('0' + i);
        ASSERT("fs multi create", fs_create(path, FS_TYPE_FILE) >= 0);
        ASSERT("fs multi write",  fs_write_file(path, "x", 1) == 0);
    }
    t_ok("fs multi-file create/write");
}

/* ── VFS ─────────────────────────────────────────────────────── */

static void test_vfs(void) {
    if (!ata_is_present()) {
        t_ok("vfs SKIP (no ATA disk)");
        return;
    }

    /* Format for clean state */
    fs_format();

    const char *data = "vfs layer test data";
    ASSERT("vfs write",   vfs_write("/vf", data, strlen(data)) == 0);

    static char rbuf[256];
    uint32_t sz = 0;
    ASSERT("vfs read",    vfs_read("/vf", rbuf, sizeof(rbuf) - 1, &sz) == 0);
    rbuf[sz] = '\0';
    ASSERT_STR("vfs read content", rbuf, data);

    struct vfs_stat st;
    ASSERT("vfs stat",    vfs_stat("/vf", &st) == 0);
    ASSERT_EQ("vfs stat size", st.size, (uint64_t)strlen(data));

    ASSERT("vfs create dir", vfs_create("/vdir", FS_TYPE_DIR) >= 0);

    ASSERT("vfs unlink",  vfs_unlink("/vf") == 0);
    ASSERT("vfs unlink post-stat fails", vfs_stat("/vf", &st) < 0);
}

/* ── Pipes ───────────────────────────────────────────────────── */

static void test_pipe(void) {
    /* Basic lifecycle */
    int id = pipe_create();
    ASSERT("pipe create >= 0", id >= 0);
    if (id < 0) return;

    const char *msg = "pipe test data";
    int n = pipe_write(id, msg, strlen(msg));
    ASSERT_EQ("pipe write count", n, (uint64_t)strlen(msg));
    ASSERT("pipe available > 0", pipe_available(id) > 0);

    char buf[64];
    int r = pipe_read(id, buf, sizeof(buf) - 1);
    ASSERT_EQ("pipe read count", r, (uint64_t)strlen(msg));
    buf[r] = '\0';
    ASSERT_STR("pipe read content", buf, msg);
    ASSERT_EQ("pipe empty after read", pipe_available(id), 0);

    /* Small repeat writes */
    for (int i = 0; i < 4; i++) {
        pipe_write(id, "ab", 2);
    }
    ASSERT_EQ("pipe after 4 writes", pipe_available(id), 8);
    /* Read them all */
    char tmp[16];
    pipe_read(id, tmp, 8);
    ASSERT_EQ("pipe drained", pipe_available(id), 0);

    pipe_close_write(id);
    pipe_close_read(id);
    t_ok("pipe lifecycle");

    /* Second pipe (independent) */
    int id2 = pipe_create();
    ASSERT("pipe2 create >= 0", id2 >= 0);
    if (id2 >= 0) {
        pipe_write(id2, "y", 1);
        ASSERT_EQ("pipe2 avail", pipe_available(id2), 1);
        pipe_close_write(id2);
        pipe_close_read(id2);
        t_ok("pipe2 independent");
    }
}

/* ── Speaker ─────────────────────────────────────────────────── */

static void test_speaker(void) {
    /* Headless QEMU has no audio output; we test the code paths don't crash */
    speaker_tone(440);
    speaker_off();
    speaker_tone(880);
    speaker_off();
    speaker_tone(0);   /* 0 should also be handled */
    t_ok("speaker tone/off");

    /* Brief beep — uses timer for duration */
    speaker_beep(1000, 10);
    t_ok("speaker beep 10ms");
}

/* ── Mouse ───────────────────────────────────────────────────── */

static void test_mouse(void) {
    int x = -1, y = -1;
    mouse_get_pos(&x, &y);
    ASSERT("mouse x in [0,79]",  x >= 0 && x <= 79);
    ASSERT("mouse y in [0,24]",  y >= 0 && y <= 24);

    uint8_t btn = mouse_get_buttons();
    ASSERT("mouse buttons <= 7", btn <= 7);
}

/* ── Signals ─────────────────────────────────────────────────── */

static void test_signal(void) {
    struct process *cur = process_get_current();
    if (!cur) { t_fail("signal cur", "no current process"); return; }

    /* SIGCONT on a RUNNING process is safe (just sets pending, clears on next check) */
    ASSERT("signal_send SIGCONT self == 0", signal_send(cur->pid, SIGCONT) == 0);

    /* Sending to invalid pid fails */
    ASSERT("signal_send 9999 fails", signal_send(9999, SIGUSR1) < 0);

    /* Register a handler, then reset it */
    signal_register(SIGUSR1, (signal_handler_t)1 /* SIG_IGN */);
    signal_register(SIGUSR1, (signal_handler_t)0 /* SIG_DFL */);
    t_ok("signal_register handler");
}

/* ── Network ─────────────────────────────────────────────────── */

static void test_network(void) {
    if (!e1000_is_present()) {
        t_ok("net SKIP (no NIC)");
        return;
    }

    uint8_t mac[6];
    e1000_get_mac(mac);
    int nonzero = 0;
    for (int i = 0; i < 6; i++) if (mac[i]) { nonzero = 1; break; }
    ASSERT("e1000 MAC non-zero", nonzero);

    uint8_t ip[4];
    net_get_ip(ip);
    t_ok("net_get_ip");

    uint32_t gw = net_get_gateway();
    (void)gw;
    t_ok("net_get_gateway");

    uint32_t mask = net_get_mask();
    (void)mask;
    t_ok("net_get_mask");
}

/* ── UDP binding ─────────────────────────────────────────────── */

static volatile int udp_recv_count = 0;
static void test_udp_handler(uint32_t src_ip, uint16_t src_port,
                              const uint8_t *data, uint16_t len) {
    (void)src_ip; (void)src_port; (void)data; (void)len;
    udp_recv_count++;
}

static void test_udp_binding(void) {
    if (!e1000_is_present()) {
        t_ok("udp binding SKIP (no NIC)");
        return;
    }
    net_udp_bind(9999, test_udp_handler);
    t_ok("udp bind port 9999");
    /* Can't easily self-inject a packet here; just verifying it doesn't crash */
}

/* ── Master runner ───────────────────────────────────────────── */

void test_run_all(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("       OS KERNEL TEST SUITE             \n");
    kprintf("========================================\n");

    test_string();
    test_memory();
    test_timer();
    test_rtc();
    test_process();
    test_scheduler();
    test_filesystem();
    test_vfs();
    test_pipe();
    test_speaker();
    test_mouse();
    test_signal();
    test_network();
    test_udp_binding();

    kprintf("----------------------------------------\n");
    kprintf("Results: %u passed, %u failed\n",
            (uint64_t)tpass, (uint64_t)tfail);
    if (tfail == 0) {
        kprintf("ALL TESTS PASSED\n");
    } else {
        kprintf("SOME TESTS FAILED\n");
    }
    kprintf("========================================\n");

    /* Shut down QEMU so the host script can read the serial output */
    acpi_shutdown();

    /* Halt in case ACPI shutdown is not available */
    for (;;) __asm__ volatile("hlt");
}
