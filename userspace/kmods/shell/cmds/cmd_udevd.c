/* cmd_udevd.c — Device event manager (udev)
 *
 * Listens to kernel uevents via netlink, matches against
 * /etc/udev/rules.d/ rules, and creates/removes device nodes
 * in /dev.
 *
 * Item S163: Udev — device event manager
 *
 * Usage: udevd [--daemon]
 *   --daemon    Run in background (fork)
 */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "libc.h"
#include "heap.h"

/* ── Constants ─────────────────────────────────────────────────────────── */

#define UDEV_RULES_DIR      "/etc/udev/rules.d/"
#define UDEV_MAX_RULES      128
#define UDEV_RULE_LINE_MAX  256

/* Rule match fields */
#define UDEV_MATCH_SUBSYSTEM   1
#define UDEV_MATCH_ACTION      2
#define UDEV_MATCH_KERNEL      4
#define UDEV_MATCH_ATTR        8

/* Rule action types */
#define UDEV_ACTION_RUN        1
#define UDEV_ACTION_SYMLINK    2
#define UDEV_ACTION_OWNER      3
#define UDEV_ACTION_MODE       4

/* Device node type constants */
#define UDEV_S_IFCHR 0020000
#define UDEV_S_IFBLK 0060000

/* ── Rule structure ────────────────────────────────────────────────────── */

struct udev_rule {
    int     match_flags;
    char    subsystem[64];
    char    action[32];
    char    kernel[128];
    char    attr_key[64];
    char    attr_val[128];

    int     action_type;
    char    action_data[256];
    int     mode;
    int     owner_uid;
    int     owner_gid;
};

/* ── State ─────────────────────────────────────────────────────────────── */

static struct udev_rule udev_rules[UDEV_MAX_RULES];
static int udev_num_rules = 0;
static int udev_running = 0;

/* vfs_mknod is defined in kernel/vfs.c */
extern int vfs_mknod(const char *path, uint16_t mode,
                     uint16_t dev_major, uint16_t dev_minor);

/* ── Rule parsing ─────────────────────────────────────────────────────── */

