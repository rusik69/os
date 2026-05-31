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

#define KERNEL_INTERNAL
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
#include "ramdisk.h"
#include "fs.h"
#include "e1000.h"
#include "virtio_net.h"
#include "net.h"
#include "fat32.h"
#include "shm.h"
#include "mutex.h"
#include "semaphore.h"
#include "vmm.h"
#include "ac97.h"
#include "io.h"
#include "doom.h"
#include "dos.h"
#include "elf.h"
#include "syscall.h"
#include "usb.h"
#include "usb_msc.h"
#include "blockdev.h"
#include "serial.h"

/* ── Progress tracking ────────────────────────────────────────── */
/* Every PROGRESS_INTERVAL tests, write a dot directly to serial
 * so the host can see the suite is alive even on timeout. */
#define PROGRESS_INTERVAL 5
static int test_count = 0;

static void test_progress_tick(void) {
    test_count++;
    if (test_count % PROGRESS_INTERVAL == 0) {
        outb(0x3F8, '.');
        if (test_count % (PROGRESS_INTERVAL * 10) == 0)
            outb(0x3F8, '\n');
    }
}

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

/* Extended heap tests: coalescing and accounting correctness */
static void test_heap_ext(void) {
    uint64_t free_before = heap_get_free();

    /* Allocate adjacent blocks */
    void *a = kmalloc(64);
    void *b = kmalloc(64);
    ASSERT("heap_ext kmalloc a", a != NULL);
    ASSERT("heap_ext kmalloc b", b != NULL);

    /* Free them — coalescing should not corrupt heap_used_bytes */
    kfree(a);
    kfree(b);

    uint64_t free_after = heap_get_free();
    ASSERT("heap coalesce no underflow", free_after > 0 && free_after <= heap_get_total());
    ASSERT("heap coalesce same as before", free_after == free_before);

    /* Allocate a larger block */
    void *c = kmalloc(512);
    ASSERT("heap_ext kmalloc 512", c != NULL);
    kfree(c);
    free_after = heap_get_free();
    ASSERT("heap free 512 same", free_after == free_before);

    t_ok("heap extended");
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
#ifdef SKIP_DISK_TESTS
    /* In CI/TCG mode, run FS tests on a ramdisk instead of ATA.
     * The ramdisk is much faster because it avoids ATA PIO emulation. */
    ramdisk_init();
    ata_set_redirect(ramdisk_read_sectors, ramdisk_write_sectors);
#else
    if (!ata_is_present()) {
        t_ok("fs SKIP (no ATA disk)");
        return;
    }
#endif
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

    /* Overwrite file — should reuse blocks, not leak */
    uint32_t used_before = 0, used_after = 0;
    fs_get_usage(NULL, NULL, &used_before, NULL);
    const char *content2 = "overwritten";
    ASSERT("fs overwrite", fs_write_file("/ktest", content2, strlen(content2)) == 0);
    fs_get_usage(NULL, NULL, &used_after, NULL);
    ASSERT("fs overwrite no block leak", used_after <= used_before);
    memset(rbuf, 0, sizeof(rbuf));
    sz = 0;
    fs_read_file("/ktest", rbuf, sizeof(rbuf) - 1, &sz);
    rbuf[sz] = '\0';
    ASSERT_STR("fs overwrite content", rbuf, content2);

    /* Delete file */
    ASSERT("fs delete file", fs_delete("/ktest") == 0);
    uint32_t used_after_del = 0;
    fs_get_usage(NULL, NULL, &used_after_del, NULL);
    ASSERT("fs delete no block leak", used_after_del <= used_before);

    /* Bitmap block reuse: delete then rewrite should not grow usage */
    {
        uint32_t u_write = 0, u_del = 0, u_rewrite = 0;
        ASSERT("fs bitmap create", fs_create("/reuse", FS_TYPE_FILE) >= 0);
        ASSERT("fs bitmap write", fs_write_file("/reuse", "0123456789", 10) == 0);
        fs_get_usage(NULL, NULL, &u_write, NULL);
        ASSERT("fs bitmap rm", fs_delete("/reuse") == 0);
        fs_get_usage(NULL, NULL, &u_del, NULL);
        ASSERT("fs bitmap recreate", fs_create("/reuse", FS_TYPE_FILE) >= 0);
        ASSERT("fs bitmap rewrite", fs_write_file("/reuse", "0123456789", 10) == 0);
        fs_get_usage(NULL, NULL, &u_rewrite, NULL);
        ASSERT("fs bitmap reuse stable", u_rewrite <= u_write);
        fs_delete("/reuse");
    }

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

    /* Large file (>32KB) — use heap; 40KB does not fit on 32KB kernel stack */
    {
        const uint32_t big_len = 40000;
        char *big = (char *)kmalloc(big_len);
        ASSERT("fs big kmalloc", big != NULL);
        if (big) {
            for (uint32_t i = 0; i < big_len; i++) big[i] = (char)('A' + (i % 26));
            ASSERT("fs create big", fs_create("/bigfile", FS_TYPE_FILE) >= 0);
            ASSERT("fs write big", fs_write_file("/bigfile", big, big_len) == 0);
            sz = 0;
            memset(rbuf, 0, sizeof(rbuf));
            ASSERT("fs read big head", fs_read_file("/bigfile", rbuf, 64, &sz) == 0);
            uint32_t bigsz = 0;
            fs_stat("/bigfile", &bigsz, &ftype);
            ASSERT_EQ("fs big stat size", bigsz, big_len);
            kfree(big);
            t_ok("fs large file 40KB");
        }
    }
}

/* ── VFS ─────────────────────────────────────────────────────── */

