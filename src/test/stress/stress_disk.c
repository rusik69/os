/*
 * src/test/stress/stress_disk.c — Comprehensive disk I/O stress test
 *
 * Tests:
 *   - Sequential large-file write/read (100MB+ in chunks)
 *   - Random access patterns (lseek + small read/write)
 *   - Multiple concurrent file I/O
 *   - File creation/deletion storms
 *   - Write-buffer verification (write pattern, read back, verify)
 *
 * Usage:
 *   stress_disk [duration_seconds] [temp_dir]
 *
 * Defaults: duration=30, temp_dir=/stress_tmp
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

#define DEFAULT_DURATION 30
#define DEFAULT_DIR      "/stress_tmp"

#define CHUNK_SIZE       (64 * 1024)    /* 64 KB chunks */
#define LARGE_FILE_SIZE  (4 * 1024 * 1024)  /* 4 MB per large file */
#define MAX_FILES        16
#define SMALL_FILE_SIZE  4096
#define STORM_FILE_COUNT 100
#define NUM_RANDOM_OPS   500
#define CLOCK_REALTIME   0

/* ssize_t not provided by userspace libc headers */
typedef long ssize_t;

/* ── Stats ──────────────────────────────────────────────────── */
static unsigned long g_bytes_written   = 0;
static unsigned long g_bytes_read      = 0;
static unsigned long g_files_created   = 0;
static unsigned long g_files_deleted   = 0;
static unsigned long g_write_ops       = 0;
static unsigned long g_read_ops        = 0;
static unsigned long g_random_ops      = 0;
static unsigned long g_verify_errors   = 0;
static unsigned long g_io_errors       = 0;

/* ── Timer helpers ──────────────────────────────────────────── */

static struct timespec ts_start;
static int timer_init = 0;

static double elapsed(void)
{
    struct timespec now;
    if (!timer_init) {
        clock_gettime(CLOCK_REALTIME, &ts_start);
        timer_init = 1;
        return 0.0;
    }
    clock_gettime(CLOCK_REALTIME, &now);
    double s = (double)(now.tv_sec - ts_start.tv_sec);
    s += (double)(now.tv_nsec - ts_start.tv_nsec) / 1e9;
    return s;
}

static void reset_timer(void)
{
    timer_init = 0;
}

/* ── Helper: fill buffer with deterministic pattern ────────── */

static void fill_pattern(unsigned char *buf, unsigned long size,
                         unsigned long base)
{
    for (unsigned long i = 0; i < size; i++)
        buf[i] = (unsigned char)((base + i) & 0xFF);
}

/* ── Helper: verify buffer against pattern ─────────────────── */

static int verify_pattern(const unsigned char *buf, unsigned long size,
                          unsigned long base)
{
    for (unsigned long i = 0; i < size; i++) {
        if (buf[i] != (unsigned char)((base + i) & 0xFF)) {
            printf("[stress_disk] CORRUPTION at offset %lu: "
                   "expected 0x%02x got 0x%02x\n",
                   i, (unsigned char)((base + i) & 0xFF), buf[i]);
            return -1;
        }
    }
    return 0;
}

/* ── 1. Sequential large-file write/read ────────────────────── */

static int test_sequential(const char *dir, int id)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/seq_%d.dat", dir, id);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("[stress_disk] FAIL: open %s for write\n", path);
        g_io_errors++;
        return -1;
    }

    unsigned long total = 0;
    unsigned char *buf = (unsigned char *)malloc(CHUNK_SIZE);
    if (!buf) { close(fd); g_io_errors++; return -1; }

    while (total < LARGE_FILE_SIZE) {
        unsigned long remain = LARGE_FILE_SIZE - total;
        unsigned long chunk = (remain < CHUNK_SIZE) ? remain : CHUNK_SIZE;
        fill_pattern(buf, chunk, total);
        ssize_t w = write(fd, buf, chunk);
        if (w < 0 || (unsigned long)w != chunk) {
            printf("[stress_disk] write error at %lu: %ld/%lu\n",
                   total, (long)w, chunk);
            g_io_errors++;
            free(buf);
            close(fd);
            return -1;
        }
        g_bytes_written += (unsigned long)w;
        g_write_ops++;
        total += (unsigned long)w;
    }
    close(fd);

    /* Read back and verify */
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("[stress_disk] FAIL: open %s for read\n", path);
        g_io_errors++;
        free(buf);
        return -1;
    }

    total = 0;
    while (total < LARGE_FILE_SIZE) {
        unsigned long remain = LARGE_FILE_SIZE - total;
        unsigned long chunk = (remain < CHUNK_SIZE) ? remain : CHUNK_SIZE;
        ssize_t r = read(fd, buf, chunk);
        if (r < 0 || (unsigned long)r != chunk) {
            printf("[stress_disk] read error at %lu: %ld/%lu\n",
                   total, (long)r, chunk);
            g_io_errors++;
            free(buf);
            close(fd);
            return -1;
        }
        if (verify_pattern(buf, chunk, total) != 0) {
            g_verify_errors++;
            free(buf);
            close(fd);
            return -1;
        }
        g_bytes_read += (unsigned long)r;
        g_read_ops++;
        total += (unsigned long)r;
    }
    close(fd);
    free(buf);

    /* Cleanup */
    unlink(path);
    g_files_deleted++;
    return 0;
}

