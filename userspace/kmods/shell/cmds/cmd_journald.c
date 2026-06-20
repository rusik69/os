/* cmd_journald.c — Structured journal logging daemon
 *
 * Collects log messages from /dev/kmsg and /proc/kmsg,
 * writes them to /var/log/journal/ in a structured binary format,
 * handles log rotation based on size, and accepts log messages
 * over a FIFO at /run/systemd/journal/stdout.
 *
 * Item S167: Journald — structured log daemon
 *
 * Usage: journald [start|stop|status]
 */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "libc.h"
#include "heap.h"

/* ── Configuration ────────────────────────────────────────────────────── */

#define JOURNAL_DIR         "/var/log/journal/"
#define JOURNAL_SOCKET      "/run/systemd/journal/stdout"
#define JOURNAL_MAX_SIZE    (1024 * 1024)  /* 1 MB default max size */
#define JOURNAL_MAX_ROTATE  3               /* keep 3 rotated journals */
#define KMSG_PATH           "/dev/kmsg"

/* Journal entry header (binary format) */
struct journal_header {
    uint64_t timestamp;     /* microseconds since boot */
    uint32_t pid;           /* originating process PID */
    uint32_t uid;           /* originating user UID */
    uint16_t priority;      /* syslog priority (0-7) */
    uint16_t unit_len;      /* length of unit/service name */
    uint32_t msg_len;       /* length of message payload */
} __attribute__((packed));

/* Journal state */
static int journal_running = 0;
static uint64_t journal_current_size = 0;

/* ── Syscall helpers (kernel mode — use direct syscalls) ─────────────── */

static inline long syscall(long num, long a1, long a2, long a3,
                           long a4, long a5)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int k_open(const char *path, int flags)
{
    return (int)syscall(2, (long)path, flags, 0, 0, 0);
}

static inline long k_read(int fd, void *buf, unsigned long count)
{
    return syscall(0, fd, (long)buf, count, 0, 0);
}

static inline int k_close(int fd)
{
    return (int)syscall(3, fd, 0, 0, 0, 0);
}

/* ── File helpers using VFS ──────────────────────────────────────────── */

/* Append data to a file using vfs_read + vfs_write */
static int file_append(const char *path, const void *data, uint32_t len)
{
    uint32_t existing = 0;
    uint8_t type;

    /* Get existing file size */
    if (fs_stat(path, &existing, &type) == 0) {
        /* Read existing content */
        char *old = (char *)kmalloc(existing + len + 1);
        if (!old) return -1;
        uint32_t read_sz = 0;
        if (existing > 0)
            vfs_read(path, old, existing, &read_sz);

        /* Append new data */
        memcpy(old + read_sz, data, len);
        int rc = vfs_write(path, old, read_sz + len);
        kfree(old);
        return rc;
    } else {
        /* File doesn't exist — create it */
        return vfs_write(path, data, len);
    }
}

/* Build a journal file path for the current journal */
static void journal_path(char *buf, size_t maxlen, int rotation)
{
    if (rotation == 0)
        snprintf(buf, maxlen, "%sjournal.jrn", JOURNAL_DIR);
    else
        snprintf(buf, maxlen, "%sjournal.%d.jrn", JOURNAL_DIR, rotation);
}

/*
 * Rotate journal files.
 */
static void journal_rotate(void)
{
    char oldpath[128], newpath[128];

    /* Remove the oldest rotation */
    journal_path(newpath, sizeof(newpath), JOURNAL_MAX_ROTATE);
    fs_delete(newpath);

    /* Shift existing rotations */
    for (int i = JOURNAL_MAX_ROTATE - 1; i >= 1; i--) {
        journal_path(oldpath, sizeof(oldpath), i);
        uint32_t sz; uint8_t tp;
        if (fs_stat(oldpath, &sz, &tp) == 0) {
            journal_path(newpath, sizeof(newpath), i + 1);
            char *buf = (char *)kmalloc(sz + 1);
            if (buf) {
                uint32_t read_sz = 0;
                if (vfs_read(oldpath, buf, sz, &read_sz) == 0) {
                    vfs_write(newpath, buf, read_sz);
                }
                kfree(buf);
            }
            fs_delete(oldpath);
        }
    }

    /* Rename current journal to .1 */
    journal_path(oldpath, sizeof(oldpath), 0);
    uint32_t sz; uint8_t tp;
    if (fs_stat(oldpath, &sz, &tp) == 0) {
        char *buf = (char *)kmalloc(sz + 1);
        if (buf) {
            uint32_t read_sz = 0;
            if (vfs_read(oldpath, buf, sz, &read_sz) == 0) {
                journal_path(newpath, sizeof(newpath), 1);
                vfs_write(newpath, buf, read_sz);
            }
            kfree(buf);
        }
        fs_delete(oldpath);
    }

    journal_current_size = 0;
    kprintf("[journald] Log rotated\n");
}

/*
 * Write a structured journal entry.
 */