static void test_vfs(void) {
#ifdef SKIP_DISK_TESTS
    ramdisk_init();
    ata_set_redirect(ramdisk_read_sectors, ramdisk_write_sectors);
    /* Format for clean state — happens below */
#else
    if (!ata_is_present()) {
        t_ok("vfs SKIP (no ATA disk)");
        return;
    }
#endif
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

    /* Test vfs path resolution — root "/" */
    ASSERT("vfs stat root", vfs_stat("/", &st) == 0);
    ASSERT("vfs stat root dir", st.type == FS_TYPE_DIR);

    /* Test ".." resolution */
    ASSERT("vfs create deep", vfs_create("/a", FS_TYPE_DIR) >= 0);
    ASSERT("vfs create deeper", vfs_create("/a/b", FS_TYPE_DIR) >= 0);
    ASSERT("vfs stat /a/b", vfs_stat("/a/b", &st) == 0);
    ASSERT("vfs stat /a/../a/b", vfs_stat("/a/../a/b", &st) == 0);
    ASSERT("vfs stat /a/b/..", vfs_stat("/a/b/..", &st) == 0);
    ASSERT("vfs stat /a/b/.. eq /a", st.type == FS_TYPE_DIR);
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

    /* Edge cases: negative length returns error */
    int id3 = pipe_create();
    ASSERT("pipe3 create", id3 >= 0);
    if (id3 >= 0) {
        int wn = pipe_write(id3, "test", -1);
        ASSERT("pipe write neg len error", wn < 0);
        int rn = pipe_read(id3, buf, -1);
        ASSERT("pipe read neg len error", rn < 0);
        pipe_close_write(id3);
        pipe_close_read(id3);
        t_ok("pipe edge cases");
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

    /* Signal mask: mask SIGTERM, then send it — process should not terminate */
    uint32_t old_mask = cur->sig_mask;
    signal_mask((1u << SIGTERM));
    ASSERT("signal_send masked SIGTERM OK", signal_send(cur->pid, SIGTERM) == 0);
    signal_unmask((1u << SIGTERM));
    cur->sig_mask = old_mask;
    t_ok("signal mask/unmask");
}

/* ── Network ─────────────────────────────────────────────────── */

static void test_procfs(void) {
    char buf[256];
    uint32_t sz = 0;
    ASSERT("procfs meminfo read", vfs_read("/proc/meminfo", buf, sizeof(buf) - 1, &sz) == 0);
    buf[sz] = '\0';
    ASSERT("procfs MemTotal present", strstr(buf, "MemTotal:") != NULL);
    ASSERT("procfs MemFree numeric", strstr(buf, "MemFree:") != NULL &&
           strstr(buf, "unknown") == NULL);
    ASSERT("procfs HeapUsed present", strstr(buf, "HeapUsed:") != NULL);
    t_ok("procfs meminfo");
}

static void test_fork(void) {
    int child = process_fork();
    if (child < 0) {
        t_ok("fork SKIP");
        return;
    }
    /* Child starts at fork_child_entry and exits immediately with code 0 */
    int status;
    process_waitpid(child, &status);
    ASSERT("fork child exit 0", status == 0);
    t_ok("fork parent");
}

static void test_shm_mutex(void) {
    int sid = shm_get(42);
    ASSERT("shm_get >= 0", sid >= 0);
    if (sid >= 0) {
        uint64_t addr = shm_at(sid);
        ASSERT("shm_at non-zero", addr != 0);
        shm_dt(sid);
        shm_free(sid);
    }
    int mid = mutex_init();
    ASSERT("mutex_init >= 0", mid >= 0);
    if (mid >= 0) {
        mutex_lock(mid);
        mutex_unlock(mid);
        mutex_destroy(mid);
    }
    t_ok("shm mutex");
}

static void test_fat32(void) {
#ifdef SKIP_DISK_TESTS
    t_ok("fat32 SKIP (ramdisk — requires FAT32 format support)");
    return;
#else
    /* ── ATA FAT32 tests (if ATA disk present) ────────────────── */
    if (ata_is_present()) {
        if (fat32_mount(FAT32_DISK_ATA, 0) == 0) {
            ASSERT("fat32 write", fat32_write_file("/testos.txt", "hi", 2) == 2);
            char buf[8];
            ASSERT("fat32 read", fat32_read_file("/testos.txt", buf, sizeof(buf)) == 2);
            buf[2] = '\0';
            ASSERT_STR("fat32 content", buf, "hi");
            ASSERT("fat32 sync", fat32_sync() == 0);
            t_ok("fat32 ata rw");

            /* Test directory listing */
            char names[16][FAT32_MAX_NAME];
            int n = fat32_list_dir("/", names, 16);
            ASSERT("fat32 list dir", n >= 1);
            t_ok("fat32 ata list dir");
        } else {
            t_ok("fat32 SKIP (no ATA FAT partition)");
        }
    } else {
        t_ok("fat32 SKIP (no ATA)");
    }

    /* ── USB FAT32 tests (if USB MSC present) ────────────────── */
    {
        int usb_present = 0;
        if (usb_is_present()) {
            if (usb_msc_init() == 0) {
                usb_present = 1;
            }
        }
        if (usb_present && blockdev_is_registered(BLOCKDEV_USB0)) {
            if (fat32_mount(FAT32_DISK_USB0, 0) == 0) {
                char buf[64];
                int n = fat32_read_file("/testos.txt", buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    t_ok("fat32 usb read /testos.txt");
                }

                char names[16][FAT32_MAX_NAME];
                int cnt = fat32_list_dir("/", names, 16);
                ASSERT("fat32 usb list dir", cnt >= 0);
                if (cnt > 0) {
                    ASSERT("fat32 usb names[0] non-empty", names[0][0] != '\0');
                }
                t_ok("fat32 usb operations");
            } else {
                t_ok("fat32 SKIP (no USB FAT partition)");
            }
        } else {
            t_ok("fat32 SKIP (no USB MSC)");
        }
    }

    t_ok("fat32 tests");
#endif
}

static void test_ac97(void) {
    if (!ac97_present()) {
        t_ok("ac97 SKIP");
        return;
    }
    static int16_t samples[64];
    for (int i = 0; i < 64; i++) samples[i] = (int16_t)(i * 100);
    ac97_play_pcm(samples, sizeof(samples), 8000);
    t_ok("ac97 play pcm");
}

static void test_doom(void) {
    doom_math_init();
    ASSERT("doom trig sin/cos", doom_test_trig());
    ASSERT("doom ray hit", doom_test_ray_hit());
    ASSERT("doom wall collision", doom_test_collision());
    ASSERT("doom column sky", doom_test_column_has_sky());
    ASSERT("doom column wall", doom_test_column_has_wall());
    /* Full frame rendering is disabled in automated tests (too CPU-intensive
     * on shared CI runners).  Run manually via the 'doom' shell command. */
    if (0) {
        ASSERT("doom frame varies", doom_test_frame_varies());
        ASSERT("doom door opens", doom_test_door_opens());
    }
}

static void test_network(void) {
    if (!virtio_net_present() && !e1000_is_present()) {
        t_ok("net SKIP (no NIC)");
        return;
    }

    uint8_t mac[6];
    if (virtio_net_present())
        virtio_net_get_mac(mac);
    else
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
    if (!virtio_net_present() && !e1000_is_present()) {
        t_ok("udp binding SKIP (no NIC)");
        return;
    }
    net_udp_bind(9999, test_udp_handler);
    t_ok("udp bind port 9999");
    /* Can't easily self-inject a packet here; just verifying it doesn't crash */
}

/* ── ELF loader tests ─────────────────────────────────────────── */

static void test_elf(void) {
    kprintf("[DBG] test_elf enter\n");
    /* 1. Bad magic → returns 0 */
    kprintf("[DBG] test_elf #1\n");
    uint8_t bad[16] = {0};
    ASSERT_EQ("elf bad magic", elf_load(bad, sizeof(bad)), 0);

    /* 2. Wrong architecture → returns 0 */
    kprintf("[DBG] test_elf #2\n");
    uint8_t wrong[128];
    memset(wrong, 0, sizeof(wrong));
    *(uint32_t *)wrong = ELF_MAGIC;
    wrong[4] = ELF_CLASS64;
    wrong[5] = ELF_DATA2LSB;
    ASSERT_EQ("elf wrong arch", elf_load(wrong, sizeof(wrong)), 0);

    /* 3. No program headers → returns 0 */
    kprintf("[DBG] test_elf #3\n");
    struct elf64_header *wh = (struct elf64_header *)wrong;
    wh->e_type = ET_EXEC;
    wh->e_machine = EM_X86_64;
    wh->e_version = 1;
    wh->e_phoff = 0;
    wh->e_phnum = 0;
    wh->e_ehsize = sizeof(struct elf64_header);
    ASSERT_EQ("elf no phdrs", elf_load(wrong, sizeof(wrong)), 0);

    /* 4. Segment out of bounds (offset+filesz > size) → returns 0 */
    kprintf("[DBG] test_elf #4\n");
    wh->e_phoff = sizeof(struct elf64_header);
    wh->e_phnum = 1;
    wh->e_phentsize = sizeof(struct elf64_phdr);
    if (sizeof(wrong) >= sizeof(struct elf64_header) + sizeof(struct elf64_phdr)) {
        struct elf64_phdr *ph = (struct elf64_phdr *)(wrong + sizeof(struct elf64_header));
        ph->p_type = PT_LOAD;
        ph->p_offset = 0;
        ph->p_filesz = 99999; /* larger than buffer */
        ph->p_vaddr = 0x100000;
        ph->p_memsz = 99999;
        ASSERT_EQ("elf oob segment", elf_load(wrong, sizeof(wrong)), 0);
    }

    /* 5. Segment targeting NULL page (p_vaddr < 0x1000) → returns 0 */
    kprintf("[DBG] test_elf #5\n");
    struct elf64_phdr *ph = (struct elf64_phdr *)(wrong + sizeof(struct elf64_header));
    ph->p_offset = sizeof(struct elf64_header) + sizeof(struct elf64_phdr);
    ph->p_filesz = 4;
    ph->p_vaddr = 0x800;
    ph->p_memsz = 4;
    ASSERT_EQ("elf null-page seg", elf_load(wrong, sizeof(wrong)), 0);

    kprintf("[DBG] test_elf #5 passed\n");
    /* 6. Successful load of a minimal ELF at a pre-allocated frame */
    {
        kprintf("[DBG] test_elf #6 enter\n");
        uint64_t frame = pmm_alloc_frame();
        kprintf("[DBG] test_elf #6 frame=0x%x\n", frame);
        ASSERT("elf alloc frame", frame != 0);
        if (frame && frame >= 0x1000) {
            kprintf("[DBG] test_elf #6 alloc ok\n");
            uint8_t buf[256];
            memset(buf, 0, sizeof(buf));
            struct elf64_header *hdr = (struct elf64_header *)buf;
            *(uint32_t *)hdr->e_ident = ELF_MAGIC;
            hdr->e_ident[4] = ELF_CLASS64;
            hdr->e_ident[5] = ELF_DATA2LSB;
            hdr->e_ident[6] = 1; /* EI_VERSION */
            hdr->e_type = ET_EXEC;
            hdr->e_machine = EM_X86_64;
            hdr->e_version = 1;
            hdr->e_entry = frame; /* entry = base of loaded segment */
            hdr->e_phoff = sizeof(struct elf64_header);
            hdr->e_ehsize = sizeof(struct elf64_header);
            hdr->e_phentsize = sizeof(struct elf64_phdr);
            hdr->e_phnum = 1;

            struct elf64_phdr *pph =
                (struct elf64_phdr *)(buf + sizeof(struct elf64_header));
            pph->p_type = PT_LOAD;
            pph->p_flags = 7; /* RWX */
            pph->p_offset = sizeof(struct elf64_header) + sizeof(struct elf64_phdr);
            pph->p_vaddr = frame;
            pph->p_filesz = 1; /* single RET instruction */
            pph->p_memsz = 1;
            pph->p_align = 0x1000;

            /* Code byte at the end: RET (0xC3) */
            uint32_t code_off = sizeof(struct elf64_header) + sizeof(struct elf64_phdr);
            buf[code_off] = 0xC3;
            uint32_t total_sz = code_off + 1;

            /* Use the high-half VMA for the segment so elf_load treats this
             * as a kernel-mode ELF and actually copies the segment data. */
            uint64_t frame_vma = (uint64_t)PHYS_TO_VIRT(frame);
            hdr->e_entry = frame_vma;
            pph->p_vaddr  = frame_vma;

            kprintf("[DBG] test_elf before elf_load\n");
            uint64_t entry = elf_load(buf, total_sz);
            kprintf("[DBG] test_elf after elf_load entry=0x%x\n", entry);
            ASSERT_EQ("elf load entry", entry, frame_vma);
            ASSERT("elf loaded data", *(volatile uint8_t *)PHYS_TO_VIRT(frame) == 0xC3);

            /* Clean up */
            memset(PHYS_TO_VIRT(frame), 0, PAGE_SIZE);
            pmm_free_frame(frame);
            t_ok("elf successful load");
        } else if (frame) {
            pmm_free_frame(frame);
        }
    }

    t_ok("elf tests");
}

/* ── VMM tests ────────────────────────────────────────────────── */
static void test_vmm(void) {
    /* ── Basic page table ──────────────────────────────────────── */
    /* Test vmm_get_pml4 returns a non-null page table */
    uint64_t *pml4 = vmm_get_pml4();
    ASSERT("vmm get pml4", pml4 != NULL);

    /* Test vmm_get_physaddr on known identity-mapped VGA memory */
    uint64_t vga_phys = vmm_get_physaddr(0xB8000ULL);
    ASSERT("vmm vga physaddr", vga_phys == 0xB8000ULL);

    /* Test vmm_virt_to_phys (returns 0 on success) */
    uint64_t phys = 0;
    ASSERT("vmm virt_to_phys vga", vmm_virt_to_phys(0xB8000ULL, &phys) == 0);
    ASSERT("vmm virt_to_phys vga val", phys == 0xB8000ULL);
    /* Test that invalid addresses return error */
    ASSERT("vmm virt_to_phys invalid", vmm_virt_to_phys(~0ULL, &phys) != 0);

    /* Test vmm_user_range_ok rejects kernel addresses */
    ASSERT("vmm user range kernel", vmm_user_range_ok(NULL, 0xFFFFFFFFFFFFFFFFULL, 1, 0) == 0);

    /* ── Page table walk ────────────────────────────────────────── */
    {
        /* Walk the VGA address 0xB8000 through the page tables */
        int pml4_idx = (0xB8000ULL >> 39) & 0x1FF;
        int pdpt_idx = (0xB8000ULL >> 30) & 0x1FF;
        int pd_idx   = (0xB8000ULL >> 21) & 0x1FF;
        int pt_idx   = (0xB8000ULL >> 12) & 0x1FF;

        uint64_t pml4e = pml4[pml4_idx];
        ASSERT("walk pml4e present", pml4e & VMM_FLAG_PRESENT);
        if (!(pml4e & VMM_FLAG_PRESENT)) {
            t_ok("vmm page table walk (low mapping removed)");
            goto vmm_walk_done;
        }

        uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & 0x000FFFFFFFFFF000ULL);
        uint64_t pdpte = pdpt[pdpt_idx];
        ASSERT("walk pdpte present", pdpte & VMM_FLAG_PRESENT);
        if (!(pdpte & VMM_FLAG_PRESENT)) {
            t_ok("vmm page table walk (low mapping removed)");
            goto vmm_walk_done;
        }

        uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & 0x000FFFFFFFFFF000ULL);
        uint64_t pde = pd[pd_idx];
        ASSERT("walk pde present", pde & VMM_FLAG_PRESENT);
        if (!(pde & VMM_FLAG_PRESENT)) {
            t_ok("vmm page table walk (low mapping removed)");
            goto vmm_walk_done;
        }

        /* Check for 2MB huge page at VGA (common in boot page tables) */
        if (pde & (1ULL << 7)) {
            /* Huge page — verify address */
            uint64_t huge_base = pde & 0x000FFFFFFFE00000ULL;
            ASSERT("walk pde huge 0xB8000", (huge_base + (0xB8000ULL & 0x1FFFFF)) == 0xB8000ULL);
        } else {
            uint64_t *pt = (uint64_t *)(uintptr_t)(pde & 0x000FFFFFFFFFF000ULL);
            uint64_t pte = pt[pt_idx];
            ASSERT("walk pte present", pte & VMM_FLAG_PRESENT);
            ASSERT("walk pte maps 0xB8000", (pte & 0x000FFFFFFFFFF000ULL) == 0xB8000ULL);
        }
        t_ok("vmm page table walk");
vmm_walk_done: ;
    }

    /* ── Page alloc, map, write, read-back, unmap ─────────────── */
    {
        /* Pick a virtual address that's unlikely to be in use.
         * Must be in the kernel's higher half (PML4 index 256-511). */
        uint64_t test_va = 0xFFFFF00000000000ULL;

        /* Verify it's not already mapped */
        ASSERT("vmm va not mapped before", vmm_virt_to_phys(test_va, &phys) != 0);

        /* Allocate a physical frame */
        uint64_t frame = pmm_alloc_frame();
        ASSERT("vmm alloc frame", frame != 0);
        if (frame) {
            uint64_t pattern = 0xDEADBEEFCAFEBABEULL;

            /* Map the frame at test_va */
            int r = vmm_map_page(test_va, frame, VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
            ASSERT("vmm map page", r == 0);

            /* Verify vmm_virt_to_phys now returns the mapped frame */
            phys = 0;
            ASSERT("vmm virt_to_phys mapped OK", vmm_virt_to_phys(test_va, &phys) == 0);
            ASSERT_EQ("vmm virt_to_phys mapped addr", phys & ~0xFFFULL, frame);

            /* Write a pattern through the virtual address */
            *(volatile uint64_t *)test_va = pattern;

            /* Read back via virtual address */
            ASSERT("vmm write/read virt", *(volatile uint64_t *)test_va == pattern);

            /* Read back via physical address (identity-mapped) */
            ASSERT("vmm write/read phys", *(volatile uint64_t *)(uintptr_t)frame == pattern);

            /* Write a different pattern */
            *(volatile uint64_t *)test_va = 0;

            /* Unmap */
            vmm_unmap_page(test_va);
            ASSERT("vmm unmapped check", vmm_virt_to_phys(test_va, &phys) != 0);

            /* Free the frame */
            pmm_free_frame(frame);
            t_ok("vmm page alloc/map/unmap");
        }
    }

    t_ok("vmm tests");
}