/* ── 2. Random access patterns ──────────────────────────────── */

static int test_random_access(const char *dir, int id)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/rand_%d.dat", dir, id);

    /* Create a file of RANDOM_FILE_SIZE */
    unsigned long file_size = 1024 * 1024; /* 1 MB */
    unsigned char *buf = (unsigned char *)malloc(4096);
    if (!buf) { g_io_errors++; return -1; }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(buf); g_io_errors++; return -1; }

    fill_pattern(buf, 4096, 0);
    for (unsigned long i = 0; i < file_size; i += 4096) {
        if (write(fd, buf, 4096) != 4096) {
            g_io_errors++;
            close(fd); free(buf); return -1;
        }
        g_bytes_written += 4096;
        g_write_ops++;
    }
    close(fd);

    /* Random read/write patterns */
    fd = open(path, O_RDWR);
    if (fd < 0) { free(buf); g_io_errors++; return -1; }

    srand(42 + id);
    for (int i = 0; i < NUM_RANDOM_OPS; i++) {
        unsigned long offset = (unsigned long)(rand() % (int)(file_size - 4096));
        /* Align to 512 bytes */
        offset = (offset / 512) * 512;
        if (offset + 512 > file_size) offset = file_size - 512;

        if (i % 2 == 0) {
            /* Write */
            fill_pattern(buf, 512, offset);
            if (lseek(fd, (long)offset, SEEK_SET) < 0 ||
                write(fd, buf, 512) != 512) {
                g_io_errors++;
            } else {
                g_bytes_written += 512;
                g_write_ops++;
            }
        } else {
            /* Read and verify */
            if (lseek(fd, (long)offset, SEEK_SET) < 0 ||
                read(fd, buf, 512) != 512) {
                g_io_errors++;
            } else {
                if (verify_pattern(buf, 512, offset) != 0)
                    g_verify_errors++;
                g_bytes_read += 512;
                g_read_ops++;
            }
        }
        g_random_ops++;
    }
    close(fd);
    free(buf);
    unlink(path);
    g_files_deleted++;
    return 0;
}

/* ── 3. Multiple concurrent files I/O ───────────────────────── */

static int test_concurrent_files(const char *dir)
{
    int fds[MAX_FILES];
    char paths[MAX_FILES][64];
    unsigned char *buf = (unsigned char *)malloc(4096);
    if (!buf) { g_io_errors++; return -1; }

    /* Open many files */
    for (int i = 0; i < MAX_FILES; i++) {
        snprintf(paths[i], sizeof(paths[i]), "%s/conc_%d.dat", dir, i);
        fds[i] = open(paths[i], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fds[i] < 0) {
            for (int j = 0; j < i; j++) { close(fds[j]); unlink(paths[j]); }
            free(buf);
            g_io_errors++;
            return -1;
        }
        g_files_created++;
    }

    /* Write round-robin to each */
    for (int round = 0; round < 10; round++) {
        for (int i = 0; i < MAX_FILES; i++) {
            fill_pattern(buf, 4096, (unsigned long)(round * MAX_FILES + i) * 4096);
            ssize_t w = write(fds[i], buf, 4096);
            if (w != 4096) {
                g_io_errors++;
            } else {
                g_bytes_written += 4096;
                g_write_ops++;
            }
        }
    }

    /* Close all */
    for (int i = 0; i < MAX_FILES; i++)
        close(fds[i]);

    /* Reopen and verify */
    for (int i = 0; i < MAX_FILES; i++) {
        fds[i] = open(paths[i], O_RDONLY);
        if (fds[i] < 0) { g_io_errors++; continue; }

        unsigned long pos = 0;
        for (int round = 0; round < 10; round++) {
            ssize_t r = read(fds[i], buf, 4096);
            if (r != 4096) {
                g_io_errors++;
            } else {
                if (verify_pattern(buf, 4096, pos) != 0)
                    g_verify_errors++;
                g_bytes_read += 4096;
                g_read_ops++;
            }
            pos += 4096;
        }
        close(fds[i]);
        unlink(paths[i]);
        g_files_deleted++;
    }

    free(buf);
    return 0;
}

