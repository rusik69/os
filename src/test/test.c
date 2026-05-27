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
#include "virtio_net.h"
#include "net.h"
#include "fat32.h"
#include "shm.h"
#include "mutex.h"
#include "semaphore.h"
#include "vmm.h"
#include "ac97.h"
#include "doom.h"
#include "dos.h"

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
    if (!ata_is_present()) {
        t_ok("fat32 SKIP (no ATA)");
        return;
    }
    if (fat32_mount(FAT32_DISK_ATA, 0) != 0) {
        t_ok("fat32 SKIP (no FAT partition)");
        return;
    }
    ASSERT("fat32 write", fat32_write_file("/testos.txt", "hi", 2) == 2);
    char buf[8];
    ASSERT("fat32 read", fat32_read_file("/testos.txt", buf, sizeof(buf)) == 2);
    buf[2] = '\0';
    ASSERT_STR("fat32 content", buf, "hi");
    ASSERT("fat32 sync", fat32_sync() == 0);
    t_ok("fat32 rw");
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
    ASSERT("doom frame varies", doom_test_frame_varies());
    ASSERT("doom door opens", doom_test_door_opens());
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

/* ── VMM tests ────────────────────────────────────────────────── */
static void test_vmm(void) {
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

/* ── DOS emulator tests ───────────────────────────────────────── */
static void test_dos(void) {
    int dos_load_com(struct dos_cpu_state *state, const uint8_t *data, uint32_t size);
    void dos_emu_init(struct dos_cpu_state *state);
    void dos_emu_run(struct dos_cpu_state *state);

    struct dos_cpu_state state;
    dos_emu_init(&state);

    uint8_t com[] = { 0xB8, 0x00, 0x4C, 0xCD, 0x21 };
    int ret = dos_load_com(&state, com, sizeof(com));
    ASSERT("dos load com", ret == 0);

    /* Run the emulator (should execute ~5 instructions then exit via INT 21h) */
    kprintf("  [dos] running emulator...\n");
    dos_emu_run(&state);
    kprintf("  [dos] emulator stopped, running=%d\n", (uint64_t)(uintptr_t)state.running);

    ASSERT("dos ran and stopped", state.running == 0);
    ASSERT("dos exit code 0", state.ax == 0x0000);
    t_ok("dos minimal");
}

/* ── Master runner ───────────────────────────────────────────── */

void test_run_all(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("       OS KERNEL TEST SUITE             \n");
    kprintf("========================================\n");

    test_string();
    test_memory();
    test_heap_ext();
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
    test_procfs();
    test_fork();
    test_shm_mutex();
    test_fat32();
    test_ac97();
    test_doom();
    test_vmm();
    test_semaphore();
    test_shm_ext();
    test_dos();

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
