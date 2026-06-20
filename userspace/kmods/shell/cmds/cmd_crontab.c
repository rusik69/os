/*
 * cmd_crontab.c — crontab: per-user cron job manager (Item 317)
 *
 * Standard crontab command for installing, listing, editing, and removing
 * per-user crontab entries.  Reads from /var/spool/cron/<username> which
 * the crond daemon (cmd_crond.c, Item U10) loads and executes.
 *
 * Usage:
 *   crontab -l            List the current user's crontab
 *   crontab -e            Edit the current user's crontab (opens editor)
 *   crontab -r            Remove the current user's crontab
 *   crontab <file>        Install crontab from file (replaces existing)
 *   crontab <file>        Install crontab from path (alias)
 *
 * Crontab format (standard POSIX):
 *   # comment
 *   min hour day month dayofweek command
 *   @reboot command
 *   @daily  command
 *   @hourly command
 *
 * The file is stored at /var/spool/cron/<username> and is read by crond.
 */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "syscall.h"

/* ── Paths ──────────────────────────────────────────────────────────── */

/* Spool directory for per-user crontabs (same as in cmd_crond.c) */
#define CRONTAB_SPOOL_DIR   "/var/spool/cron"
#define CRONTAB_PREFIX_LEN  33  /* "/var/spool/cron/" + max username (32) + NUL */

/* Maximum size of a crontab file we can handle */
#define CRONTAB_MAX_SIZE    16384

/* ── Forward declarations ───────────────────────────────────────────── */

/* Get the current user's crontab file path into @buf (size must be >= PREFIX_LEN) */
static void crontab_user_path(char *buf, int bufsz) {
    struct libc_user_session *s = libc_session_get();
    if (!s || !s->logged_in) {
        /* No session — use a fallback */
        snprintf(buf, bufsz, CRONTAB_SPOOL_DIR "/root");
        return;
    }
    snprintf(buf, bufsz, CRONTAB_SPOOL_DIR "/%s", s->username);
}

/* ── crontab -l: list the user's crontab ────────────────────────────── */

static void crontab_list(void) {
    char path[CRONTAB_PREFIX_LEN];
    crontab_user_path(path, sizeof(path));

    /* Try to read the crontab file */
    char buf[CRONTAB_MAX_SIZE];
    uint32_t size = 0;

    if (libc_vfs_read(path, buf, sizeof(buf) - 1, &size) < 0 || size == 0) {
        kprintf("crontab: no crontab for current user\n");
        return;
    }

    buf[size] = '\0';
    kprintf("%s", buf);
}

/* ── crontab -r: remove the user's crontab ──────────────────────────── */

static void crontab_remove(void) {
    char path[CRONTAB_PREFIX_LEN];
    crontab_user_path(path, sizeof(path));

    if (libc_vfs_unlink(path) < 0) {
        kprintf("crontab: no crontab to remove\n");
        return;
    }

    kprintf("crontab: crontab removed\n");
}

/* ── crontab -e: edit the user's crontab ────────────────────────────── */

static void crontab_edit(void) {
    char path[CRONTAB_PREFIX_LEN];
    crontab_user_path(path, sizeof(path));

    /* Ensure the spool directory exists */
    libc_vfs_create(CRONTAB_SPOOL_DIR, 2); /* 2 = directory */

    /* If the crontab doesn't exist yet, create an empty one with a header */
    uint32_t tmp_size = 0;
    char tmp_buf[64];
    if (libc_vfs_read(path, tmp_buf, sizeof(tmp_buf), &tmp_size) < 0 || tmp_size == 0) {
        /* Write a header template */
        const char *header =
            "# Edit your crontab below.\n"
            "# Format: min hour day month dayofweek command\n"
            "# Fields: 0-59 0-23 1-31 1-12 0-7 (0/7=Sunday)\n"
            "# Special: @reboot, @daily, @hourly, @weekly, @monthly, @yearly\n"
            "# Example:\n"
            "# 0 5 * * * /bin/echo Good morning\n"
            "# @daily /bin/date > /tmp/daily_date\n";
        libc_vfs_write(path, header, (uint32_t)(strlen(header) + 1));
    }

    /* Read the current crontab into a buffer */
    char buf[CRONTAB_MAX_SIZE];
    uint32_t size = 0;
    if (libc_vfs_read(path, buf, sizeof(buf) - 1, &size) < 0) {
        kprintf("crontab: failed to read crontab\n");
        return;
    }
    buf[size] = '\0';

    /* Display current crontab content and prompt for replacement via stdin */
    kprintf("Current crontab:\n");
    kprintf("────────────────────────────────────────\n");
    kprintf("%s", buf);
    kprintf("────────────────────────────────────────\n");
    kprintf("Enter new crontab content (end with Ctrl+D on empty line, or . on empty line):\n");

    /* Read new content line by line from stdin */
    char newbuf[CRONTAB_MAX_SIZE];
    int pos = 0;
    char line[256];

    while (pos < CRONTAB_MAX_SIZE - 1) {
        /* Read a line from stdin */
        int li = 0;
        while (li < 254) {
            char c;
            /* Raw syscall read from stdin (fd=0) */
            int64_t n = (int64_t)libc_syscall(SYS_READ, 0, (uint64_t)(uintptr_t)&c, 1, 0, 0);
            if (n <= 0) break; /* EOF or error */
            if (c == '\n') { line[li] = '\0'; li++; break; }
            line[li++] = c;
        }
        line[li] = '\0';

        /* Check for end markers */
        if (li == 0 || (li == 1 && line[0] == '.')) break;

        /* Copy to new buffer */
        int line_len = (int)strlen(line);
        if (pos + line_len + 2 >= CRONTAB_MAX_SIZE) {
            kprintf("crontab: content too large, truncating\n");
            break;
        }
        memcpy(newbuf + pos, line, (uint32_t)line_len);
        pos += line_len;
        newbuf[pos++] = '\n';
    }

    if (pos > 0) {
        /* Write the new crontab */
        if (libc_vfs_write(path, newbuf, (uint32_t)pos) < 0) {
            kprintf("crontab: failed to write crontab\n");
            return;
        }
        kprintf("crontab: installed new crontab (%d bytes)\n", pos);
    } else {
        /* Empty input — remove the crontab */
        libc_vfs_unlink(path);
        kprintf("crontab: empty input, crontab removed\n");
    }
}