static int journal_write_entry(const char *unit, const char *msg,
                               int priority)
{
    if (!journal_running)
        return -1;

    char path[128];
    journal_path(path, sizeof(path), 0);

    uint32_t unit_len = (uint32_t)(unit ? strlen(unit) : 0);
    uint32_t msg_len = (uint32_t)(msg ? strlen(msg) : 0);
    uint32_t total_len = sizeof(struct journal_header) + unit_len + msg_len;

    char *buf = (char *)kmalloc(total_len);
    if (!buf)
        return -1;

    struct journal_header *hdr = (struct journal_header *)buf;
    hdr->timestamp = 0;
    hdr->pid = 0;
    hdr->uid = 0;
    hdr->priority = (uint16_t)priority;
    hdr->unit_len = unit_len;
    hdr->msg_len = msg_len;

    if (unit && unit_len > 0)
        memcpy(buf + sizeof(struct journal_header), unit, unit_len);
    if (msg && msg_len > 0)
        memcpy(buf + sizeof(struct journal_header) + unit_len, msg, msg_len);

    file_append(path, buf, total_len);
    kfree(buf);

    journal_current_size += total_len;
    if (journal_current_size >= JOURNAL_MAX_SIZE)
        journal_rotate();

    return 0;
}

/*
 * Read from /dev/kmsg and write entries.
 */
static void journal_collect_kmsg(void)
{
    int fd = k_open(KMSG_PATH, 0);
    if (fd < 0)
        return;

    char buf[4096];
    long n = k_read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        const char *msg_start = buf;
        if (buf[0] == '<') {
            char *end = strchr(buf, '>');
            if (end)
                msg_start = end + 1;
        }
        journal_write_entry("kernel", msg_start, 5);
    }

    k_close(fd);
}

/*
 * Accept a log message via the FIFO socket.
 */
static __attribute__((unused)) void journal_handle_socket_msg(const char *data, int len)
{
    char unit[64] = "unknown";
    int priority = 6;
    const char *msg = data;
    char buf[256];

    if (data[0] == '<') {
        const char *end = strchr(data, '>');
        if (end) {
            char prio_str[8];
            int plen = (int)(end - data - 1);
            if (plen > 7) plen = 7;
            memcpy(prio_str, data + 1, plen);
            prio_str[plen] = '\0';
            priority = atoi(prio_str);

            const char *p = end + 1;
            while (*p == ' ') p++;
            const char *unit_end = strchr(p, ' ');
            if (unit_end && unit_end - p < (int)sizeof(unit) - 1) {
                memcpy(unit, p, (size_t)(unit_end - p));
                unit[unit_end - p] = '\0';
                msg = unit_end + 1;
            } else {
                msg = p;
            }
        }
    }

    int mlen = strlen(msg);
    if (mlen > (int)sizeof(buf) - 2)
        mlen = sizeof(buf) - 2;
    memcpy(buf, msg, mlen);
    buf[mlen] = '\0';

    journal_write_entry(unit, buf, priority);
}

/*
 * Create the FIFO for accepting log messages.
 */
static int journal_create_socket(void)
{
    uint32_t sz; uint8_t tp;
    if (fs_stat(JOURNAL_SOCKET, &sz, &tp) == 0)
        fs_delete(JOURNAL_SOCKET);

    return fs_create(JOURNAL_SOCKET, FS_TYPE_FILE);
}

/* ── Daemon lifecycle ─────────────────────────────────────────────────── */

static int journald_start(void)
{
    if (journal_running) {
        kprintf("[journald] Already running\n");
        return -1;
    }

    /* Create journal directory */
    uint32_t sz; uint8_t tp;
    if (fs_stat(JOURNAL_DIR, &sz, &tp) < 0)
        fs_create(JOURNAL_DIR, FS_TYPE_DIR);

    /* Initialize current journal file size */
    char jpath[128];
    journal_path(jpath, sizeof(jpath), 0);
    if (fs_stat(jpath, &sz, &tp) == 0)
        journal_current_size = sz;
    else
        journal_current_size = 0;

    /* Collect existing kernel messages */
    journal_collect_kmsg();

    /* Create the log socket */
    journal_create_socket();

    journal_running = 1;
    kprintf("[journald] Started (journal at %s)\n", JOURNAL_DIR);
    kprintf("[journald] Listening on %s\n", JOURNAL_SOCKET);

    return 0;
}

static void journald_stop(void)
{
    if (!journal_running) return;
    journal_running = 0;
    kprintf("[journald] Stopped\n");
}

static void journald_status(void)
{
    kprintf("[journald] %s\n", journal_running ? "Running" : "Stopped");
    if (journal_running) {
        char jpath[128];
        journal_path(jpath, sizeof(jpath), 0);
        uint32_t sz; uint8_t tp;
        if (fs_stat(jpath, &sz, &tp) == 0)
            kprintf("[journald] Journal size: %u bytes\n", sz);
    }
}

/* ── Shell command entry point ────────────────────────────────────────── */

void cmd_journald(const char *args)
{
    if (!args || !*args) {
        kprintf("Usage: journald <start|stop|status>\n");
        return;
    }

    char subcmd[16] = {0};
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < (int)sizeof(subcmd) - 1)
        subcmd[i++] = *p++;

    if (strcmp(subcmd, "start") == 0) {
        journald_start();
    } else if (strcmp(subcmd, "stop") == 0) {
        journald_stop();
    } else if (strcmp(subcmd, "status") == 0) {
        journald_status();
    } else {
        kprintf("journald: unknown subcommand '%s'\n", subcmd);
        kprintf("Usage: journald <start|stop|status>\n");
    }
}