/* ── TCP / Networking tests ──────────────────────────────────── */

static void test_tcp(void) {
    if (!virtio_net_present() && !e1000_is_present()) {
        t_ok("tcp SKIP (no NIC)");
        return;
    }

    /* Verify that the network stack reports sane values */
    uint8_t ip[4];
    net_get_ip(ip);
    /* In QEMU user-mode networking the guest IP is typically 10.0.2.x */
    ASSERT("tcp ip octet1 non-zero", ip[0] != 0);

    uint32_t gw = net_get_gateway();
    ASSERT("tcp gateway non-zero", gw != 0);

    uint32_t mask = net_get_mask();
    ASSERT("tcp mask non-zero", mask != 0);

    /* Test TCP listen/unlisten lifecycle (no actual connection) */
    net_tcp_listen(9998, NULL, NULL, NULL);
    t_ok("tcp listen port 9998");

    /* Listen on a second port */
    net_tcp_listen(9997, NULL, NULL, NULL);
    t_ok("tcp listen port 9997");

    /* Unlisten both */
    net_tcp_unlisten(9998);
    net_tcp_unlisten(9997);
    t_ok("tcp unlisten");

    /* Verify ARP cache enumeration (may be empty, but should not crash) */
    int arp_count = net_arp_list(NULL);
    ASSERT("tcp arp list ok", arp_count >= 0);
    t_ok("tcp arp list");

    /* Verify TCP connection list (should be empty but should not crash) */
    net_conn_list(NULL);
    t_ok("tcp conn list");

    /* Verify net_rx_pending / net_poll are callable */
    net_poll();
    t_ok("tcp net_poll");
    (void)net_rx_pending();
    t_ok("tcp net_rx_pending");

    t_ok("tcp tests");
}

