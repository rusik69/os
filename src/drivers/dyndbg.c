#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "debugfs.h"
#include "dynamic_debug.h"
#include "errno.h"

/*
 * dyndbg.c — Dynamic debug control file interface
 *
 * Provides the /sys/kernel/debug/dynamic_debug/control debugfs file
 * that userspace can write to enable/disable pr_debug()-style messages
 * at run time.  Supports matching by module name, source file, or
 * function name (Item 211).
 *
 * Command syntax (one per line or echo'd in batch):
 *   all +p              — enable all debug sites
 *   all -p              — disable all debug sites
 *   module <name> +p    — enable all sites in module <name>
 *   module <name> -p    — disable all sites in module <name>
 *   file <path> +p      — enable all sites in source file <path>
 *   file <path> -p      — disable all sites in source file <path>
 *   func <name> +p      — enable all sites in function <name>
 *   func <name> -p      — disable all sites in function <name>
 *
 * Match-type constants (mirroring dynamic_debug.h):
 *   0 = func, 1 = file, 2 = module
 */

#define MATCH_FUNC    0
#define MATCH_FILE    1
#define MATCH_MODULE  2

#define MAX_DEBUG_SITES 64

/*
 * The dyndbg site table is a separate, additional registry used for
 * sites that do not (yet) register through dynamic_debug_register().
 * In the long run everything should migrate to struct
 * dynamic_debug_descriptor, but this table preserves backward
 * compatibility and provides a convenience API.
 */
static struct {
    const char *name;       /* site identifier (function name) */
    int enabled;
} debug_sites[MAX_DEBUG_SITES];
static int debug_count = 0;
static int dyndbg_initialized = 0;
static int dyndbg_all_enabled = 0;
static int dyndbg_all_funcs = 0;  /* function-level all flag */

/* ── Forward declarations ──────────────────────────────────────────── */
static void dyndbg_control_file_read(char *buf, int *len);
static int  dyndbg_control_file_write(const char *buf, int len);

void __init dyndbg_init(void) {
    if (dyndbg_initialized) return;
    memset(debug_sites, 0, sizeof(debug_sites));
    dyndbg_initialized = 1;

    /* Create the debugfs control file at /sys/kernel/debug/dynamic_debug/control */
    int ret = debugfs_create_rw_file("dynamic_debug/control",
                                     dyndbg_control_file_read,
                                     dyndbg_control_file_write);
    if (ret == 0) {
        kprintf("[DYNDBG] control file at /sys/kernel/debug/dynamic_debug/control\n");
    }

    kprintf("[OK] Dynamic debug initialized\n");
}

int dyndbg_register(const char *name) {
    if (debug_count >= MAX_DEBUG_SITES) return -1;
    debug_sites[debug_count].name = name;
    debug_sites[debug_count].enabled = 1;
    return debug_count++;
}

void dyndbg_enable(const char *name) {
    for (int i = 0; i < debug_count; i++)
        if (strcmp(debug_sites[i].name, name) == 0) debug_sites[i].enabled = 1;
}

void dyndbg_disable(const char *name) {
    for (int i = 0; i < debug_count; i++)
        if (strcmp(debug_sites[i].name, name) == 0) debug_sites[i].enabled = 0;
}

int dyndbg_enabled(const char *name) {
    if (dyndbg_all_enabled || dyndbg_all_funcs) return 1;
    for (int i = 0; i < debug_count; i++)
        if (debug_sites[i].enabled && strcmp(debug_sites[i].name, name) == 0) return 1;
    return 0;
}

/* ── Debugfs control file: read current dynamic debug settings ──────── */
static void dyndbg_control_file_read(char *buf, int *len)
{
    int pos = 0;
    int max = 1024;

    {
        int n = snprintf(buf + pos, (size_t)(max - pos),
                        "# Dynamic debug control\n"
                        "# Syntax: all <+p|-p>\n"
                        "#         module <name> +p|-p\n"
                        "#         file   <name> +p|-p\n"
                        "#         func   <name> +p|-p\n"
                        "\n"
                        "all: %s\n",
                        (dyndbg_all_enabled || dyndbg_all_funcs) ? "+p" : "-p");
        if (n > 0 && pos + n < max) pos += n;
    }

    /* Report legacy dyndbg-registered sites */
    for (int i = 0; i < debug_count; i++) {
        int n = snprintf(buf + pos, (size_t)(max - pos),
                        "site:%s flags:%s (%s)\n",
                        debug_sites[i].name,
                        debug_sites[i].enabled ? "+p" : "-p",
                        debug_sites[i].enabled ? "enabled" : "disabled");
        if (n > 0 && pos + n < max) pos += n;
        if (pos >= max - 64) break;
    }

    if (pos > max) pos = max;
    *len = pos;
}

/* ── Debugfs control file: parse commands and apply changes ─────────── */
/*
 * Parse a single command line and apply it.
 * Supported command formats:
 *   all +p             — enable all debug
 *   all -p             — disable all debug
 *   module <name> +p   — enable debug for module (Item 211)
 *   module <name> -p   — disable debug for module
 *   file   <name> +p   — enable debug for file
 *   file   <name> -p   — disable debug for file
 *   func   <name> +p   — enable debug for function
 *   func   <name> -p   — disable debug for function
 *
 * Returns 0 on success, -EINVAL on parse error.
 */
