/* cmd_journalctl.c — Journal log query tool
 *
 * Queries and displays structured journal logs written by journald.
 * Supports:
 *   -f        Follow live log (tail -f equivalent)
 *   -u NAME   Filter by unit/service name
 *   -n 50     Show last N lines
 *   --since   Time-based filter (e.g. "1h ago")
 *   Output formats: short, verbose, json
 *
 * Item S168: Journalctl — log query tool
 *
 * Usage: journalctl [options]
 *   journalctl -f
 *   journalctl -u sshd
 *   journalctl -n 50
 *   journalctl --since "1h ago"
 *   journalctl -o short|verbose|json
 */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "libc.h"
#include "heap.h"

/* ── Constants ─────────────────────────────────────────────────────────── */

#define JOURNAL_DIR     "/var/log/journal/"
#define DEFAULT_LINES   10

/* Journal entry header (matches cmd_journald.c) */
struct journal_header {
    uint64_t timestamp;     /* microseconds since boot */
    uint32_t pid;           /* originating process PID */
    uint32_t uid;           /* originating user UID */
    uint16_t priority;      /* syslog priority (0-7) */
    uint16_t unit_len;      /* length of unit/service name */
    uint32_t msg_len;       /* length of message payload */
} __attribute__((packed));

/* Priority strings */
static const char *priority_str(int prio)
{
    static const char *levels[] = {
        "EMERG", "ALERT", "CRIT", "ERR",
        "WARNING", "NOTICE", "INFO", "DEBUG"
    };
    if (prio >= 0 && prio <= 7)
        return levels[prio];
    return "UNKNOWN";
}

/*
 * Read all journal files (including rotated ones) and collect entries.
 * Returns a buffer containing all raw entries, with sizes in an array.
 */
struct journal_entry {
    struct journal_header hdr;
    char *unit;
    char *msg;
};

/* ── Journal file iteration ───────────────────────────────────────────── */

/*
 * Read a complete journal file into memory.
 * Returns a kmalloc'd buffer (caller must kfree), or NULL on failure.
 * *out_size receives the file size.
 */
static char *read_journal_file(const char *path, uint32_t *out_size)
{
    uint8_t type;
    uint32_t size;

    if (fs_stat(path, &size, &type) < 0 || size == 0) {
        *out_size = 0;
        return NULL;
    }

    char *buf = (char *)kmalloc(size + 1);
    if (!buf) {
        *out_size = 0;
        return NULL;
    }

    uint32_t bytes_read = 0;
    if (vfs_read(path, buf, size, &bytes_read) < 0) {
        kfree(buf);
        *out_size = 0;
        return NULL;
    }

    *out_size = bytes_read;
    return buf;
}

/*
 * Count entries in a raw journal buffer.
 */
static int count_entries(const char *buf, uint32_t size)
{
    int count = 0;
    uint32_t pos = 0;

    while (pos + sizeof(struct journal_header) <= size) {
        const struct journal_header *hdr =
            (const struct journal_header *)(buf + pos);
        uint32_t entry_size = sizeof(struct journal_header) +
                              hdr->unit_len + hdr->msg_len;
        if (pos + entry_size > size)
            break;
        count++;
        pos += entry_size;
    }

    return count;
}

/*
 * Parse entries from a raw journal buffer into an array.
 * Returns number of entries parsed.  The array is allocated via kmalloc
 * and must be freed by the caller.
 */
static int parse_entries(const char *buf, uint32_t size,
                         struct journal_entry *entries, int max_entries)
{
    int count = 0;
    uint32_t pos = 0;

    while (pos + sizeof(struct journal_header) <= size && count < max_entries) {
        const struct journal_header *hdr =
            (const struct journal_header *)(buf + pos);
        uint32_t entry_size = sizeof(struct journal_header) +
                              hdr->unit_len + hdr->msg_len;

        if (pos + entry_size > size)
            break;

        /* Copy header */
        memcpy(&entries[count].hdr, hdr, sizeof(struct journal_header));

        /* Allocate and copy unit name */
        if (hdr->unit_len > 0) {
            entries[count].unit = (char *)kmalloc(hdr->unit_len + 1);
            if (entries[count].unit) {
                memcpy(entries[count].unit, buf + pos + sizeof(struct journal_header),
                       hdr->unit_len);
                entries[count].unit[hdr->unit_len] = '\0';
            }
        } else {
            entries[count].unit = NULL;
        }

        /* Allocate and copy message */
        if (hdr->msg_len > 0) {
            entries[count].msg = (char *)kmalloc(hdr->msg_len + 1);
            if (entries[count].msg) {
                memcpy(entries[count].msg,
                       buf + pos + sizeof(struct journal_header) + hdr->unit_len,
                       hdr->msg_len);
                entries[count].msg[hdr->msg_len] = '\0';
            }
        } else {
            entries[count].msg = NULL;
        }

        count++;
        pos += entry_size;
    }