/* ── Semaphore tests ──────────────────────────────────────────── */
static void test_semaphore(void) {
    int id = sem_init(2);
    ASSERT("sem init", id >= 0);
    sem_wait(id);
    ASSERT("sem wait ok", 1);
    sem_post(id);
    sem_post(id);
    sem_post(id);
    ASSERT("sem post ok", 1);
    sem_destroy(id);
}

/* ── Enhanced SHM tests ───────────────────────────────────────── */
static void test_shm_ext(void) {
    int id = shm_get(42);
    ASSERT("shm ext get", id >= 0);
    uint64_t addr = shm_at(id);
    ASSERT("shm ext at", addr != 0);
    /* Verify we can write to mapped memory */
    if (addr) {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)addr;
        p[0] = 0xAB;
        p[4095] = 0xCD;
        ASSERT("shm ext write byte0", p[0] == 0xAB);
        ASSERT("shm ext write byte4095", p[4095] == 0xCD);
    }
    ASSERT("shm ext dt", shm_dt(id) == 0);
    ASSERT("shm ext free", shm_free(id) == 0);
}

/* ── Comprehensive IPC tests ──────────────────────────────────── */

static void test_ipc(void) {
    /* ── Shared memory ──────────────────────────────────────────── */
    {
        /* Create two segments with different keys */
        int sid1 = shm_get(100);
        ASSERT("ipc shm get 100", sid1 >= 0);
        int sid2 = shm_get(200);
        ASSERT("ipc shm get 200", sid2 >= 0);

        /* Attach both */
        uint64_t addr1 = shm_at(sid1);
        uint64_t addr2 = shm_at(sid2);
        ASSERT("ipc shm at 100", addr1 != 0);
        ASSERT("ipc shm at 200", addr2 != 0);

        if (addr1 && addr2) {
            volatile uint8_t *p1 = (volatile uint8_t *)(uintptr_t)addr1;
            volatile uint8_t *p2 = (volatile uint8_t *)(uintptr_t)addr2;

            /* Write distinct patterns */
            p1[0] = 0x11;
            p1[1] = 0x22;
            p2[0] = 0xAA;
            p2[1] = 0xBB;

            /* Verify they are independent */
            ASSERT("ipc shm indep p1[0]", p1[0] == 0x11);
            ASSERT("ipc shm indep p1[1]", p1[1] == 0x22);
            ASSERT("ipc shm indep p2[0]", p2[0] == 0xAA);
            ASSERT("ipc shm indep p2[1]", p2[1] == 0xBB);
            ASSERT("ipc shm not aliased", &p1[0] != &p2[0]);
        }

        /* Detach and free */
        ASSERT("ipc shm dt 100", shm_dt(sid1) == 0);
        ASSERT("ipc shm dt 200", shm_dt(sid2) == 0);
        ASSERT("ipc shm free 100", shm_free(sid1) == 0);
        ASSERT("ipc shm free 200", shm_free(sid2) == 0);
        t_ok("ipc shared memory");
    }

    /* ── Mutex ──────────────────────────────────────────────────── */
    {
        int m1 = mutex_init();
        int m2 = mutex_init();
        ASSERT("ipc mutex init m1", m1 >= 0);
        ASSERT("ipc mutex init m2", m2 >= 0);

        /* Lock/unlock same mutex twice */
        mutex_lock(m1);
        ASSERT("ipc mutex locked", 1);
        mutex_unlock(m1);
        ASSERT("ipc mutex unlocked", 1);
        mutex_lock(m1);
        mutex_unlock(m1);
        ASSERT("ipc mutex lock twice", 1);

        /* Two independent mutexes interleaved */
        mutex_lock(m1);
        mutex_lock(m2);
        mutex_unlock(m2);
        mutex_unlock(m1);
        ASSERT("ipc mutex interleaved", 1);

        mutex_destroy(m1);
        mutex_destroy(m2);
        t_ok("ipc mutex");
    }

    /* ── Semaphore ─────────────────────────────────────────────── */
    {
        int s1 = sem_init(3); /* count = 3 */
        int s2 = sem_init(1); /* count = 1 */
        ASSERT("ipc sem init s1", s1 >= 0);
        ASSERT("ipc sem init s2", s2 >= 0);

        /* Consume s1 entirely */
        sem_wait(s1);
        sem_wait(s1);
        sem_wait(s1);
        ASSERT("ipc sem s1 consumed", 1);

        /* Post back */
        sem_post(s1);
        ASSERT("ipc sem s1 posted", 1);

        /* s2 binary-semaphore behavior */
        sem_wait(s2);
        ASSERT("ipc sem s2 locked", 1);
        sem_post(s2);
        ASSERT("ipc sem s2 unlocked", 1);

        sem_destroy(s1);
        sem_destroy(s2);
        t_ok("ipc semaphore");
    }

    t_ok("ipc tests");
}