static int dyndbg_parse_and_apply(const char *line, int line_len)
{
    /* Skip leading whitespace */
    while (line_len > 0 && (*line == ' ' || *line == '\t')) {
        line++;
        line_len--;
    }

    /* Skip empty lines and comments */
    if (line_len == 0 || *line == '#' || *line == '\n')
        return 0;

    char buf[128];
    int blen = line_len < (int)sizeof(buf) - 1 ? line_len : (int)sizeof(buf) - 1;
    memcpy(buf, line, (size_t)blen);
    buf[blen] = '\0';

    /* Tokenize: we need keyword [+-]p */
    /* Format: <keyword> <name> <op>   where op is +p or -p */

    /* Extract first word (keyword) */
    char *p = buf;
    while (*p == ' ') p++;
    if (*p == '\0') return 0;

    char *keyword = p;
    while (*p && *p != ' ') p++;
    int kw_len = (int)(p - keyword);

    /* Skip spaces to get name */
    while (*p == ' ') p++;
    char *name = p;
    while (*p && *p != ' ') p++;
    int name_len = (int)(p - name);

    /* Skip spaces to get op */
    while (*p == ' ') p++;
    char *op = p;
    int op_len = (int)(strlen(op));

    /* Null-terminate the tokens */
    keyword[kw_len] = '\0';
    if (name_len > 0) name[name_len] = '\0';
    if (op_len > 0) { /* op is already null-terminated */ }

    /* Check: all +p|-p */
    if (kw_len == 3 && memcmp(keyword, "all", 3) == 0) {
        if (op_len == 2 && memcmp(op, "+p", 2) == 0) {
            dyndbg_all_enabled = 1;
            dyndbg_all_funcs = 1;
            for (int i = 0; i < debug_count; i++)
                debug_sites[i].enabled = 1;
            dynamic_debug_enable(NULL, 0);
            kprintf("[DYNDBG] all debug enabled\n");
            return 0;
        } else if (op_len == 2 && memcmp(op, "-p", 2) == 0) {
            dyndbg_all_enabled = 0;
            dyndbg_all_funcs = 0;
            for (int i = 0; i < debug_count; i++)
                debug_sites[i].enabled = 0;
            dynamic_debug_disable(NULL, 0);
            kprintf("[DYNDBG] all debug disabled\n");
            return 0;
        }
        return -EINVAL;
    }

    if (name_len == 0 || op_len == 0)
        return -EINVAL;
    name[name_len] = '\0';

    /* Check: module <name> +p|-p — NEW in Item 211 */
    if (kw_len == 6 && memcmp(keyword, "module", 6) == 0) {
        if (op_len == 2 && memcmp(op, "+p", 2) == 0) {
            int n = dynamic_debug_enable(name, MATCH_MODULE);
            kprintf("[DYNDBG] module '%s' enabled (%d site(s))\n", name, n);
            return 0;
        } else if (op_len == 2 && memcmp(op, "-p", 2) == 0) {
            int n = dynamic_debug_disable(name, MATCH_MODULE);
            kprintf("[DYNDBG] module '%s' disabled (%d site(s))\n", name, n);
            return 0;
        }
        return -EINVAL;
    }

    /* Check: func <name> +p|-p */
    if (kw_len == 4 && memcmp(keyword, "func", 4) == 0) {
        if (op_len == 2 && memcmp(op, "+p", 2) == 0) {
            dyndbg_enable(name);
            int n = dynamic_debug_enable(name, MATCH_FUNC);
            kprintf("[DYNDBG] func '%s' enabled (%d site(s))\n", name, n);
            return 0;
        } else if (op_len == 2 && memcmp(op, "-p", 2) == 0) {
            dyndbg_disable(name);
            int n = dynamic_debug_disable(name, MATCH_FUNC);
            kprintf("[DYNDBG] func '%s' disabled (%d site(s))\n", name, n);
            return 0;
        }
        return -EINVAL;
    }

    /* Check: file <name> +p|-p */
    if (kw_len == 4 && memcmp(keyword, "file", 4) == 0) {
        if (op_len == 2 && memcmp(op, "+p", 2) == 0) {
            int n = dynamic_debug_enable(name, MATCH_FILE);
            kprintf("[DYNDBG] file '%s' enabled (%d site(s))\n", name, n);
            return 0;
        } else if (op_len == 2 && memcmp(op, "-p", 2) == 0) {
            int n = dynamic_debug_disable(name, MATCH_FILE);
            kprintf("[DYNDBG] file '%s' disabled (%d site(s))\n", name, n);
            return 0;
        }
        return -EINVAL;
    }

    kprintf("[DYNDBG] unrecognized: '%s'\n", buf);
    return -EINVAL;
}

/* Write handler: processes each line of input as a separate command. */
static int dyndbg_control_file_write(const char *buf, int len)
{
    if (!buf || len <= 0) return -EINVAL;

    const char *p = buf;
    int remaining = len;

    while (remaining > 0) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)remaining);
        int line_len;

        if (nl) {
            line_len = (int)(nl - p);
            remaining -= (line_len + 1);
        } else {
            line_len = remaining;
            remaining = 0;
        }

        if (line_len > 0 && p[line_len - 1] == '\r') line_len--;

        int ret = dyndbg_parse_and_apply(p, line_len);
        if (ret < 0 && ret != -EINVAL) return ret;

        p = nl ? nl + 1 : p + line_len;
    }

    return len;
}

/* ── Stub: dyndbg_clear ─────────────────────────────── */
static int dyndbg_clear(void)
{
    kprintf("[DYNDBG] dyndbg_clear: not yet implemented\n");
    return 0;
}
/* ── Stub: dyndbg_set_flags ─────────────────────────────── */
static int dyndbg_set_flags(const char *module, unsigned long flags)
{
    (void)module;
    (void)flags;
    kprintf("[DYNDBG] dyndbg_set_flags: not yet implemented\n");
    return 0;
}
