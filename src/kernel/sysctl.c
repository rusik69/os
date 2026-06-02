/*
 * sysctl.c — /proc/sys/kernel/ interface
 *
 * Provides kernel tunables via filesystem-like entries under /proc/sys/kernel/.
 */

#include "sysctl.h"
#include "string.h"
#include "printf.h"
#include "types.h"

/* ─── Static sysctl table ────────────────────────────────────────── */

#define SYSCTL_MAX_ENTRIES 16

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
    else { while (v) { tmp[ti++] = '0' + (v % 10); v /= 10; } }
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
    return -1;
}

void sysctl_init(void) {
    /* Built-in sysctl entries are registered statically */
    kprintf("[OK] Sysctl interface initialized\n");
}