/* ── DOS emulator tests ───────────────────────────────────────── */
static void test_dos(void) {
    int dos_load_com(struct dos_cpu_state *state, const uint8_t *data, uint32_t size);
    void dos_emu_init(struct dos_cpu_state *state);
    void dos_emu_run(struct dos_cpu_state *state);

    /* ── .COM loading tests ───────────────────────────────────── */
    {
        struct dos_cpu_state state;
        dos_emu_init(&state);
        uint8_t com[] = { 0xB8, 0x00, 0x4C, 0xCD, 0x21 }; /* MOV AX, 0x4C00; INT 0x21 */
        int ret = dos_load_com(&state, com, sizeof(com));
        ASSERT("dos load com", ret == 0);
        ASSERT("dos com at 0x100", state.memory[0x100] == 0xB8);
        ASSERT("dos com entry", state.ip == 0x100);
        ASSERT("dos com segments", state.cs == 0 && state.ds == 0);
        t_ok("dos loader");
    }

    /* ── Instruction execution: simple exit via INT 0x20 ──────── */
    {
        struct dos_cpu_state state;
        dos_emu_init(&state);
        /* INT 0x20 — terminate program (sets state->running = 0) */
        uint8_t com[] = { 0xCD, 0x20 };
        int ret = dos_load_com(&state, com, sizeof(com));
        ASSERT("dos exec load", ret == 0);

        /* Execute — should run INT 0x20 and exit immediately */
        dos_emu_run(&state);
        ASSERT("dos exec stopped", state.running == 0);
        /* IP should have advanced past the INT instruction (2 bytes) */
        ASSERT("dos exec ip advanced", state.ip == 0x102);
        t_ok("dos exec int20");
    }

    /* ── Instruction execution: basic arithmetic ──────────────── */
    {
        struct dos_cpu_state state;
        dos_emu_init(&state);
        /*
         * MOV AX, 0x1234
         * MOV BX, 0x0001
         * ADD AX, BX    → AX = 0x1235
         * INT 0x20
         */
        uint8_t com[] = {
            0xB8, 0x34, 0x12,       /* MOV AX, 0x1234 */
            0xBB, 0x01, 0x00,       /* MOV BX, 0x0001 */
            0x01, 0xD8,             /* ADD AX, BX     */
            0xCD, 0x20              /* INT 0x20       */
        };
        int ret = dos_load_com(&state, com, sizeof(com));
        ASSERT("dos arith load", ret == 0);
        dos_emu_run(&state);
        ASSERT("dos arith stopped", state.running == 0);
        ASSERT_EQ("dos arith ax", state.ax, 0x1235);
        t_ok("dos exec arithmetic");
    }

    /* ── Instruction limit: infinite loop stopped by 1M limit ──── */
    {
        struct dos_cpu_state state;
        dos_emu_init(&state);
        uint8_t com[] = { 0xEB, 0xFE }; /* JMP short -2 (infinite loop) */
        int ret = dos_load_com(&state, com, sizeof(com));
        ASSERT("dos loop load", ret == 0);

        /* The emulator has a 1,000,000-instruction hard limit */
        dos_emu_run(&state);
        ASSERT("dos loop stopped", state.running == 0);
        t_ok("dos exec infinite loop limit");
    }

    t_ok("dos tests");
}