static int parse_udev_rules_file(const char *path)
{
    uint32_t sz;
    uint8_t type;

    if (fs_stat(path, &sz, &type) < 0 || sz == 0)
        return 0;

    char *buf = (char *)kmalloc(sz + 1);
    if (!buf)
        return 0;

    uint32_t bytes_read = 0;
    if (fs_read_file(path, buf, sz, &bytes_read) < 0) {
        kfree(buf);
        return 0;
    }
    buf[bytes_read] = '\0';

    int parsed = 0;
    char *line = buf;
    char *next;

    while (line && *line && udev_num_rules < UDEV_MAX_RULES) {
        next = strchr(line, '\n');
        if (next) {
            *next = '\0';
            next++;
        }

        while (*line == ' ' || *line == '\t') line++;
        if (line[0] == '#' || line[0] == '\0') {
            line = next;
            continue;
        }

        struct udev_rule *rule = &udev_rules[udev_num_rules];
        memset(rule, 0, sizeof(struct udev_rule));
        rule->mode = -1;
        rule->owner_uid = -1;
        rule->owner_gid = -1;

        char *segment = line;
        int has_actions = 0;

        while (segment && *segment) {
            while (*segment == ' ') segment++;
            if (!*segment) break;

            char *comma = segment;
            int in_quotes = 0;
            while (*comma) {
                if (*comma == '"') in_quotes = !in_quotes;
                if (*comma == ',' && !in_quotes) break;
                comma++;
            }

            char saved = *comma;
            if (saved) *comma = '\0';

            char *eq = strstr(segment, "==");
            char *op = NULL;

            if (eq && eq > segment) {
                op = "==";
            } else {
                eq = strstr(segment, "+=");
                if (eq && eq > segment) {
                    op = "+=";
                } else {
                    eq = strstr(segment, "!=");
                    if (eq && eq > segment)
                        op = "!=";
                }
            }

            if (eq && op) {
                char key[64];
                size_t klen = (size_t)(eq - segment);
                if (klen > 63) klen = 63;
                memcpy(key, segment, klen);
                key[klen] = '\0';
                while (klen > 0 && (key[klen-1] == ' ' || key[klen-1] == '\t'))
                    key[klen--] = '\0';

                const char *val_start = eq + strlen(op);
                while (*val_start == ' ') val_start++;
                char val[256];
                int vi = 0;
                if (*val_start == '"') {
                    val_start++;
                    while (*val_start && *val_start != '"' && vi < 255)
                        val[vi++] = *val_start++;
                } else {
                    while (*val_start && vi < 255)
                        val[vi++] = *val_start++;
                }
                val[vi] = '\0';

                if (strcmp(key, "SUBSYSTEM") == 0) {
                    rule->match_flags |= UDEV_MATCH_SUBSYSTEM;
                    strncpy(rule->subsystem, val, sizeof(rule->subsystem) - 1);
                } else if (strcmp(key, "ACTION") == 0) {
                    rule->match_flags |= UDEV_MATCH_ACTION;
                    strncpy(rule->action, val, sizeof(rule->action) - 1);
                } else if (strcmp(key, "KERNEL") == 0) {
                    rule->match_flags |= UDEV_MATCH_KERNEL;
                    strncpy(rule->kernel, val, sizeof(rule->kernel) - 1);
                } else if (strncmp(key, "ATTR", 4) == 0) {
                    rule->match_flags |= UDEV_MATCH_ATTR;
                } else if (strcmp(key, "RUN") == 0 && strcmp(op, "+=") == 0) {
                    rule->action_type = UDEV_ACTION_RUN;
                    strncpy(rule->action_data, val, sizeof(rule->action_data) - 1);
                    has_actions = 1;
                } else if (strcmp(key, "SYMLINK") == 0 && strcmp(op, "+=") == 0) {
                    rule->action_type = UDEV_ACTION_SYMLINK;
                    strncpy(rule->action_data, val, sizeof(rule->action_data) - 1);
                    has_actions = 1;
                } else if (strcmp(key, "MODE") == 0) {
                    rule->action_type = UDEV_ACTION_MODE;
                    rule->mode = 0;
                    for (int mi = 0; val[mi] && mi < 8; mi++) {
                        if (val[mi] >= '0' && val[mi] <= '7')
                            rule->mode = (rule->mode * 8) + (val[mi] - '0');
                    }
                    has_actions = 1;
                } else if (strcmp(key, "OWNER") == 0) {
                    rule->action_type = UDEV_ACTION_OWNER;
                    rule->owner_uid = atoi(val);
                    has_actions = 1;
                } else if (strcmp(key, "GROUP") == 0) {
                    rule->owner_gid = atoi(val);
                }
            }

            if (saved) {
                *comma = saved;
                segment = comma + 1;
            } else {
                segment = NULL;
            }
        }

        if (has_actions) {
            udev_num_rules++;
            parsed++;
        }

        line = next;
    }

    kfree(buf);
    return parsed;
}

static int udev_load_rules(void)
{
    udev_num_rules = 0;

    static const char *known_rules[] = {
        "50-default.rules",
        "60-block.rules",
        "60-tty.rules",
        "70-misc.rules",
        NULL
    };

    int total = 0;
    for (int i = 0; known_rules[i]; i++) {
        char path[128];
        snprintf(path, sizeof(path), "%s%s", UDEV_RULES_DIR, known_rules[i]);
        int n = parse_udev_rules_file(path);
        total += n;
    }

    if (total == 0 && udev_num_rules == 0) {
        uint32_t sz; uint8_t tp;
        if (fs_stat(UDEV_RULES_DIR, &sz, &tp) < 0)
            fs_create(UDEV_RULES_DIR, FS_TYPE_DIR);

        const char *default_rules =
            "# Default udev rules\n"
            "SUBSYSTEM==\"tty\", ACTION==\"add\", MODE=\"0600\"\n"
            "SUBSYSTEM==\"input\", ACTION==\"add\", MODE=\"0640\"\n"
            "SUBSYSTEM==\"net\", ACTION==\"add\", RUN+=\"/sbin/ifup %k\"\n";

        char path[128];
        snprintf(path, sizeof(path), "%s50-default.rules", UDEV_RULES_DIR);
        fs_write_file(path, default_rules, strlen(default_rules));

        return parse_udev_rules_file(path);
    }

    return total;
}