/* ── crontab <file>: install crontab from a file ────────────────────── */

static void crontab_install(const char *filepath) {
    if (!filepath || !*filepath) {
        kprintf("crontab: missing filename\n");
        return;
    }

    /* Read the source file */
    char src_buf[CRONTAB_MAX_SIZE];
    uint32_t src_size = 0;

    if (libc_vfs_read(filepath, src_buf, sizeof(src_buf) - 1, &src_size) < 0) {
        /* Try fs_read_file as fallback */
        if (libc_fs_read_file(filepath, src_buf, sizeof(src_buf) - 1, &src_size) < 0) {
            kprintf("crontab: cannot open '%s'\n", filepath);
            return;
        }
    }

    if (src_size == 0) {
        kprintf("crontab: empty file '%s'\n", filepath);
        return;
    }

    src_buf[src_size] = '\0';

    /* Basic validation: check that the file parses (simple sanity check) */
    /* We do a quick scan for valid-looking entries or comments */
    int has_content = 0;
    const char *scan = src_buf;
    while (*scan) {
        /* Skip whitespace */
        while (*scan == ' ' || *scan == '\t') scan++;
        /* Skip comments and blank lines */
        if (*scan == '#' || *scan == '\n' || *scan == '\r') {
            while (*scan && *scan != '\n') scan++;
            if (*scan == '\n') scan++;
            continue;
        }
        if (*scan == '\0') break;
        /* We found a non-empty, non-comment line — content exists */
        has_content = 1;
        break;
    }

    /* Determine destination path */
    char dst_path[CRONTAB_PREFIX_LEN];
    crontab_user_path(dst_path, sizeof(dst_path));

    /* Ensure spool directory exists */
    libc_vfs_create(CRONTAB_SPOOL_DIR, 2);

    /* Write the file to the user's crontab path */
    if (libc_vfs_write(dst_path, src_buf, src_size) < 0) {
        /* Fallback to fs_write_file */
        if (libc_fs_write_file(dst_path, src_buf, src_size) < 0) {
            kprintf("crontab: failed to install crontab\n");
            return;
        }
    }

    kprintf("crontab: installed crontab from '%s' (%u bytes)%s\n",
            filepath, src_size,
            has_content ? "" : " (empty)");
}

/* ── Command entry point ────────────────────────────────────────────── */

void cmd_crontab(const char *args) {
    if (!args || !*args) {
        /* No arguments — show usage */
        kprintf("Usage: crontab [-l|-e|-r] [file]\n");
        kprintf("  -l    List current crontab\n");
        kprintf("  -e    Edit current crontab\n");
        kprintf("  -r    Remove current crontab\n");
        kprintf("  file  Install crontab from file\n");
        return;
    }

    /* Skip leading whitespace */
    while (*args == ' ') args++;
    if (!*args) {
        kprintf("Usage: crontab [-l|-e|-r] [file]\n");
        return;
    }

    /* Parse flags */
    if (args[0] == '-') {
        if (args[1] == 'l' && (args[2] == '\0' || args[2] == ' ')) {
            crontab_list();
            return;
        }
        if (args[1] == 'e' && (args[2] == '\0' || args[2] == ' ')) {
            crontab_edit();
            return;
        }
        if (args[1] == 'r' && (args[2] == '\0' || args[2] == ' ')) {
            crontab_remove();
            return;
        }
        kprintf("crontab: unknown option '%s'\n", args);
        return;
    }

    /* Install from file */
    crontab_install(args);
}
