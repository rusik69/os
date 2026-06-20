/*
 * cmd_sysctl.c — sysctl runtime tuning command
 *
 * Usage:
 *   sysctl [name]              — read a kernel parameter
 *   sysctl -a                  — list all parameters
 *   sysctl -w name=value       — write a parameter
 *   sysctl -p [file]           — load settings from file (default /etc/sysctl.conf)
 *
 * Reads/writes via /proc/sys/kernel/<name> interface.
 */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "vfs.h"
#include "sysctl.h"
#include "heap.h"

/* Size of the buffer used for reading sysctl values */
#define SYSCTL_BUF_SIZE 256
/* Maximum line length in sysctl.conf */
#define SYSCTL_CONF_LINE_MAX 256

/* ── Read a single sysctl value and print it ────────────────────────── */

static int sysctl_read_and_print(const char *name)
{
    char path[128];
    char buf[SYSCTL_BUF_SIZE];
    uint32_t out_size = 0;

    /* Build /proc/sys/kernel/<name> path */
    int plen = snprintf(path, sizeof(path), "/proc/sys/kernel/%s", name);
    if (plen < 0 || plen >= (int)sizeof(path))
        return -1;

    /* Read via VFS */
    if (vfs_read(path, buf, sizeof(buf) - 1, &out_size) < 0)
        return -1;

    buf[out_size] = '\0';

    /* Print "name = value" format */
    kprintf("%s = %s", name, buf);
    /* Ensure newline */
    if (out_size == 0 || buf[out_size - 1] != '\n')
        kprintf("\n");
    return 0;
}

/* ── Write a single sysctl value ────────────────────────────────────── */

static int sysctl_write_value(const char *name, const char *value)
{
    char path[128];
    int plen = snprintf(path, sizeof(path), "/proc/sys/kernel/%s", name);
    if (plen < 0 || plen >= (int)sizeof(path))
        return -1;

    /* Write via VFS */
    if (vfs_write(path, value, (uint32_t)strlen(value)) < 0)
        return -1;

    return 0;
}

/* ── List all available sysctl entries ─────────────────────────────── */

static void sysctl_list_all(void)
{
    const char *known_sysctls[] = {
        "hostname",
        "osrelease",
        "ostype",
        "panic",
        "randomize_va_space",
        "sched_latency_ns",
        "sched_min_granularity_ns",
        "sched_wakeup_granularity_ns",
        "sched_migration_cost_ns",
        "vm.swappiness",
        NULL
    };

    for (int i = 0; known_sysctls[i]; i++) {
        char path[128];
        char buf[SYSCTL_BUF_SIZE];
        uint32_t out_size = 0;

        int plen = snprintf(path, sizeof(path),
                            "/proc/sys/kernel/%s", known_sysctls[i]);
        if (plen < 0 || plen >= (int)sizeof(path))
            continue;

        if (vfs_read(path, buf, sizeof(buf) - 1, &out_size) == 0 && out_size > 0) {
            buf[out_size] = '\0';
            /* Remove trailing newline for cleaner display */
            int blen = (int)strlen(buf);
            while (blen > 0 && (buf[blen - 1] == '\n' || buf[blen - 1] == '\r'))
                buf[--blen] = '\0';
            kprintf("kernel.%s = %s\n", known_sysctls[i], buf);
        }
    }
}

/* ── Parse "name=value" for -w ──────────────────────────────────────── */

static int parse_assignment(const char *arg, char *name, int name_max,
                            char *value, int value_max)
{
    const char *eq = strchr(arg, '=');
    if (!eq)
        return -1;

    int nlen = (int)(eq - arg);
    if (nlen <= 0 || nlen >= name_max)
        return -1;
    memcpy(name, arg, (size_t)nlen);
    name[nlen] = '\0';

    const char *vstart = eq + 1;
    while (*vstart == ' ') vstart++;
    int vlen = (int)strlen(vstart);
    if (vlen >= value_max)
        vlen = value_max - 1;
    memcpy(value, vstart, (size_t)vlen);
    value[vlen] = '\0';

    return 0;
}

/* ── Load sysctl settings from a file ───────────────────────────────── */