/* ── 4. File creation/deletion storm ────────────────────────── */

static int test_file_storm(const char *dir)
{
    char path[64];
    unsigned char *buf = (unsigned char *)malloc(SMALL_FILE_SIZE);
    if (!buf) { g_io_errors++; return -1; }

    fill_pattern(buf, SMALL_FILE_SIZE, 0xDEAD);

    /* Create many small files */
    for (int i = 0; i < STORM_FILE_COUNT; i++) {
        snprintf(path, sizeof(path), "%s/storm_%d.tmp", dir, i);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            g_io_errors++;
            continue;
        }
        ssize_t w = write(fd, buf, SMALL_FILE_SIZE);
        if (w != SMALL_FILE_SIZE) {
            g_io_errors++;
        } else {
            g_bytes_written += SMALL_FILE_SIZE;
            g_write_ops++;
        }
        close(fd);
        g_files_created++;
    }

    /* Read them all back & verify */
    for (int i = 0; i < STORM_FILE_COUNT; i++) {
        snprintf(path, sizeof(path), "%s/storm_%d.tmp", dir, i);
        int fd = open(path, O_RDONLY);
        if (fd < 0) { continue; }
        ssize_t r = read(fd, buf, SMALL_FILE_SIZE);
        if (r != SMALL_FILE_SIZE) {
            g_io_errors++;
        } else {
            if (verify_pattern(buf, SMALL_FILE_SIZE, 0xDEAD) != 0)
                g_verify_errors++;
            g_bytes_read += SMALL_FILE_SIZE;
            g_read_ops++;
        }
        close(fd);
    }

    /* Delete them all */
    for (int i = 0; i < STORM_FILE_COUNT; i++) {
        snprintf(path, sizeof(path), "%s/storm_%d.tmp", dir, i);
        if (unlink(path) == 0)
            g_files_deleted++;
    }

    free(buf);
    return 0;
}

/* ── 5. Write-buffer verification (write, close, reopen, verify) ─ */