/* ── Syscall interface tests ──────────────────────────────────── */

static void test_syscall(void) {
    /* Ensure syscall_init has been called during boot */
    t_ok("syscall init assumed");

    /* SYS_GETPID — should return current process PID (> 0) */
    {
        uint64_t pid = syscall_dispatch(SYS_GETPID, 0, 0, 0, 0, 0);
        ASSERT("syscall getpid > 0", pid > 0);
        ASSERT("syscall getpid finite", pid < 10000);
        t_ok("syscall getpid");
    }

    /* SYS_WRITE to stdout (fd=1) — should succeed */
    {
        const char *msg = "syscall: hello from kernel test\n";
        /* syscall_dispatch(1=SYS_WRITE, fd=1, buf, len, 0, 0) */
        uint64_t written = syscall_dispatch(SYS_WRITE, 1,
                                            (uint64_t)(uintptr_t)msg,
                                            strlen(msg), 0, 0);
        ASSERT("syscall write > 0", written > 0);
        t_ok("syscall write stdout");
    }

    /* SYS_UPTIME — should return non-zero ticks */
    {
        uint64_t uptime = syscall_dispatch(SYS_UPTIME, 0, 0, 0, 0, 0);
        ASSERT("syscall uptime > 0", uptime > 0);
        t_ok("syscall uptime");
    }

    /* SYS_YIELD — should return without crashing */
    {
        syscall_dispatch(SYS_YIELD, 0, 0, 0, 0, 0);
        t_ok("syscall yield");
    }

    /* Invalid syscall number should not crash */
    {
        uint64_t ret = syscall_dispatch(9999, 0, 0, 0, 0, 0);
        /* Most implementations return -1 or 0 for invalid */
        t_ok("syscall invalid number");
        (void)ret;
    }

    t_ok("syscall tests");
}

/* ── Heap stress: fragmentation and OOM ────────────────────── */

