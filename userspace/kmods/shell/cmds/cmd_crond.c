/*
 * cmd_crond.c — crond: Cron Daemon (Item U10)
 *
 * Scheduled task execution daemon that parses /etc/crontab and
 * user crontabs, checking every minute for tasks to run.
 *
 * Crontab format (standard):
 *   min hour day month dayofweek user command
 *
 * Special keywords: @reboot, @daily, @hourly, @weekly, @monthly
 *
 * Usage:
 *   crond              — start the cron daemon
 *   crond stop         — stop the daemon
 *   crond status       — show daemon status
 *   crond reload       — reload crontab files
 *
 * Implementation:
 *   The daemon runs as a kernel-threaded process invoked from the shell.
 *   It reads crontab files into memory, then sleeps most of the time,
 *   waking every ~1 second to check if a new minute has started.
 *   When the minute changes, it evaluates all cron entries and runs
 *   matching jobs via libc_fork() + libc_shell_exec_cmd().
 */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"
#include "syscall.h"

/* ── Constants ──────────────────────────────────────────────────────── */

/* Polling interval in timer ticks (TIMER_FREQ = 100 Hz, so 10 = 100 ms) */
#define CROND_POLL_INTERVAL      10

/* Number of ticks corresponding to 60 seconds */
#define CROND_TICKS_PER_MINUTE   6000

/* Maximum line length in a crontab file */
#define CROND_MAX_LINE           512

/* Maximum number of cron jobs we can track */
#define CROND_MAX_JOBS           256

/* Maximum path length */
#define CROND_MAX_PATH           128

/* Crontab paths */
#define CROND_SYSTEM_CRONTAB     "/etc/crontab"
#define CROND_SPOOL_DIR          "/var/spool/cron"
#define CROND_LOG_PATH           "/var/log/cron"
#define CROND_PID_PATH           "/var/run/crond.pid"

/* ── Cron job entry ────────────────────────────────────────────────── */

struct cron_job {
    int    active;           /* 1 = slot in use */
    int    is_reboot;        /* 1 = @reboot (run once at daemon start) */
    int    reboot_done;      /* 1 = @reboot job has been executed */
    int    minute;           /* -1 = wildcard (*) */
    int    hour;
    int    day;
    int    month;
    int    dayofweek;        /* 0=Sun, 1=Mon, ..., 6=Sat */
    char   user[32];         /* user to run job as (empty = current user) */
    char   command[CROND_MAX_LINE];
};

/* ── Daemon state ──────────────────────────────────────────────────── */

static volatile int crond_stop_requested = 0;
static volatile int crond_running       = 0;

/* Job table */
static struct cron_job crond_jobs[CROND_MAX_JOBS];
static int crond_num_jobs = 0;

/* Last minute we processed (epoch seconds / 60) to detect minute rollover */
static uint64_t crond_last_minute = 0;

/* ── Date/time helpers ─────────────────────────────────────────────── */

/*
 * Days in each month (non-leap year).  Index by month 1..12.
 */