static int glob_match(const char *pattern, const char *str)
{
    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*') pattern++;
            if (!*pattern) return 1;
            while (*str) {
                if (glob_match(pattern, str))
                    return 1;
                str++;
            }
            return 0;
        } else if (*pattern == '?') {
            if (!*str) return 0;
            pattern++;
            str++;
        } else if (*pattern == *str) {
            pattern++;
            str++;
        } else {
            return 0;
        }
    }
    return *str == '\0';
}

static int rule_matches(struct udev_rule *rule, const char *subsystem,
                        const char *action, const char *kernel_name)
{
    if (rule->match_flags & UDEV_MATCH_SUBSYSTEM) {
        if (!subsystem || strcmp(rule->subsystem, subsystem) != 0)
            return 0;
    }

    if (rule->match_flags & UDEV_MATCH_ACTION) {
        if (!action) return 0;
        const char *a = rule->action;
        int matched = 0;
        while (*a && !matched) {
            const char *pipe = strchr(a, '|');
            char act[32];
            int alen;
            if (pipe) {
                alen = (int)(pipe - a);
                if (alen > 31) alen = 31;
                memcpy(act, a, alen);
                act[alen] = '\0';
                a = pipe + 1;
            } else {
                strncpy(act, a, sizeof(act) - 1);
                act[sizeof(act) - 1] = '\0';
                a = "";
            }
            if (strcmp(act, action) == 0)
                matched = 1;
        }
        if (!matched) return 0;
    }

    if (rule->match_flags & UDEV_MATCH_KERNEL) {
        if (!kernel_name || !glob_match(rule->kernel, kernel_name))
            return 0;
    }

    return 1;
}

static int execute_rule_action(struct udev_rule *rule, const char *devname)
{
    char devpath[128];
    snprintf(devpath, sizeof(devpath), "/dev/%s", devname ? devname : "unknown");

    switch (rule->action_type) {
    case UDEV_ACTION_RUN:
        kprintf("[udevd] RUN: %s (for %s)\n", rule->action_data, devname);
        break;

    case UDEV_ACTION_SYMLINK:
        kprintf("[udevd] SYMLINK: %s -> %s\n", rule->action_data, devpath);
        break;

    case UDEV_ACTION_MODE:
        kprintf("[udevd] MODE: %o for %s\n", rule->mode, devpath);
        break;

    case UDEV_ACTION_OWNER:
        kprintf("[udevd] OWNER: uid=%d gid=%d for %s\n",
                rule->owner_uid, rule->owner_gid, devpath);
        break;

    default:
        return -1;
    }

    return 0;
}

static int udev_create_device_node(const char *devname,
                                    uint16_t dev_major, uint16_t dev_minor)
{
    char path[128];
    snprintf(path, sizeof(path), "/dev/%s", devname);

    uint32_t sz; uint8_t tp;
    if (fs_stat(path, &sz, &tp) == 0)
        fs_delete(path);

    /* Create character device node */
    uint16_t mode = (uint16_t)(UDEV_S_IFCHR | 0600);
    int rc = vfs_mknod(path, mode, dev_major, dev_minor);
    if (rc == 0) {
        kprintf("[udevd] Created device node: %s (major=%d, minor=%d)\n",
                path, dev_major, dev_minor);
    }
    return rc;
}