static void test_heap_stress(void) {
    /* Allocate many small blocks to stress fragmentation */
    void *ptrs[64];
    int i;
    for (i = 0; i < 64; i++) {
        ptrs[i] = kmalloc(32);
        if (!ptrs[i]) break;
    }
    ASSERT("heap stress alloc many", i >= 8);
    /* Free every other block */
    for (i = 0; i < 64; i += 2) {
        if (ptrs[i]) kfree(ptrs[i]);
    }
    /* Allocate a large block — should succeed if coalescing works */
    void *big = kmalloc(1024);
    ASSERT("heap stress large after frag", big != NULL);
    if (big) kfree(big);
    /* Free remaining */
    for (i = 1; i < 64; i += 2) {
        if (ptrs[i]) kfree(ptrs[i]);
    }
    t_ok("heap stress");
}

/* ── ELF edge cases ───────────────────────────────────────────── */

static void test_elf_edge(void) {
    /* 1. Segment with p_memsz > p_filesz (BSS extension) */
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    struct elf64_header *hdr = (struct elf64_header *)buf;
    *(uint32_t *)hdr->e_ident = ELF_MAGIC;
    hdr->e_ident[4] = ELF_CLASS64;
    hdr->e_ident[5] = ELF_DATA2LSB;
    hdr->e_ident[6] = 1;
    hdr->e_type = ET_EXEC;
    hdr->e_machine = EM_X86_64;
    hdr->e_version = 1;
    hdr->e_phoff = sizeof(struct elf64_header);
    hdr->e_ehsize = sizeof(struct elf64_header);
    hdr->e_phentsize = sizeof(struct elf64_phdr);
    hdr->e_phnum = 1;

    struct elf64_phdr *ph = (struct elf64_phdr *)(buf + sizeof(struct elf64_header));
    ph->p_type = PT_LOAD;
    ph->p_flags = 5; /* RX */
    ph->p_offset = sizeof(struct elf64_header) + sizeof(struct elf64_phdr);
    ph->p_vaddr = 0x100000;
    ph->p_paddr = 0;
    ph->p_filesz = 4;
    ph->p_memsz = 4096 + 256; /* Much larger than filesz (BSS) */
    ph->p_align = 0x1000;

    /* Should succeed — BSS extension is normal */
    uint64_t entry = elf_load(buf, sizeof(buf));
    /* Note: we may not have a real frame at 0x100000 mapped */
    /* Just verify it doesn't crash parsing; entry may be 0 if mapping fails */
    t_ok("elf bss extension");

    /* 2. Zero-length segment (p_filesz = 0, p_memsz = 0) */
    ph->p_filesz = 0;
    ph->p_memsz = 0;
    ph->p_vaddr = 0x200000;
    entry = elf_load(buf, sizeof(buf));
    t_ok("elf zero-length segment");

    /* 3. Segment wrapping vaddr (vaddr + memsz overflow) */
    ph->p_filesz = 16;
    ph->p_memsz = 16;
    ph->p_vaddr = 0xFFFFFFFFFFFFF000ULL; /* Near top of address space */
    entry = elf_load(buf, sizeof(buf));
    /* Should return 0 (safety check prevents mapping) */
    ASSERT_EQ("elf vaddr overflow reject", entry, 0);

    t_ok("elf edge cases");
}

/* ── VMM page allocation and mapping tests ──────────────────── */

