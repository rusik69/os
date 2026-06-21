/*
 * sysctl.c — /proc/sys/kernel/ interface
 *
 * Provides kernel tunables via filesystem-like entries under /proc/sys/kernel/.
 */

#include "sysctl.h"
#include "string.h"
#include "printf.h"
#include "types.h"
#include "process.h"
#include "sysrq.h"

/* ─── Default watermark values ───────────────────────────────────── */
/* Memory reclaim watermark — minimum free pages before reclaim triggers.
 * We maintain our own copy rather than depending on pmm_extras.c. */
static uint64_t sysctl_reclaim_watermark = 64;

/* ─── Helper: convert uint64_t to string ─────────────────────────-- */

static void ul_to_str(uint64_t v, char *buf, int *pos, int max)
{
    if (v == 0) {
        if (*pos < max - 1) buf[(*pos)++] = '0';
        return;
    }
    char tmp[24];
    int n = 0;
    while (v > 0) {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    while (n-- > 0 && *pos < max - 1)
        buf[(*pos)++] = tmp[n];
}

/* ─── Static sysctl table ────────────────────────────────────────── */

#define SYSCTL_MAX_ENTRIES 32

static struct sysctl_entry g_entries[SYSCTL_MAX_ENTRIES];
static int g_num_entries = 0;

/* ─── Default sysctl handlers ────────────────────────────────────── */

/* hostname */
static char g_hostname[65] = "os";

static int sysctl_read_hostname(char *buf, int max) {
    int l = (int)strlen(g_hostname);
    if (l >= max) l = max - 1;
    memcpy(buf, g_hostname, (size_t)l);
    buf[l] = '\n';
    return l + 1;
}

static int sysctl_write_hostname(const char *buf, int len) {
    int clen = len < 64 ? len : 64;
    memcpy(g_hostname, buf, (size_t)clen);
    g_hostname[clen] = '\0';
    /* Trim trailing newline if present */
    while (clen > 0 && (g_hostname[clen-1] == '\n' || g_hostname[clen-1] == '\r'))
        g_hostname[--clen] = '\0';
    return 0;
}

const char *sysctl_get_hostname(void) {
    return g_hostname;
}

/* Set the kernel hostname from a NUL-terminated string.
 * Copies up to 64 characters; trims trailing newlines/whitespace. */
void sysctl_set_hostname(const char *name) {
    if (!name) return;
    size_t len = strlen(name);
    if (len > 64) len = 64;
    memcpy(g_hostname, name, len);
    g_hostname[len] = '\0';
    /* Trim trailing whitespace/newlines */
    while (len > 0 && (g_hostname[len-1] == '\n' || g_hostname[len-1] == '\r' || g_hostname[len-1] == ' '))
        g_hostname[--len] = '\0';
}

/* osrelease */
static const char *g_osrelease = "6.1.0-os";

static int sysctl_read_osrelease(char *buf, int max) {
    int l = (int)strlen(g_osrelease);
    if (l >= max) l = max - 1;
    memcpy(buf, g_osrelease, (size_t)l);
    buf[l] = '\n';
    return l + 1;
}

/* ostype */
static const char *g_ostype = "Linux";

static int sysctl_read_ostype(char *buf, int max) {
    int l = (int)strlen(g_ostype);
    if (l >= max) l = max - 1;
    memcpy(buf, g_ostype, (size_t)l);
    buf[l] = '\n';
    return l + 1;
}

/* panic timeout */
static int g_panic_timeout = 0;

static int sysctl_read_panic(char *buf, int max) {
    int p = 0;
    char tmp[16]; int ti = 0;
    int v = g_panic_timeout;
    if (v == 0) { tmp[ti++] = '0'; }
    else { while (v) { tmp[ti++] = (char)('0' + (int)(v % 10)); v /= 10; } }
    for (int i = ti - 1; i >= 0 && p < max - 1; i--) buf[p++] = tmp[i];
    if (p < max - 1) buf[p++] = '\n';
    buf[p] = '\0';
    return p;
}

static int sysctl_write_panic(const char *buf, int len) {
    int v = 0;
    for (int i = 0; i < len && buf[i] >= '0' && buf[i] <= '9'; i++)
        v = v * 10 + (buf[i] - '0');
    g_panic_timeout = v;
    return 0;
}

/* randomize_va_space */
static int g_randomize_va_space = 2;

static int sysctl_read_rand_va(char *buf, int max) {
    if (max < 3) return 0;
    buf[0] = '0' + (char)g_randomize_va_space;
    buf[1] = '\n';
    buf[2] = '\0';
    return 2;
}

static int sysctl_write_rand_va(const char *buf, int len) {
    if (len > 0 && buf[0] >= '0' && buf[0] <= '2')
        g_randomize_va_space = buf[0] - '0';
    return 0;
}

/* vm.reclaim_watermark — minimum free pages before reclaim triggers */
static int sysctl_read_reclaim_watermark(char *buf, int max)
{
    int p = 0;
    ul_to_str(sysctl_reclaim_watermark, buf, &p, max);
    if (p < max - 1) buf[p++] = '\n';
    if (p < max) buf[p] = '\0';
    return p;
}

static int sysctl_write_reclaim_watermark(const char *buf, int len)
{
    uint64_t v = 0;
    for (int i = 0; i < len && buf[i] >= '0' && buf[i] <= '9'; i++)
        v = v * 10 + (uint64_t)(buf[i] - '0');
    sysctl_reclaim_watermark = v;
    return 0;
}

/* ── Time namespace offset write handlers ──────────────────────────────
 * These allow setting the per-process time namespace offsets via sysctl:
 *
 *   echo +3600000000000 > /proc/sys/kernel/timens_mono_offset
 *   echo -5000000000    > /proc/sys/kernel/timens_boottime_offset
 *
 * Only affects the current process (and its future children via fork
 * inheritance).  Only effective when the process has CLONE_NEWTIME set
 * via unshare(CLONE_NEWTIME).
 */
static int sysctl_write_timens_mono_offset(const char *buf, int len)
{
    struct process *cur = process_get_current();
    if (!cur) return -1;
    if (!(cur->ns_flags & CLONE_NEWTIME))
        return -1; /* Not in a time namespace — cannot set offsets */

    int64_t val = 0;
    int sign = 1, i = 0;
    if (i < len && buf[i] == '-') { sign = -1; i++; }
    else if (i < len && buf[i] == '+') { i++; }
    for (; i < len && buf[i] >= '0' && buf[i] <= '9'; i++)
        val = val * 10 + (int64_t)(buf[i] - '0');
    cur->timens_mono_offset = val * sign;
    return 0;
}

static int sysctl_write_timens_boottime_offset(const char *buf, int len)
{
    struct process *cur = process_get_current();
    if (!cur) return -1;
    if (!(cur->ns_flags & CLONE_NEWTIME))
        return -1; /* Not in a time namespace */

    int64_t val = 0;
    int sign = 1, i = 0;
    if (i < len && buf[i] == '-') { sign = -1; i++; }
    else if (i < len && buf[i] == '+') { i++; }
    for (; i < len && buf[i] >= '0' && buf[i] <= '9'; i++)
        val = val * 10 + (int64_t)(buf[i] - '0');
    cur->timens_boottime_offset = val * sign;
    return 0;
}

/* ── SysRq enable mask ───────────────────────────────────────────── */

static int sysctl_read_sysrq(char *buf, int max)
{
    int v = sysrq_get_mask();
    int p = 0;
    if (v < 0) {
        if (p < max - 1) buf[p++] = '-';
        v = -v;
    }
    char tmp[16]; int ti = 0;
    if (v == 0) { tmp[ti++] = '0'; }
    else { while (v) { tmp[ti++] = (char)('0' + (int)(v % 10)); v /= 10; } }
    for (int i = ti - 1; i >= 0 && p < max - 1; i--) buf[p++] = tmp[i];
    if (p < max - 1) buf[p++] = '\n';
    buf[p] = '\0';
    return p;
}

static int sysctl_write_sysrq(const char *buf, int len)
{
    int v = 0, sign = 1, i = 0;
    if (i < len && buf[i] == '-') { sign = -1; i++; }
    for (; i < len && buf[i] >= '0' && buf[i] <= '9'; i++)
        v = v * 10 + (buf[i] - '0');
    sysrq_set_mask(v * sign);
    return 0;
}

/* ─── Public API ─────────────────────────────────────────────────── */

int sysctl_register(const char *name,
                    int (*read_handler)(char *buf, int max),
                    int (*write_handler)(const char *buf, int len)) {
    if (g_num_entries >= SYSCTL_MAX_ENTRIES) return -1;
    g_entries[g_num_entries].name = name;
    g_entries[g_num_entries].read_handler = read_handler;
    g_entries[g_num_entries].write_handler = write_handler;
    g_entries[g_num_entries].data = NULL;
    g_entries[g_num_entries].maxlen = 0;
    g_entries[g_num_entries].mode = 0644;
    g_num_entries++;
    return 0;
}

int sysctl_read(const char *name, char *buf, int max) {
    if (!name || !buf) return -1;
    for (int i = 0; i < g_num_entries; i++) {
        if (strcmp(g_entries[i].name, name) == 0) {
            if (g_entries[i].read_handler)
                return g_entries[i].read_handler(buf, max);
            return 0;
        }
    }
    /* Default: try to read kernel tunables by name */
    if (strcmp(name, "hostname") == 0)
        return sysctl_read_hostname(buf, max);
    if (strcmp(name, "osrelease") == 0)
        return sysctl_read_osrelease(buf, max);
    if (strcmp(name, "ostype") == 0)
        return sysctl_read_ostype(buf, max);
    if (strcmp(name, "panic") == 0)
        return sysctl_read_panic(buf, max);
    if (strcmp(name, "randomize_va_space") == 0)
        return sysctl_read_rand_va(buf, max);
    if (strcmp(name, "vm.reclaim_watermark") == 0)
        return sysctl_read_reclaim_watermark(buf, max);
    return -1;
}

int sysctl_write(const char *name, const char *buf, int len) {
    if (!name || !buf) return -1;
    for (int i = 0; i < g_num_entries; i++) {
        if (strcmp(g_entries[i].name, name) == 0) {
            if (g_entries[i].write_handler)
                return g_entries[i].write_handler(buf, len);
            return 0;
        }
    }
    /* Default handlers */
    if (strcmp(name, "hostname") == 0)
        return sysctl_write_hostname(buf, len);
    if (strcmp(name, "panic") == 0)
        return sysctl_write_panic(buf, len);
    if (strcmp(name, "randomize_va_space") == 0)
        return sysctl_write_rand_va(buf, len);
    if (strcmp(name, "vm.reclaim_watermark") == 0)
        return sysctl_write_reclaim_watermark(buf, len);
    if (strcmp(name, "timens_mono_offset") == 0)
        return sysctl_write_timens_mono_offset(buf, len);
    if (strcmp(name, "timens_boottime_offset") == 0)
        return sysctl_write_timens_boottime_offset(buf, len);
    if (strcmp(name, "sysrq") == 0)
        return sysctl_write_sysrq(buf, len);
    return -1;
}

/* ─── List registered sysctl entries ──────────────────────────────── */

int sysctl_list_names(char names[][48], int max_names)
{
    int count = 0;

    /* First, collect dynamically registered entries */
    for (int i = 0; i < g_num_entries && count < max_names; i++) {
        int nlen = (int)strlen(g_entries[i].name);
        int copylen = nlen < 47 ? nlen : 47;
        memcpy(names[count], g_entries[i].name, (size_t)copylen);
        names[count][copylen] = '\0';
        count++;
    }

    /* Add built-in entries that aren't already in the table */
    static const char *builtins[] = {
        "hostname", "osrelease", "ostype",
        "panic", "randomize_va_space",
        "vm.reclaim_watermark",
        "sysrq",
        NULL
    };

    for (int b = 0; builtins[b] && count < max_names; b++) {
        /* Check if already registered */
        int found = 0;
        for (int i = 0; i < g_num_entries; i++) {
            if (strcmp(g_entries[i].name, builtins[b]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            int nlen = (int)strlen(builtins[b]);
            int copylen = nlen < 47 ? nlen : 47;
            memcpy(names[count], builtins[b], (size_t)copylen);
            names[count][copylen] = '\0';
            count++;
        }
    }

    return count;
}

/* ─── Initialisation ────────────────────────────────────────────── */

void sysctl_init(void) {
    /* Register built-in sysctl entries dynamically so they appear in listings */
    sysctl_register("hostname", sysctl_read_hostname, sysctl_write_hostname);
    sysctl_register("osrelease", sysctl_read_osrelease, NULL);
    sysctl_register("ostype", sysctl_read_ostype, NULL);
    sysctl_register("panic", sysctl_read_panic, sysctl_write_panic);
    sysctl_register("randomize_va_space", sysctl_read_rand_va, sysctl_write_rand_va);
    sysctl_register("vm.reclaim_watermark", sysctl_read_reclaim_watermark, sysctl_write_reclaim_watermark);
    sysctl_register("sysrq", sysctl_read_sysrq, sysctl_write_sysrq);

    kprintf("[OK] Sysctl interface initialized (%d entries)\n", g_num_entries);
}

/* ── Stub: sysctl_unregister ─────────────────────────────── */
int sysctl_unregister(void *table)
{
    (void)table;
    kprintf("[sysctl] sysctl_unregister: not yet implemented\n");
    return -ENOSYS;
}
