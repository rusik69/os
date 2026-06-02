/*
 * cmd_syslogd.c — syslogd: System Logger Daemon (U7)
 *
 * Receives kernel log messages via the SYSLOG system call, formats them
 * with RFC 3164-style timestamps, and appends them to /var/log/messages.
 * Rotates logs when the file exceeds a size threshold.
 *
 * Usage:
 *   syslogd           — run the daemon in the foreground
 *   syslogd &         — run in the background (shell job control)
 *   syslogd stop      — signal the daemon to stop
 *   syslogd status    — check daemon status
 *
 * Log rotation:
 *   When /var/log/messages grows beyond 64 KB, it is renamed to
 *   /var/log/messages.old and a new file is started.
 *
 * Implementation note:
 *   We use raw syscalls (via libc_syscall) for fd-based file I/O so we
 *   can seek to the end of the file before writing (append mode).
 *   The fd-based path avoids the fs_write_file overwrite semantic.
 */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"
#include "stdlib.h"
#include "syscall.h"

/* ── Constants ──────────────────────────────────────────────────────── */

/* Size threshold for log rotation (64 KB) */
#define SYSLOGD_MAX_LOG_SIZE      (64UL * 1024)

/* Polling interval in timer ticks (TIMER_FREQ = 100 Hz, so 200 = 2 sec) */
#define SYSLOGD_POLL_INTERVAL     200

/* File paths */
#define SYSLOGD_LOG_PATH          "/var/log/messages"
#define SYSLOGD_LOG_PATH_OLD      "/var/log/messages.old"

/* Maximum length of a single formatted log message line */
#define SYSLOGD_LINE_MAX          384

/* Maximum bytes to read from kernel log buffer per poll */
#define SYSLOGD_BUF_SIZE          8192

/* ── Stop flag ──────────────────────────────────────────────────────── */
/* Set to 1 by "syslogd stop" to gracefully terminate the daemon. */
static volatile int syslogd_stop_requested = 0;