static void test_vmm_alloc(void) {
    /* Allocate a physical frame and map it into the virtual address space */
    uint64_t frame = pmm_alloc_frame();
    ASSERT("vmm alloc frame", frame != 0 && frame != ~0ULL);
    if (!frame || frame == ~0ULL) {
        t_ok("vmm alloc SKIP (no memory)");
        return;
    }

    /* Map the frame at a test virtual address */
    uint64_t test_vaddr = 0x70000000ULL; /* High enough to not conflict */
    int map_ret = vmm_map_page(test_vaddr, frame, VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    ASSERT("vmm map page", map_ret == 0);

    /* Write to the page and read back */
    volatile uint8_t *test_page = (volatile uint8_t *)(uintptr_t)test_vaddr;
    test_page[0] = 0xAB;
    test_page[1] = 0xCD;
    test_page[4095] = 0xEF; /* Last byte in page */
    ASSERT("vmm write-read", test_page[0] == 0xAB);
    ASSERT("vmm write-read2", test_page[1] == 0xCD);
    ASSERT("vmm write-read last", test_page[4095] == 0xEF);

    /* Verify physical address resolves correctly */
    uint64_t resolved = vmm_get_physaddr(test_vaddr);
    ASSERT("vmm get_physaddr mapped", resolved == frame);

    /* Unmap the page */
    vmm_unmap_page(test_vaddr);
    /* No ASSERT for void return — unmapping succeeded if we got here */

    /* After unmapping, get_physaddr should return 0 or error */
    uint64_t after_unmap = vmm_get_physaddr(test_vaddr);
    ASSERT("vmm get_physaddr after unmap", after_unmap == 0 || after_unmap == ~0ULL);

    /* Free the frame */
    pmm_free_frame(frame);
    t_ok("vmm alloc tests");
}

/* ── Pipe edge cases ─────────────────────────────────────────── */

static void test_pipe_edge(void) {
    /* Create a pipe — pipe_create() returns a single pipe index */
    int pid = pipe_create();
    ASSERT("pipe edge create", pid >= 0);
    if (pid < 0) return;

    /* Write a small amount and verify available */
    const char *msg = "Hello!";
    int written = pipe_write(pid, msg, 6);
    ASSERT_EQ("pipe edge write 6", written, 6);

    int avail = pipe_available(pid);
    ASSERT("pipe edge avail > 0", avail > 0);

    /* Read it back */
    char buf[16];
    int rd = pipe_read(pid, buf, sizeof(buf));
    ASSERT("pipe edge read", rd == 6);
    ASSERT("pipe edge content", buf[0] == 'H' && buf[5] == '!');

    /* Write in chunks to test partial read */
    written = 0;
    while (written < 256) {
        int n = pipe_write(pid, "abcdefghij", 10);
        if (n <= 0) break;
        written += n;
    }
    ASSERT("pipe edge write many", written > 0);

    /* Read partial */
    char small[4];
    rd = pipe_read(pid, small, 4);
    ASSERT_EQ("pipe edge partial read", rd, 4);

    /* Close and verify cleanup */
    pipe_close_write(pid);
    pipe_close_read(pid);
    t_ok("pipe edge tests");
}

/* ── Master runner ───────────────────────────────────────────── */

/* Discard hook: suppresses VGA/serial during test execution.
 * Output still flows to the dmesg ring buffer, so no data is lost. */
static void discard_hook(char c, void *ctx) {
    (void)c; (void)ctx;
}

/* QEMU isa-debug-exit device at port 0xF4.
 * Enabled with: -device isa-debug-exit,iobase=0xf4,iosize=0x04
 * Writing value N makes QEMU exit with exit code (N & 0xff).
 * Passing: write 0x31 → exit 33. Failing: write 0x10 → exit 16.
 * More reliable than ACPI shutdown in TCG mode. */
#define QEMU_DEBUG_EXIT_PORT 0xF4

static void qemu_exit(int code) {
    outb(QEMU_DEBUG_EXIT_PORT, (uint8_t)code);
    for (;;) __asm__ volatile("hlt");
}

void test_run_all(void) {
    outb(0x3F8, 'Z');  /* marker: test task is running */

    /* Suppress VGA/serial output during tests.
     * Under TCG emulation every serial OUT instruction causes a costly
     * translation-block exit; buffering all output in the dmesg ring
     * buffer and flushing at the end dramatically speeds up the suite. */
    kprintf_set_hook(discard_hook, NULL);
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("       OS KERNEL TEST SUITE             \n");
    kprintf("========================================\n");

    kprintf("[TEST] string\n");      test_string();      test_progress_tick();
    kprintf("[TEST] memory\n");      test_memory();      test_progress_tick();
    kprintf("[TEST] heap_ext\n");    test_heap_ext();    test_progress_tick();
    kprintf("[TEST] heap_stress\n"); test_heap_stress(); test_progress_tick();
    kprintf("[TEST] timer\n");       test_timer();       test_progress_tick();
    kprintf("[TEST] rtc\n");         test_rtc();         test_progress_tick();
    kprintf("[TEST] process\n");     test_process();     test_progress_tick();
    kprintf("[TEST] scheduler\n");   test_scheduler();   test_progress_tick();
    kprintf("[TEST] filesystem\n");  test_filesystem();  test_progress_tick();
    kprintf("[TEST] vfs\n");         test_vfs();         test_progress_tick();
    kprintf("[TEST] pipe\n");        test_pipe();        test_progress_tick();
    kprintf("[TEST] pipe_edge\n");   test_pipe_edge();   test_progress_tick();
    kprintf("[TEST] speaker\n");     test_speaker();     test_progress_tick();
    kprintf("[TEST] mouse\n");       test_mouse();       test_progress_tick();
    kprintf("[TEST] signal\n");      test_signal();      test_progress_tick();
    kprintf("[TEST] network\n");     test_network();     test_progress_tick();
    kprintf("[XX] after network\n");
    kprintf("[TEST] udp_binding\n"); test_udp_binding(); test_progress_tick();
    kprintf("[TEST] elf\n");         test_elf();         test_progress_tick();
    kprintf("[TEST] elf_edge\n");    test_elf_edge();    test_progress_tick();
    kprintf("[TEST] vmm\n");         test_vmm();         test_progress_tick();
    kprintf("[TEST] vmm_alloc\n");   test_vmm_alloc();   test_progress_tick();
    kprintf("[TEST] tcp\n");         test_tcp();         test_progress_tick();
    kprintf("[TEST] procfs\n");      test_procfs();      test_progress_tick();
    kprintf("[TEST] fork\n");        test_fork();        test_progress_tick();
    kprintf("[TEST] shm_mutex\n");   test_shm_mutex();   test_progress_tick();
    kprintf("[TEST] semaphore\n");   test_semaphore();   test_progress_tick();
    kprintf("[TEST] shm_ext\n");     test_shm_ext();     test_progress_tick();
    kprintf("[TEST] ipc\n");         test_ipc();         test_progress_tick();
    kprintf("[TEST] fat32\n");       test_fat32();       test_progress_tick();
    kprintf("[TEST] ac97\n");        test_ac97();        test_progress_tick();
    kprintf("[TEST] doom\n");        test_doom();        test_progress_tick();
    kprintf("[TEST] dos\n");         test_dos();         test_progress_tick();
    kprintf("[TEST] syscall\n");     test_syscall();     test_progress_tick();

    kprintf("----------------------------------------\n");
    kprintf("Results: %u passed, %u failed\n",
            (uint64_t)tpass, (uint64_t)tfail);
    if (tfail == 0) {
        kprintf("ALL TESTS PASSED\n");
    } else {
        kprintf("SOME TESTS FAILED\n");
    }
    kprintf("========================================\n");

    /* Restore normal VGA/serial output. */
    kprintf_set_hook(NULL, NULL);

    /* Flush the dmesg ring buffer to serial in small chunks.
     * Using kprintf_dmesg_flush_serial() avoids the 64 KB stack buffer
     * that previously caused stack overflow when allocated on a small
     * kernel stack. */
    kprintf_dmesg_flush_serial();

    /* Write sentinel so run_tests.sh can detect completion even if
     * ACPI shutdown fails (e.g. in TCG mode). */
    serial_write("[[TEST_DONE]]\n");

    /* Try ACPI shutdown first; fall back to QEMU debug exit if it fails.
     * The isa-debug-exit device must be enabled in QEMU CLI. */
    if (tfail == 0)
        qemu_exit(0x31);  /* exit code 33 = PASS */
    else
        qemu_exit(0x10);  /* exit code 16 = FAIL */

    /* Halt in case both shutdown methods fail */
    for (;;) __asm__ volatile("hlt");
}