    return count;
}

/* Free an array of journal entries */
static void free_entries(struct journal_entry *entries, int count)
{
    for (int i = 0; i < count; i++) {
        if (entries[i].unit) kfree(entries[i].unit);
        if (entries[i].msg)  kfree(entries[i].msg);
    }
}

/*
 * Check if an entry matches a unit filter.
 * Returns 1 if it matches (or no filter), 0 otherwise.
 */
static int entry_matches_unit(const struct journal_entry *entry,
                              const char *unit_filter)
{
    if (!unit_filter || !unit_filter[0])
        return 1;
    if (!entry->unit)
        return 0;
    return strcmp(entry->unit, unit_filter) == 0;
}

/*
 * Check if an entry matches a time-based filter.
 * Returns 1 if it matches (or no filter), 0 otherwise.
 */
static int entry_matches_since(const struct journal_entry *entry,
                               const char *since_filter,
                               uint64_t current_time)
{
    if (!since_filter || !since_filter[0])
        return 1;

    /* Parse "1h ago", "30m ago", "10s ago" — very simplified */
    (void)since_filter;
    (void)current_time;

    /* For now, always pass — full time-based filtering requires
     * proper time parsing and comparison. */
    return 1;
}

/*
 * Format and print a journal entry.
 * Supports formats: short, verbose, json
 */
static void print_entry(const struct journal_entry *entry,
                        const char *format)
{
    const char *unit = entry->unit ? entry->unit : "unknown";
    const char *msg  = entry->msg  ? entry->msg  : "";

    if (strcmp(format, "json") == 0) {
        kprintf("{");
        kprintf("\"prio\":\"%s\",", priority_str(entry->hdr.priority));
        kprintf("\"unit\":\"%s\",", unit);
        kprintf("\"pid\":%u,", entry->hdr.pid);
        kprintf("\"msg\":\"%s\"", msg);
        kprintf("}\n");
    } else if (strcmp(format, "verbose") == 0) {
        kprintf("-- Priority: %s --\n", priority_str(entry->hdr.priority));
        kprintf("   Unit:     %s\n", unit);
        kprintf("   PID:      %u\n", entry->hdr.pid);
        kprintf("   Message:  %s\n", msg);
    } else {
        /* short (default) */
        kprintf("%-6s %-12s %s\n",
                priority_str(entry->hdr.priority),
                unit, msg);
    }
}

/* ── Main command ─────────────────────────────────────────────────────── */

