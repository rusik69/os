/*
 * src/test/stress/stress_disk.c — Disk I/O stress test for Hermes OS
 *
 * Writes and reads large files repeatedly to stress the filesystem and
 * block device layers.
 *
 * Usage:
 *   stress_disk [num_iterations] [file_size_kb] [path]
 *
 * Defaults: num_iterations=20, file_size_kb=512, path=/stress_test.dat
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "sys/wait.h"
#include "fcntl.h"

#define DEFAULT_ITERATIONS 20
#define DEFAULT_SIZE_KB   512
#define DEFAULT_PATH      "/stress_test.dat"

static int do_stress_write(const char *path, size_t size_kb)
{
    size_t size = (size_t)size_kb * 1024;
    unsigned char *buf = (unsigned char *)malloc(size);
    if (!buf) {
        printf("[stress_disk] malloc(%zu) failed\n", size);
        return -1;
    }

    /* Fill with pattern */
    for (size_t i = 0; i < size; i++)
        buf[i] = (unsigned char)(i & 0xFF);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("[stress_disk] open(%s) for write failed\n", path);
        free(buf);
        return -1;
    }

    ssize_t written = write(fd, buf, size);
    if (written < 0 || (size_t)written != size) {
        printf("[stress_disk] write failed: wrote %zd/%zu bytes\n",
               written, size);
        close(fd);
        free(buf);
        return -1;
    }

    close(fd);
    free(buf);
    return 0;
}

static int do_stress_read(const char *path, size_t size_kb)
{
    size_t size = (size_t)size_kb * 1024;
    unsigned char *buf = (unsigned char *)malloc(size);
    if (!buf) {
        printf("[stress_disk] malloc(%zu) failed\n", size);
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("[stress_disk] open(%s) for read failed\n", path);
        free(buf);
        return -1;
    }

    ssize_t read_bytes = read(fd, buf, size);
    if (read_bytes < 0 || (size_t)read_bytes != size) {
        printf("[stress_disk] read failed: read %zd/%zu bytes\n",
               read_bytes, size);
        close(fd);
        free(buf);
        return -1;
    }

    /* Verify pattern */
    for (size_t i = 0; i < size; i++) {
        if (buf[i] != (unsigned char)(i & 0xFF)) {
            printf("[stress_disk] corruption at offset %zu: "
                   "expected 0x%02x, got 0x%02x\n",
                   i, (unsigned char)(i & 0xFF), buf[i]);
            close(fd);
            free(buf);
            return -1;
        }
    }

    close(fd);
    free(buf);
    return 0;
}

int main(int argc, char *argv[])
{
    int iterations = DEFAULT_ITERATIONS;
    int size_kb = DEFAULT_SIZE_KB;
    const char *path = DEFAULT_PATH;

    if (argc > 1)
        iterations = atoi(argv[1]);
    if (argc > 2)
        size_kb = atoi(argv[2]);
    if (argc > 3)
        path = argv[3];

    if (iterations < 1) iterations = 1;
    if (iterations > 1000) iterations = 1000;
    if (size_kb < 4) size_kb = 4;
    if (size_kb > 65536) size_kb = 65536;

    printf("[stress_disk] Running %d iterations, %d KB file at %s\n",
           iterations, size_kb, path);

    for (int i = 0; i < iterations; i++) {
        /* Write phase */
        if (do_stress_write(path, (size_t)size_kb) != 0) {
            printf("[stress_disk] FAIL: write failed at iteration %d\n", i);
            return 1;
        }

        /* Read + verify phase */
        if (do_stress_read(path, (size_t)size_kb) != 0) {
            printf("[stress_disk] FAIL: read/verify failed at iteration %d\n", i);
            return 1;
        }

        /* Remove the file for next iteration */
        unlink(path);

        if (i % 5 == 0 && i > 0) {
            printf("[stress_disk] Iteration %d/%d completed\n",
                   i, iterations);
        }
    }

    printf("[stress_disk] PASS: %d iterations completed (%d KB each)\n",
           iterations, size_kb);
    return 0;
}