static int test_write_verify(const char *dir, int id)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/wvfy_%d.dat", dir, id);

    unsigned long file_size = 256 * 1024; /* 256 KB */
    unsigned char *buf = (unsigned char *)malloc(file_size);
    if (!buf) { g_io_errors++; return -1; }

    /* Write with specific pattern */
    fill_pattern(buf, file_size, 0xABCD);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(buf); g_io_errors++; return -1; }
    ssize_t w = write(fd, buf, file_size);
    if (w < 0 || (unsigned long)w != file_size) {
        g_io_errors++;
        close(fd); free(buf); return -1;
    }
    g_bytes_written += (unsigned long)w;
    g_write_ops++;
    close(fd);

    /* Read back into separate buffer */
    unsigned char *rbuf = (unsigned char *)malloc(file_size);
    if (!rbuf) { free(buf); g_io_errors++; return -1; }

    fd = open(path, O_RDONLY);
    if (fd < 0) { free(buf); free(rbuf); g_io_errors++; return -1; }
    ssize_t r = read(fd, rbuf, file_size);
    if (r < 0 || (unsigned long)r != file_size) {
        g_io_errors++;
        close(fd); free(buf); free(rbuf); return -1;
    }
    g_bytes_read += (unsigned long)r;
    g_read_ops++;
    close(fd);

    /* Verify exact match */
    int errors = 0;
    for (unsigned long i = 0; i < file_size; i++) {
        if (buf[i] != rbuf[i]) {
            printf("[stress_disk] W-VFY mismatch at %lu: "
                   "expected 0x%02x got 0x%02x\n",
                   i, buf[i], rbuf[i]);
            errors++;
            if (errors >= 5) break;
        }
    }
    if (errors > 0) {
        g_verify_errors += (unsigned long)errors;
        free(buf); free(rbuf);
        unlink(path);
        return -1;
    }

    free(buf);
    free(rbuf);
    unlink(path);
    g_files_deleted++;
    return 0;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    int duration = DEFAULT_DURATION;
    const char *dir = DEFAULT_DIR;

    if (argc > 1)
        duration = atoi(argv[1]);
    if (argc > 2)
        dir = argv[2];

    if (duration < 1) duration = 1;
    if (duration > 3600) duration = 3600;

    printf("\n============================================\n");
    printf("  Hermes OS Disk I/O Stress Test v2\n");
    printf("============================================\n");
    printf("  Duration:  %d seconds\n", duration);
    printf("  Temp dir:  %s\n", dir);
    printf("  Tests:\n");
    printf("    - Sequential large-file R/W with verify\n");
    printf("    - Random access patterns\n");
    printf("    - Concurrent multi-file I/O\n");
    printf("    - File creation/deletion storm\n");
    printf("    - Write-buffer verification\n");
    printf("============================================\n\n");

    reset_timer();
    double wall_start = elapsed();

    /* Create temp directory */
    mkdir(dir, 0755);

    int seq_count = 0;
    int rand_count = 0;
    int conc_count = 0;
    int storm_count = 0;
    int wvfy_count = 0;
    int test_errors = 0;

    while (elapsed() - wall_start < (double)duration) {
        /* Rotate through test types */
        int phase = ((int)(elapsed() - wall_start)) % 5;

        switch (phase) {
        case 0:
            /* Sequential large-file */
            if (test_sequential(dir, seq_count++) != 0)
                test_errors++;
            break;
        case 1:
            /* Random access */
            if (test_random_access(dir, rand_count++) != 0)
                test_errors++;
            break;
        case 2:
            /* Concurrent files */
            if (test_concurrent_files(dir) != 0)
                test_errors++;
            conc_count++;
            break;
        case 3:
            /* File storm */
            if (test_file_storm(dir) != 0)
                test_errors++;
            storm_count++;
            break;
        case 4:
            /* Write-verify */
            if (test_write_verify(dir, wvfy_count++) != 0)
                test_errors++;
            break;
        }

        if ((int)(elapsed() - wall_start) % 10 == 0 &&
            (int)(elapsed() - wall_start) > 0) {
            printf("[stress_disk] Progress: %.0fs elapsed, "
                   "%lu KB written, %lu KB read, %lu errors\n",
                   elapsed() - wall_start,
                   g_bytes_written / 1024, g_bytes_read / 1024,
                   g_verify_errors + g_io_errors);
        }

        /* Brief yield to not monopolize the system */
        yield();
    }

    double wall_elapsed = elapsed() - wall_start;

    /* Clean up temp dir */
    rmdir(dir);

    /* ── Summary Report ── */
    printf("\n");
    printf("============================================\n");
    printf("  DISK I/O STRESS TEST SUMMARY\n");
    printf("============================================\n");
    printf("  Wall clock:            %.2f s\n", wall_elapsed);
    printf("  Sequential tests:      %d\n", seq_count);
    printf("  Random-access tests:   %d\n", rand_count);
    printf("  Concurrent-file tests: %d\n", conc_count);
    printf("  File-storm tests:      %d\n", storm_count);
    printf("  Write-verify tests:    %d\n", wvfy_count);
    printf("\n");
    printf("  Write operations:      %lu\n", g_write_ops);
    printf("  Read operations:       %lu\n", g_read_ops);
    printf("  Random seeks:          %lu\n", g_random_ops);
    printf("  Bytes written:         %lu (%lu KB)\n",
           g_bytes_written, g_bytes_written / 1024);
    printf("  Bytes read:            %lu (%lu KB)\n",
           g_bytes_read, g_bytes_read / 1024);
    printf("  Files created:         %lu\n", g_files_created);
    printf("  Files deleted:         %lu\n", g_files_deleted);
    printf("\n");
    printf("  I/O errors:            %lu\n", g_io_errors);
    printf("  Verify errors:         %lu\n", g_verify_errors);
    printf("  Test-level errors:     %d\n", test_errors);
    printf("\n");

    if (g_verify_errors > 0 || g_io_errors > 0 || test_errors > 0) {
        printf("  >>> STATUS: FAIL <<<\n");
        return 1;
    }
    printf("  >>> STATUS: PASS <<<\n");
    printf("============================================\n");

    return 0;
}

/* ── Stub: stress_disk_run ─────────────────────────────── */
int stress_disk_run(const char *path, size_t size)
{
    (void)path;
    (void)size;
    kprintf("[stress] stress_disk_run: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: stress_disk_stop ─────────────────────────────── */
int stress_disk_stop(void)
{
    kprintf("[stress] stress_disk_stop: not yet implemented\n");
    return -ENOSYS;
}