/* ── Month name table for RFC 3164 timestamps ──────────────────────── */
static const char * const month_names[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* ── Helper macros for fd operations ────────────────────────────────── */

/* SYS_OPEN flags — we use O_WRONLY (1) */
#ifndef O_WRONLY
#define O_WRONLY 1
#endif

/* SEEK_END for lseek */
#ifndef SEEK_END
#define SEEK_END 2
#endif

/* ── Helper: get the current RTC time fields ────────────────────────── */
static void get_rtc_fields(unsigned int *year, unsigned int *month,
                           unsigned int *day, unsigned int *hour,
                           unsigned int *minute, unsigned int *second)
{
    struct libc_rtc_time rtc;
    if (libc_rtc_get_time(&rtc) == 0) {
        *year   = rtc.year;
        *month  = rtc.month;
        *day    = rtc.day;
        *hour   = rtc.hour;
        *minute = rtc.minute;
        *second = rtc.second;
    } else {
        /* RTC unavailable — use zeroes so we still produce valid output */
        *year = *month = *day = 0;
        *hour = *minute = *second = 0;
    }
}

/* ── Helper: build an RFC 3164 timestamp string "Oct  1 12:34:56" ──── */
static void format_timestamp(char *buf, int buf_size)
{
    unsigned int y, mo, d, h, mi, s;
    get_rtc_fields(&y, &mo, &d, &h, &mi, &s);

    if (mo < 1 || mo > 12) mo = 1;
    const char *mname = month_names[mo - 1];

    if (buf_size >= 16) {
        snprintf(buf, buf_size, "%s %2u %02u:%02u:%02u", mname, d, h, mi, s);
    }
}

/* ── Helper: parse an optional kernel syslog priority prefix (<N>) ────
 * Returns the severity (0-7), or -1 if no prefix is present.
 * Advances *msg past the prefix on success. */
static int parse_priority_prefix(const char **msg)
{
    const char *p = *msg;
    if (*p == '<') {
        p++;
        int prio = 0;
        while (*p >= '0' && *p <= '9') {
            prio = prio * 10 + (*p - '0');
            p++;
        }
        if (*p == '>') {
            *msg = p + 1;  /* skip past ">" */
            return prio & 7;  /* severity is low 3 bits of priority */
        }
    }
    return -1;
}

/* ── Helper: low-level fd-based file append ────────────────────────────
 * Opens the file, seeks to the end, writes the data, then closes.
 * If the file does not exist, it is created first via libc_fs_create().
 * Returns 0 on success, negative on error. */
static int fd_append(const char *path, const char *data, uint32_t len)
{
    /* Try to open the existing file */
    int fd = (int)libc_syscall(SYS_OPEN, (uint64_t)(uintptr_t)path,
                                O_WRONLY, 0, 0, 0);
    if (fd < 0) {
        /* File does not exist — create it */
        int ret = libc_fs_create(path, FS_TYPE_FILE);
        if (ret < 0 && ret != -1 /* -EEXIST */) {
            /* Ensure /var/log/ exists */
            libc_fs_create("/var", FS_TYPE_DIR);
            libc_fs_create("/var/log", FS_TYPE_DIR);
            ret = libc_fs_create(path, FS_TYPE_FILE);
            if (ret < 0 && ret != -1)
                return ret;
        }
        fd = (int)libc_syscall(SYS_OPEN, (uint64_t)(uintptr_t)path,
                                O_WRONLY, 0, 0, 0);
        if (fd < 0)
            return fd;
    }

    /* Seek to end for append */
    libc_lseek(fd, 0, SEEK_END);

    /* Write the data */
    int written = libc_fd_write(fd, data, len);

    /* Close */
    libc_syscall(SYS_CLOSE, (uint64_t)(int64_t)fd, 0, 0, 0, 0);

    return written;
}

/* ── Helper: get file size (returns -1 on error) ────────────────────── */
static long get_file_size(const char *path)
{
    uint32_t sz;
    uint8_t  type;
    if (libc_fs_stat(path, &sz, &type) == 0)
        return (long)sz;
    return -1;
}

/* ── Helper: rename a file via raw syscall ──────────────────────────── */
static int file_rename(const char *old_path, const char *new_path)
{
    return (int)libc_syscall(SYS_RENAME,
                              (uint64_t)(uintptr_t)old_path,
                              (uint64_t)(uintptr_t)new_path,
                              0, 0, 0);
}

/* ── Helper: append a complete log line to /var/log/messages ───────────
 * Rotates the log file if it exceeds SYSLOGD_MAX_LOG_SIZE. */
static int log_append_line(const char *line, uint32_t len)
{
    /* Check current size and rotate if needed */
    long sz = get_file_size(SYSLOGD_LOG_PATH);
    if (sz > 0 && (unsigned long)sz >= SYSLOGD_MAX_LOG_SIZE) {
        /* Remove old backup, rename current to .old */
        libc_fs_delete(SYSLOGD_LOG_PATH_OLD);
        file_rename(SYSLOGD_LOG_PATH, SYSLOGD_LOG_PATH_OLD);
    }

    /* Append to the log file */
    return fd_append(SYSLOGD_LOG_PATH, line, len);
}

/* ── Severity labels for syslog priority mapping ────────────────────── */
static const char * const sev_labels[8] = {
    "emerg",  "alert",  "crit", "error",
    "warn",   "notice", "info", "debug"
};

/* ── Core: poll kernel log and write formatted entries ────────────────
 * This is the main loop.  It reads the kernel message buffer via the
 * SYSLOG syscall (action READ_CLEAR), formats each line with a timestamp
 * and severity label, and appends it to the log file. */
static int syslogd_loop(void)
{
    char buf[SYSLOGD_BUF_SIZE];
    char line_buf[SYSLOGD_LINE_MAX];
    char timestamp[32];

    kprintf("[syslogd] started — logging kernel messages to %s\n",
            SYSLOGD_LOG_PATH);
    kprintf("[syslogd] polling every %u ticks, rotate at %lu bytes\n",
            (unsigned)SYSLOGD_POLL_INTERVAL,
            (unsigned long)SYSLOGD_MAX_LOG_SIZE);

    while (!syslogd_stop_requested) {
        /* Read (and clear) the kernel log buffer via SYS_SYSLOG.
         * SYS_SYSLOG = 279, SYSLOG_ACTION_READ_CLEAR = 4 */
        long copied = (long)libc_syscall(279, 4,
                                          (uint64_t)(uintptr_t)buf,
                                          (uint64_t)SYSLOGD_BUF_SIZE,
                                          0, 0);

        if (copied > 0) {
            /* Null-terminate for string processing */
            if (copied >= (long)SYSLOGD_BUF_SIZE)
                copied = (long)SYSLOGD_BUF_SIZE - 1;
            buf[copied] = '\0';

            /* Build one timestamp for this batch */
            format_timestamp(timestamp, sizeof(timestamp));

            /* Process each line in the buffer */
            char *line = buf;
            while (line && *line) {
                /* Find and null-terminate at newline */
                char *nl = strchr(line, '\n');
                if (nl) *nl = '\0';

                if (*line) {
                    const char *msg = (const char *)line;
                    int severity = parse_priority_prefix(&msg);

                    /* Skip leading spaces */
                    while (*msg == ' ') msg++;

                    int written;
                    if (severity >= 0 && severity < 8) {
                        written = snprintf(line_buf, sizeof(line_buf),
                                           "%s kern.%s: %s\n",
                                           timestamp,
                                           sev_labels[severity],
                                           msg);
                    } else {
                        written = snprintf(line_buf, sizeof(line_buf),
                                           "%s kern.info: %s\n",
                                           timestamp, msg);
                    }

                    if (written > 0 && written < (int)sizeof(line_buf)) {
                        log_append_line(line_buf, (uint32_t)written);
                    }
                }

                /* Advance to next line */
                if (nl)
                    line = nl + 1;
                else
                    break;
            }
        }

        /* Sleep between polls (but not if stop was requested) */
        if (!syslogd_stop_requested)
            libc_sleep_ticks(SYSLOGD_POLL_INTERVAL);
    }

    kprintf("[syslogd] stopped gracefully\n");
    return 0;
}

/* ── Public command entry point ─────────────────────────────────────── */
void cmd_syslogd(const char *args)
{
    if (args && *args) {
        /* Skip leading spaces */
        while (*args == ' ') args++;

        if (strcmp(args, "stop") == 0) {
            syslogd_stop_requested = 1;
            kprintf("[syslogd] stop signal sent\n");
            return;
        }

        if (strcmp(args, "status") == 0) {
            kprintf("syslogd: %s\n",
                    syslogd_stop_requested ? "stopped" : "running");
            long sz = get_file_size(SYSLOGD_LOG_PATH);
            if (sz >= 0) {
                kprintf("syslogd: %s is %ld bytes\n",
                        SYSLOGD_LOG_PATH, sz);
            } else {
                kprintf("syslogd: %s does not exist yet\n",
                        SYSLOGD_LOG_PATH);
            }
            return;
        }

        kprintf("syslogd: unknown argument '%s'\n", args);
        kprintf("Usage: syslogd           — start daemon\n");
        kprintf("       syslogd stop      — stop daemon\n");
        kprintf("       syslogd status    — check daemon status\n");
        return;
    }

    /* Reset stop flag and run the main loop */
    syslogd_stop_requested = 0;
    syslogd_loop();
}