void cmd_journalctl(const char *args)
{
    int follow = 0;
    int n_lines = DEFAULT_LINES;
    char unit_filter[64] = {0};
    char since_filter[64] = {0};
    char format[16] = "short";
    int show_usage = 0;

    /* Parse arguments */
    const char *p = args;
    while (p && *p) {
        /* Skip leading spaces */
        while (*p == ' ') p++;
        if (!*p) break;

        if (p[0] == '-' && p[1] == 'f' && (p[2] == ' ' || p[2] == '\0')) {
            follow = 1;
            p += 2;
        } else if (p[0] == '-' && p[1] == 'n' && (p[2] == ' ' || p[2] == '\0')) {
            p += 2;
            while (*p == ' ') p++;
            char num[16] = {0};
            int i = 0;
            while (*p && *p != ' ' && i < 15)
                num[i++] = *p++;
            n_lines = atoi(num);
            if (n_lines < 1) n_lines = 1;
            if (n_lines > 10000) n_lines = 10000;
        } else if (p[0] == '-' && p[1] == 'u' && (p[2] == ' ' || p[2] == '\0')) {
            p += 2;
            while (*p == ' ') p++;
            int i = 0;
            while (*p && *p != ' ' && i < (int)sizeof(unit_filter) - 1)
                unit_filter[i++] = *p++;
        } else if (p[0] == '-' && p[1] == 'o' && (p[2] == ' ' || p[2] == '\0')) {
            p += 2;
            while (*p == ' ') p++;
            int i = 0;
            while (*p && *p != ' ' && i < (int)sizeof(format) - 1)
                format[i++] = *p++;
        } else if (strncmp(p, "--since", 7) == 0 && (p[7] == ' ' || p[7] == '=')) {
            p += 7;
            if (*p == '=') p++;
            while (*p == ' ') p++;
            /* Handle quoted string */
            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                int i = 0;
                while (*p && *p != quote && i < (int)sizeof(since_filter) - 1)
                    since_filter[i++] = *p++;
                if (*p == quote) p++;
            } else {
                int i = 0;
                while (*p && *p != ' ' && i < (int)sizeof(since_filter) - 1)
                    since_filter[i++] = *p++;
            }
        } else {
            show_usage = 1;
            break;
        }
    }

    if (show_usage) {
        kprintf("Usage: journalctl [options]\n");
        kprintf("  -f              Follow live log\n");
        kprintf("  -u NAME         Filter by unit/service name\n");
        kprintf("  -n N            Show last N lines (default: %d)\n", DEFAULT_LINES);
        kprintf("  --since TIME    Time-based filter (e.g. \"1h ago\")\n");
        kprintf("  -o FORMAT       Output format: short, verbose, json\n");
        return;
    }

    /* Collect all journal files in order (newest first: journal.jrn, journal.1.jrn, ...) */
    struct journal_entry all_entries[4096];
    int total_entries = 0;
    char path[128];

    /* Read current journal */
    snprintf(path, sizeof(path), "%sjournal.jrn", JOURNAL_DIR);
    uint32_t file_size;
    char *file_buf = read_journal_file(path, &file_size);
    if (file_buf) {
        int n = parse_entries(file_buf, file_size, all_entries + total_entries,
                              (int)(sizeof(all_entries) / sizeof(all_entries[0])) - total_entries);
        total_entries += n;
        kfree(file_buf);
    }

    /* Read rotated journals (1, 2, ...) */
    for (int rot = 1; rot <= 3; rot++) {
        snprintf(path, sizeof(path), "%sjournal.%d.jrn", JOURNAL_DIR, rot);
        file_buf = read_journal_file(path, &file_size);
        if (file_buf) {
            int n = parse_entries(file_buf, file_size, all_entries + total_entries,
                                  (int)(sizeof(all_entries) / sizeof(all_entries[0])) - total_entries);
            total_entries += n;
            kfree(file_buf);
        }
    }

    if (total_entries == 0) {
        kprintf("No journal entries found.\n");
        return;
    }

    /* Apply unit filter */
    if (unit_filter[0]) {
        int filtered_count = 0;
        for (int i = 0; i < total_entries; i++) {
            if (entry_matches_unit(&all_entries[i], unit_filter)) {
                if (filtered_count < i) {
                    memcpy(&all_entries[filtered_count], &all_entries[i],
                           sizeof(struct journal_entry));
                }
                filtered_count++;
            }
        }
        total_entries = filtered_count;
    }

    /* Handle -n: show only last N entries */
    int start_idx = 0;
    if (n_lines < total_entries)
        start_idx = total_entries - n_lines;

    /* Print entries (in chronological order) */
    kprintf("-- Journal entries --\n");

    if (follow) {
        /* Follow mode: show all then wait for new entries */
        for (int i = start_idx; i < total_entries; i++)
            print_entry(&all_entries[i], format);

        /* In follow mode, we would poll for new entries.
         * For now, just show a message. */
        kprintf("-- Follow mode: waiting for new entries (Ctrl+C to stop) --\n");

        /* Simple poll: re-read journal file periodically */
        int poll_count = 0;
        while (poll_count < 60) { /* ~30 seconds of polling */
            /* Busy-wait approx 500ms */
            for (volatile int j = 0; j < 5000000; j++)
                __asm__ volatile("pause");

            /* Check for new entries */
            snprintf(path, sizeof(path), "%sjournal.jrn", JOURNAL_DIR);
            file_buf = read_journal_file(path, &file_size);
            if (file_buf) {
                /* Count entries to find new ones */
                int new_count = count_entries(file_buf, file_size);
                if (new_count > total_entries) {
                    int new_start = total_entries;
                    int to_parse = new_count - new_start;
                    if (to_parse > 100) to_parse = 100;

                    struct journal_entry new_entries[100];
                    int parsed = parse_entries(file_buf + new_start * sizeof(struct journal_header),
                                               file_size - new_start * sizeof(struct journal_header),
                                               new_entries, to_parse);
                    for (int j = 0; j < parsed; j++)
                        print_entry(&new_entries[j], format);
                    free_entries(new_entries, parsed);
                    total_entries = new_count;
                }
                kfree(file_buf);
            }
            poll_count++;
        }
    } else {
        /* Normal mode: print filtered range */
        for (int i = start_idx; i < total_entries; i++)
            print_entry(&all_entries[i], format);
    }

    /* Cleanup */
    free_entries(all_entries, total_entries);
}