static int sysctl_load_file(const char *filepath)
{
    /* Read the entire file into memory */
    struct vfs_stat st;
    if (vfs_stat(filepath, &st) < 0) {
        kprintf("sysctl: cannot open '%s'\n", filepath);
        return -1;
    }

    /* Clamp size to a reasonable maximum (64 KB) */
    uint32_t file_size = st.size;
    if (file_size > 65536) {
        kprintf("sysctl: file too large (%u bytes), max 65536\n",
                (unsigned int)file_size);
        return -1;
    }

    char *content = (char *)kmalloc(file_size + 1);
    if (!content) {
        kprintf("sysctl: out of memory\n");
        return -1;
    }

    uint32_t bytes_read = 0;
    if (vfs_read(filepath, content, file_size, &bytes_read) < 0) {
        kprintf("sysctl: error reading '%s'\n", filepath);
        kfree(content);
        return -1;
    }
    content[bytes_read] = '\0';

    /* Parse line by line */
    int loaded = 0;
    int errors = 0;
    char *line = content;
    while (line && *line) {
        /* Skip to next line */
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';

        /* Trim whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        /* Skip comments and empty lines */
        if (*p == '#' || *p == ';' || *p == '\0') {
            line = next;
            continue;
        }

        /* Parse: name = value */
        char name[64];
        char value[128];
        if (parse_assignment(p, name, sizeof(name), value, sizeof(value)) == 0) {
            /* Strip \"kernel.\" prefix if present */
            const char *sysctl_name = name;
            if (strncmp(name, "kernel.", 7) == 0)
                sysctl_name = name + 7;

            if (sysctl_write_value(sysctl_name, value) == 0) {
                loaded++;
            } else {
                kprintf("sysctl: failed to set '%s'\n", name);
                errors++;
            }
        } else {
            kprintf("sysctl: invalid line: '%s'\n", p);
            errors++;
        }

        line = next;
    }

    kfree(content);
    kprintf("sysctl: loaded %d settings from '%s'", loaded, filepath);
    if (errors > 0)
        kprintf(", %d error(s)", errors);
    kprintf("\n");
    return errors > 0 ? -1 : 0;
}

/* ── Main command entry point ───────────────────────────────────────── */

void cmd_sysctl(const char *args)
{
    /* Skip leading spaces */
    while (args && *args == ' ')
        args++;

    if (!args || *args == '\0') {
        /* No args: show usage */
        kprintf("Usage: sysctl [options] [name[=value] ...]\n");
        kprintf("Options:\n");
        kprintf("  -a          List all tunable parameters\n");
        kprintf("  -w name=val Set a parameter (mutually exclusive -w, -p)\n");
        kprintf("  -p [file]   Load values from file (default: /etc/sysctl.conf)\n");
        kprintf("  name        Read a parameter\n");
        return;
    }

    /* Parse options */
    if (args[0] == '-') {
        switch (args[1]) {
        case 'a':
            sysctl_list_all();
            return;

        case 'w': {
            /* sysctl -w name=value */
            const char *val_arg = args + 2;
            while (*val_arg == ' ') val_arg++;
            if (*val_arg == '\0') {
                kprintf("sysctl: missing 'name=value' after -w\n");
                return;
            }

            char name[64], value[128];
            if (parse_assignment(val_arg, name, sizeof(name),
                                 value, sizeof(value)) < 0) {
                kprintf("sysctl: invalid assignment '%s'\n", val_arg);
                return;
            }

            /* Strip "kernel." prefix if present */
            const char *sysctl_name = name;
            if (strncmp(name, "kernel.", 7) == 0)
                sysctl_name = name + 7;

            if (sysctl_write_value(sysctl_name, value) == 0) {
                kprintf("kernel.%s = %s\n", sysctl_name, value);
            } else {
                kprintf("sysctl: failed to set '%s'\n", name);
                kprintf("sysctl: use 'sysctl -a' to see available parameters\n");
            }
            return;
        }

        case 'p': {
            /* sysctl -p [file] */
            const char *filepath = args + 2;
            while (*filepath == ' ') filepath++;
            if (*filepath == '\0')
                filepath = "/etc/sysctl.conf";
            sysctl_load_file(filepath);
            return;
        }

        default:
            kprintf("sysctl: unknown option '-%c'\n", args[1]);
            return;
        }
    }

    /* Read a parameter by name */
    char param_name[64];
    const char *src = args;
    int i = 0;
    while (*src && *src != ' ' && i < (int)sizeof(param_name) - 1)
        param_name[i++] = *src++;
    param_name[i] = '\0';

    /* Strip "kernel." prefix if present */
    const char *sysctl_name = param_name;
    if (strncmp(param_name, "kernel.", 7) == 0)
        sysctl_name = param_name + 7;

    if (sysctl_read_and_print(sysctl_name) < 0)
        kprintf("sysctl: unknown parameter '%s'\n", param_name);
}