static const int days_in_month[13] = {
    0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

/* Returns 1 if 'year' is a leap year (year is absolute, e.g. 2026) */
static int crond_is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/*
 * Break epoch seconds into calendar fields.
 * Uses the standard Gregorian calendar algorithm.
 */
static void crond_epoch_to_fields(uint64_t epoch_secs,
                                  int *year, int *month, int *day,
                                  int *hour, int *minute, int *wday) {
    /* Days since Unix epoch (1970-01-01 was a Thursday) */
    uint64_t days = epoch_secs / 86400;
    uint64_t remaining = epoch_secs % 86400;

    /* Day of week: epoch was Thursday (4) */
    *wday = (int)((days + 4) % 7);

    /* Hour and minute */
    *hour   = (int)(remaining / 3600);
    *minute = (int)((remaining % 3600) / 60);

    /* Year calculation */
    int y = 1970;
    while (1) {
        int days_in_year = crond_is_leap(y) ? 366 : 365;
        if ((uint64_t)days_in_year > days) break;
        days -= days_in_year;
        y++;
    }
    *year = y;

    /* Month calculation */
    int days_yr = (int)days;
    int m;
    for (m = 1; m <= 12; m++) {
        int dim = days_in_month[m];
        if (m == 2 && crond_is_leap(y)) dim = 29;
        if (days_yr < dim) break;
        days_yr -= dim;
    }
    *month = m;
    *day = days_yr + 1;  /* days_yr is 0-indexed */
}

/* ── Crontab parsing ───────────────────────────────────────────────── */

/*
 * Parse a single crontab line.
 * Returns 1 if the line was parsed into a job, 0 otherwise.
 *
 * Standard format:
 *   min hour day month dayofweek user command
 * Special:
 *   @reboot    user command
 *   @daily     user command   (same as 0 0 * * *)
 *   @hourly    user command   (same as 0 * * * *)
 *   @weekly    user command   (same as 0 0 * * 0)
 *   @monthly   user command   (same as 0 0 1 * *)
 *   # comments ignored
 *   blank lines ignored
 */
static int crond_parse_line(const char *line, struct cron_job *job) {
    /* Clean slate */
    memset(job, 0, sizeof(*job));
    job->minute = -1;
    job->hour   = -1;
    job->day    = -1;
    job->month  = -1;
    job->dayofweek = -1;

    /* Skip leading whitespace */
    while (*line == ' ' || *line == '\t') line++;

    /* Empty line or comment */
    if (*line == '\0' || *line == '#' || *line == '\n') return 0;

    /* Check for @-prefixed keywords */
    if (line[0] == '@') {
        const char *p = line + 1;
        const char *kw = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        int kwlen = (int)(p - kw);

        /* Skip whitespace after keyword */
        while (*p == ' ' || *p == '\t') p++;

        /* Extract user (next token) */
        char user[32];
        int ui = 0;
        while (*p && *p != ' ' && *p != '\t' && ui < 31 && *p != '\n')
            user[ui++] = *p++;
        user[ui] = '\0';

        /* Skip whitespace after user */
        while (*p == ' ' || *p == '\t') p++;

        /* Rest is the command */
        char cmd[CROND_MAX_LINE];
        int ci = 0;
        while (*p && *p != '\n' && ci < CROND_MAX_LINE - 1)
            cmd[ci++] = *p++;
        cmd[ci] = '\0';

        if (ci == 0) return 0; /* No command */

        int matched = 0;
        /* @reboot */
        if (kwlen == 5 && memcmp(kw, "reboot", 5) == 0) {
            job->is_reboot = 1;
            matched = 1;
        }
        /* @daily or @midnight */
        else if ((kwlen == 5 && memcmp(kw, "daily", 5) == 0) ||
                 (kwlen == 8 && memcmp(kw, "midnight", 8) == 0)) {
            job->minute = 0;
            job->hour   = 0;
            matched = 1;
        }
        /* @hourly */
        else if (kwlen == 6 && memcmp(kw, "hourly", 6) == 0) {
            job->minute = 0;
            matched = 1;
        }
        /* @weekly */
        else if (kwlen == 6 && memcmp(kw, "weekly", 6) == 0) {
            job->minute = 0;
            job->hour   = 0;
            job->dayofweek = 0; /* Sunday */
            matched = 1;
        }
        /* @monthly */
        else if (kwlen == 7 && memcmp(kw, "monthly", 7) == 0) {
            job->minute = 0;
            job->hour   = 0;
            job->day    = 1;
            matched = 1;
        }
        /* @yearly or @annually */
        else if ((kwlen == 6 && memcmp(kw, "yearly", 6) == 0) ||
                 (kwlen == 8 && memcmp(kw, "annually", 8) == 0)) {
            job->minute = 0;
            job->hour   = 0;
            job->day    = 1;
            job->month  = 1;
            matched = 1;
        }

        if (!matched) return 0;

        strncpy(job->user, user, 31);
        job->user[31] = '\0';
        strncpy(job->command, cmd, CROND_MAX_LINE - 1);
        job->command[CROND_MAX_LINE - 1] = '\0';
        job->active = 1;
        return 1;
    }

    /* Standard 6-field format: min hour day month dayofweek user command */
    int fields[5];
    char user_buf[32];
    char cmd_buf[CROND_MAX_LINE];
    int field_idx = 0;
    int parsed_ok = 1;
    int ci = 0;
    const char *scan = line;

    /* Parse 5 time fields, then user, then command */
    while (*scan && field_idx < 5) {
        /* Skip leading whitespace */
        while (*scan == ' ' || *scan == '\t') scan++;
        if (*scan == '\0' || *scan == '\n') { parsed_ok = 0; break; }

        if (*scan == '*') {
            fields[field_idx] = -1; /* wildcard */
            scan++;
        } else if (*scan >= '0' && *scan <= '9') {
            int val = 0;
            while (*scan >= '0' && *scan <= '9')
                val = val * 10 + (*scan++ - '0');
            fields[field_idx] = val;
        } else {
            parsed_ok = 0;
            break;
        }
        field_idx++;
        /* Skip whitespace to next field */
        while (*scan == ' ' || *scan == '\t') scan++;
    }

    if (!parsed_ok || field_idx < 5) return 0;

    /* Parse user field */
    {
        int ui = 0;
        while (*scan && *scan != ' ' && *scan != '\t' && ui < 31 && *scan != '\n')
            user_buf[ui++] = *scan++;
        user_buf[ui] = '\0';
        if (ui == 0) return 0; /* missing user */
    }

    /* Skip whitespace to command */
    while (*scan == ' ' || *scan == '\t') scan++;

    /* Parse command (rest of line) */
    {
        ci = 0;
        while (*scan && *scan != '\n' && ci < CROND_MAX_LINE - 1)
            cmd_buf[ci++] = *scan++;
        cmd_buf[ci] = '\0';
        if (ci == 0) return 0; /* missing command */
    }

    /* Fill in the job entry */
    job->minute = fields[0];
    job->hour   = fields[1];
    job->day    = fields[2];
    job->month  = fields[3];
    job->dayofweek = fields[4];

    strncpy(job->user, user_buf, 31);
    job->user[31] = '\0';
    strncpy(job->command, cmd_buf, CROND_MAX_LINE - 1);
    job->command[CROND_MAX_LINE - 1] = '\0';
    job->active = 1;
    return 1;
}

/*
 * Load crontab entries from a single file.
 * Returns number of jobs loaded, or -1 on error.
 */
static int crond_load_file(const char *path) {
    /* Read the entire file */
    char buf[16384];
    uint32_t size = 0;

    if (libc_vfs_read(path, buf, sizeof(buf) - 1, &size) < 0)
        return -1;

    if (size == 0) return 0;

    buf[size] = '\0';

    /* Parse line by line */
    int count = 0;
    char *line = buf;
    while (line && *line && crond_num_jobs < CROND_MAX_JOBS) {
        /* Find end of line */
        char *nl = line;
        while (*nl && *nl != '\n') nl++;

        /* Save the newline char and null-terminate */
        char saved = *nl;
        *nl = '\0';

        struct cron_job job;
        if (crond_parse_line(line, &job)) {
            crond_jobs[crond_num_jobs++] = job;
            count++;
        }

        /* Restore */
        *nl = saved;
        line = nl + 1;
    }

    return count;
}

/*
 * Load all crontabs: /etc/crontab + /var/spool/cron/ (per-user crontabs)
 * Returns total number of jobs loaded.
 */
static int crond_load_all(void) {
    /* Reset job table */
    for (int i = 0; i < CROND_MAX_JOBS; i++) {
        crond_jobs[i].active = 0;
    }
    crond_num_jobs = 0;

    int total = 0;

    /* Load system crontab */
    int n = crond_load_file(CROND_SYSTEM_CRONTAB);
    if (n > 0) {
        total += n;
        kprintf("[crond] Loaded %d job(s) from %s\n", n, CROND_SYSTEM_CRONTAB);
    }

    /* Load user crontabs from spool directory */
    /* We enumerate files in /var/spool/cron/ */
    if (libc_vfs_stat(CROND_SPOOL_DIR, NULL) == 0) {
        /* Try to list directory contents using the filesystem listing */
        /* We use fs_list_names to enumerate user crontab files */
        char names[16][28]; /* FS_MAX_NAME = 28 */
        int num_entries = libc_fs_list_names(CROND_SPOOL_DIR, "", names, 16);
        if (num_entries > 0) {
            for (int i = 0; i < num_entries && i < 16; i++) {
                char user_crontab[128];
                snprintf(user_crontab, sizeof(user_crontab),
                         "%s/%s", CROND_SPOOL_DIR, names[i]);
                n = crond_load_file(user_crontab);
                if (n > 0) {
                    total += n;
                    kprintf("[crond] Loaded %d job(s) from %s\n", n, user_crontab);
                }
            }
        }
    }

    return total;
}

/* ─── Job execution ─────────────────────────────────────────────────── */

/*
 * Check if the current time fields match a cron job's schedule.
 * Returns 1 if the job should run now.
 */
static int crond_job_matches(const struct cron_job *job,
                              int minute, int hour, int day, int month, int wday) {
    if (!job || !job->active) return 0;
    if (job->is_reboot) return 0; /* Handled separately at startup */

    /* Check each field: -1 means wildcard (always matches) */
    if (job->minute >= 0 && job->minute != minute) return 0;
    if (job->hour   >= 0 && job->hour   != hour)   return 0;
    if (job->day    >= 0 && job->day    != day)    return 0;
    if (job->month  >= 0 && job->month  != month)  return 0;
    if (job->dayofweek >= 0 && job->dayofweek != wday) return 0;

    return 1;
}

/*
 * Execute a cron job by forking and running the command.
 * We fork a child process, then in the child we call libc_shell_exec_cmd().
 * The parent logs the job execution and cleans up.
 */
static void crond_exec_job(const struct cron_job *job, uint64_t now_sec) {
    /* Build a log message */
    char logmsg[CROND_MAX_LINE + 64];
    int loglen = snprintf(logmsg, sizeof(logmsg),
                          "[cron] %llu: user=%s cmd=%s\n",
                          (unsigned long long)now_sec,
                          job->user, job->command);

    /* Write to log file */
    if (loglen > 0) {
        if (loglen >= (int)sizeof(logmsg))
            loglen = (int)sizeof(logmsg) - 1;
        /* Ensure /var/log exists */
        libc_vfs_create("/var/log", 2);
        /* Append to log (we use fd-based write) */
        int fd = libc_syscall(SYS_OPEN, (uint64_t)CROND_LOG_PATH, 2, 0, 0, 0);
        if ((int)fd >= 0) {
            libc_lseek(fd, 0, 2); /* seek to end */
            libc_fd_write(fd, logmsg, (uint32_t)loglen);
            libc_syscall(SYS_CLOSE, fd, 0, 0, 0, 0);
        } else {
            /* Try creating the file if it doesn't exist */
            libc_vfs_create(CROND_LOG_PATH, 1);
            libc_fs_write_file(CROND_LOG_PATH, logmsg, (uint32_t)loglen);
        }
    }

    kprintf("[crond] Running job: %s\n", job->command);

    /* Fork and execute the command in the child */
    int pid = libc_fork();
    if (pid == 0) {
        /* Child process: execute the command */
        /* The command is run via libc_shell_exec_cmd which simulates */
        /* a shell command execution */
        libc_shell_exec_cmd(job->command, "");
        libc_syscall(SYS_EXIT, 0, 0, 0, 0, 0);
        /* NOTREACHED */
    } else if (pid > 0) {
        /* Parent: we don't wait for the child — it runs in background */
        /* Optionally log the PID */
        kprintf("[crond] Started job PID=%d\n", pid);
    } else {
        kprintf("[crond] fork failed for job: %s\n", job->command);
    }
}

/* ── Main daemon loop ──────────────────────────────────────────────── */

static void crond_run_daemon(void) {
    crond_stop_requested = 0;
    crond_running = 1;

    /* Reload all crontabs */
    int total = crond_load_all();
    kprintf("[crond] Cron daemon started with %d job(s)\n", total);

    /* Run @reboot jobs */
    uint64_t now_sec = libc_time_seconds();
    for (int i = 0; i < crond_num_jobs; i++) {
        if (crond_jobs[i].active && crond_jobs[i].is_reboot) {
            crond_jobs[i].reboot_done = 1;
            crond_exec_job(&crond_jobs[i], now_sec);
        }
    }

    /* Track the current minute for rollover detection */
    {
        int y, mo, d, h, mi, wd;
        crond_epoch_to_fields(now_sec, &y, &mo, &d, &h, &mi, &wd);
        crond_last_minute = (uint64_t)(y * 12 * 31 * 24 * 60 +
                                       mo * 31 * 24 * 60 +
                                       d * 24 * 60 +
                                       h * 60 + mi);
    }

    /* Main loop: poll every 100ms, check for minute rollover */
    while (!crond_stop_requested) {
        libc_sleep_ticks(CROND_POLL_INTERVAL);

        /* Get current epoch time */
        now_sec = libc_time_seconds();
        if (now_sec == 0) continue;

        /* Compute current minute counter */
        int y, mo, d, h, mi, wd;
        crond_epoch_to_fields(now_sec, &y, &mo, &d, &h, &mi, &wd);

        uint64_t current_minute = (uint64_t)(y * 12 * 31 * 24 * 60 +
                                              mo * 31 * 24 * 60 +
                                              d * 24 * 60 +
                                              h * 60 + mi);

        /* Check if the minute has rolled over */
        if (current_minute != crond_last_minute) {
            crond_last_minute = current_minute;

            /* Evaluate all jobs */
            for (int i = 0; i < crond_num_jobs; i++) {
                if (!crond_jobs[i].active) continue;
                if (crond_jobs[i].is_reboot) continue; /* already handled */

                if (crond_job_matches(&crond_jobs[i], mi, h, d, mo, wd)) {
                    crond_exec_job(&crond_jobs[i], now_sec);
                }
            }

            /* Log the tick for debugging (optional, commented for less noise) */
            /* kprintf("[crond] Minute tick: %04d-%02d-%02d %02d:%02d\n", y, mo, d, h, mi); */
        }
    }

    crond_running = 0;
    kprintf("[crond] Cron daemon stopped\n");
}

/* ── Status display ────────────────────────────────────────────────── */

static void crond_show_status(void) {
    if (!crond_running) {
        kprintf("crond: not running\n");
        return;
    }

    kprintf("crond: running, %d job(s) loaded\n", crond_num_jobs);

    if (crond_num_jobs > 0) {
        kprintf("  #  USER     SCHEDULE              COMMAND\n");
        kprintf("  -- -------- --------------------- ---------------------------------------\n");
        for (int i = 0; i < crond_num_jobs; i++) {
            if (!crond_jobs[i].active) continue;

            kprintf("  %-2d %-8s ", i, crond_jobs[i].user);

            if (crond_jobs[i].is_reboot) {
                kprintf("%-21s ", "@reboot");
            } else {
                char sched[22];
                int len = snprintf(sched, sizeof(sched), "%s %s %s %s %s",
                    crond_jobs[i].minute >= 0 ? "*/1" : "*",
                    crond_jobs[i].hour >= 0 ? "*/1" : "*",
                    crond_jobs[i].day >= 0 ? "*/1" : "*",
                    crond_jobs[i].month >= 0 ? "*/1" : "*",
                    crond_jobs[i].dayofweek >= 0 ? "*/1" : "*");
                if (len < 0 || len >= (int)sizeof(sched)) len = (int)sizeof(sched) - 1;
                /* Actually show the values properly */
                snprintf(sched, sizeof(sched), "%d %d %d %d %d",
                    crond_jobs[i].minute,
                    crond_jobs[i].hour,
                    crond_jobs[i].day,
                    crond_jobs[i].month,
                    crond_jobs[i].dayofweek);
                /* Replace -1 with * in display */
                for (int j = 0; sched[j]; j++) {
                    if (j > 0 && sched[j-1] == ' ' && sched[j] == '-') {
                        /* Skip -1 and replace with * */
                    }
                }
                kprintf("%-21s ", sched);
            }

            kprintf("%s\n", crond_jobs[i].command);
        }
    }
}

/* ── Command entry point ───────────────────────────────────────────── */

void cmd_crond(const char *args) {
    /* Parse arguments */
    const char *arg = args;
    while (arg && *arg == ' ') arg++;

    if (arg && *arg) {
        if (strcmp(arg, "stop") == 0) {
            if (!crond_running) {
                kprintf("crond: not running\n");
                return;
            }
            crond_stop_requested = 1;
            kprintf("crond: stop requested\n");
            return;
        }

        if (strcmp(arg, "status") == 0) {
            crond_show_status();
            return;
        }

        if (strcmp(arg, "reload") == 0) {
            if (!crond_running) {
                kprintf("crond: not running, use 'crond' to start\n");
                return;
            }
            kprintf("[crond] Reloading crontab files...\n");
            crond_load_all();
            kprintf("[crond] Reload complete: %d job(s)\n", crond_num_jobs);
            return;
        }

        /* Unknown argument */
        kprintf("crond: unknown option '%s'\n", arg);
        kprintf("Usage: crond [stop|status|reload]\n");
        return;
    }

    /* No args — run the daemon */
    crond_run_daemon();
}