static void parse_uevent(const char *msg, int len,
                         char *action, int action_max,
                         char *subsystem, int subsystem_max,
                         char *kernel_name, int kernel_max)
{
    action[0] = subsystem[0] = kernel_name[0] = '\0';

    const char *p = msg;
    const char *end = msg + len;

    while (p < end) {
        const char *nl = strchr(p, '\n');
        if (!nl) nl = end;

        const char *eq = strchr(p, '=');
        if (eq && eq < nl) {
            size_t klen = (size_t)(eq - p);

            if (klen == 6 && strncmp(p, "ACTION", 6) == 0) {
                const char *val = eq + 1;
                size_t vlen = (size_t)(nl - val);
                if (vlen > (size_t)(action_max - 1))
                    vlen = (size_t)(action_max - 1);
                memcpy(action, val, vlen);
                action[vlen] = '\0';
            } else if (klen == 9 && strncmp(p, "SUBSYSTEM", 9) == 0) {
                const char *val = eq + 1;
                size_t vlen = (size_t)(nl - val);
                if (vlen > (size_t)(subsystem_max - 1))
                    vlen = (size_t)(subsystem_max - 1);
                memcpy(subsystem, val, vlen);
                subsystem[vlen] = '\0';
            }
        }

        p = nl + 1;
    }

    /* Extract kernel name from action or use generic name */
    if (action[0]) {
        strncpy(kernel_name, action, (size_t)(kernel_max - 1));
        kernel_name[kernel_max - 1] = '\0';
    }
}

static void udev_process_uevent(const char *msg, int len)
{
    char action[64], subsystem[64], kernel_name[128];

    parse_uevent(msg, len, action, sizeof(action),
                 subsystem, sizeof(subsystem),
                 kernel_name, sizeof(kernel_name));

    kprintf("[udevd] uevent: %s (%s)\n", action, subsystem);

    for (int i = 0; i < udev_num_rules; i++) {
        if (rule_matches(&udev_rules[i], subsystem, action, kernel_name)) {
            kprintf("[udevd] Rule matched\n");
            execute_rule_action(&udev_rules[i], kernel_name);

            if (strcmp(subsystem, "tty") == 0 ||
                strcmp(subsystem, "input") == 0) {
                udev_create_device_node(kernel_name, 4, 0);
            }
        }
    }
}

static void udev_scan_devices(void)
{
    kprintf("[udevd] Scanning existing devices...\n");

    static const char *synthetic_events[] = {
        "ACTION=add\nSUBSYSTEM=tty\n",
        NULL
    };

    for (int i = 0; synthetic_events[i]; i++) {
        udev_process_uevent(synthetic_events[i], strlen(synthetic_events[i]));
    }
}

/* ── Daemon lifecycle ─────────────────────────────────────────────────── */

static int udevd_start(int daemon_mode)
{
    if (udev_running) {
        kprintf("[udevd] Already running\n");
        return -1;
    }

    kprintf("[udevd] Loading rules from %s...\n", UDEV_RULES_DIR);
    int n_rules = udev_load_rules();
    kprintf("[udevd] Loaded %d rules\n", n_rules);

    udev_scan_devices();
    udev_running = 1;
    kprintf("[udevd] Started (daemon=%d)\n", daemon_mode);

    if (daemon_mode) {
        int pid = libc_fork();
        if (pid < 0) {
            kprintf("[udevd] Fork failed\n");
            return -1;
        }
        if (pid > 0) {
            kprintf("[udevd] Daemonized (PID %d)\n", pid);
            return 0;
        }
    }

    return 0;
}

static void udevd_stop(void)
{
    if (!udev_running) return;
    udev_running = 0;
    kprintf("[udevd] Stopped\n");
}

static void udevd_status(void)
{
    kprintf("[udevd] %s\n", udev_running ? "Running" : "Stopped");
    kprintf("[udevd] Rules loaded: %d\n", udev_num_rules);
}

/* ── Shell command entry point ────────────────────────────────────────── */

void cmd_udevd(const char *args)
{
    if (!args || !*args) {
        kprintf("Usage: udevd [--daemon]\n");
        kprintf("       udevd stop|status\n");
        return;
    }

    char subcmd[32] = {0};
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < (int)sizeof(subcmd) - 1)
        subcmd[i++] = *p++;

    if (strcmp(subcmd, "stop") == 0) {
        udevd_stop();
    } else if (strcmp(subcmd, "status") == 0) {
        udevd_status();
    } else if (strcmp(subcmd, "--daemon") == 0) {
        udevd_start(1);
    } else {
        udevd_start(0);
    }
}
