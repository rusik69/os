/* cmd_mdev.c — Minimal device node manager (BusyBox mdev-style)
 *
 * Usage:
 *   mdev -s        — Scan all known devices and create /dev entries
 *   mdev           — Hotplug trigger (placeholder for future uevent handler)
 *
 * On boot, `mdev -s` should be called after sysfs is mounted to populate /dev/
 * with device nodes for all available hardware.  It reads /etc/mdev.conf for
 * custom ownership and permission overrides.
 *
 * This runs in kernel space and uses devtmpfs_create_device() directly.
 */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "devtmpfs.h"
#include "nvme.h"

/* ── Well-known device nodes ───────────────────────────────────── */

struct dev_entry {
    const char *name;
    uint8_t     type;       /* DT_CHAR or DT_BLOCK */
    uint32_t    major;
    uint32_t    minor;
};

/* Standard character devices (Linux major numbering) */
static const struct dev_entry standard_chr[] = {
    { "null",      DT_CHAR, 1,  3 },
    { "zero",      DT_CHAR, 1,  5 },
    { "random",    DT_CHAR, 1,  8 },
    { "urandom",   DT_CHAR, 1,  9 },
    { "kmsg",      DT_CHAR, 1, 11 },
    { "console",   DT_CHAR, 5,  1 },
    { "ptmx",      DT_CHAR, 5,  2 },
    { "tty",       DT_CHAR, 5,  0 },
    { "ttyS0",     DT_CHAR, 4, 64 },
    { "ttyS1",     DT_CHAR, 4, 65 },
    { "ttyS2",     DT_CHAR, 4, 66 },
    { "ttyS3",     DT_CHAR, 4, 67 },
    { "loop0",     DT_BLOCK, 7,  0 },
    { "loop1",     DT_BLOCK, 7,  1 },
    { "loop2",     DT_BLOCK, 7,  2 },
    { "loop3",     DT_BLOCK, 7,  3 },
    { "fd0",       DT_BLOCK, 2,  0 },
    { "fd1",       DT_BLOCK, 2,  1 },
    { "hda",       DT_BLOCK, 3,  0 },
    { "hdb",       DT_BLOCK, 3, 64 },
    { "hdc",       DT_BLOCK, 22, 0 },
    { "hdd",       DT_BLOCK, 22, 64 },
    { "sda",       DT_BLOCK, 8,  0 },
    { "sdb",       DT_BLOCK, 8, 16 },
    { "sdc",       DT_BLOCK, 8, 32 },
    { "sdd",       DT_BLOCK, 8, 48 },
    { "nvme0",     DT_BLOCK, 250, 0 },
    { "nvme1",     DT_BLOCK, 250, 1 },
    { "vda",       DT_BLOCK, 254, 0 },
    { "vdb",       DT_BLOCK, 254, 16 },
    { "mmcblk0",   DT_BLOCK, 179, 0 },
    { "cpu/0",     DT_CHAR, 10, 0 },  /* /sys/devices/system/cpu */
};

#define STANDARD_CHR_COUNT  (sizeof(standard_chr) / sizeof(standard_chr[0]))

/* ── mdev.conf parsing ──────────────────────────────────────────── */

#define MDEV_CONF_PATH "/etc/mdev.conf"
#define MDEV_CONF_LINE_MAX 128

struct mdev_rule {
    char    name[32];          /* device name (glob-style) */
    uint8_t type;              /* DT_CHAR or DT_BLOCK (0 = any) */
    uint16_t uid;
    uint16_t gid;
    uint16_t mode;
};

#define MDEV_RULES_MAX 32
static struct mdev_rule mdev_rules[MDEV_RULES_MAX];
static int mdev_num_rules = 0;

/* Parse a single line from /etc/mdev.conf.
 * Format:  device_name  uid:gid  mode
 *   e.g.:  null         0:0     0666
 *          ttyS0        0:20    0660
 *          sda          0:0     0640
 */
static int parse_mdev_rule(const char *line)
{
    struct mdev_rule rule;
    memset(&rule, 0, sizeof(rule));

    /* Skip leading whitespace and comments */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '#' || *line == '\0' || *line == '\n')
        return 0;

    /* Read device name */
    int i = 0;
    while (*line && *line != ' ' && *line != '\t' && *line != '\n' && *line != '\r'
           && *line != ':' && i < (int)sizeof(rule.name) - 1)
        rule.name[i++] = *line++;
    rule.name[i] = '\0';
    if (i == 0) return 0;

    while (*line == ' ' || *line == '\t') line++;

    /* Parse uid:gid */
    char *end;
    rule.uid = (uint16_t)strtoul(line, &end, 10);
    if (end == line) return 0;
    line = end;
    if (*line != ':') return 0;
    line++;
    rule.gid = (uint16_t)strtoul(line, &end, 10);
    if (end == line) return 0;
    line = end;

    while (*line == ' ' || *line == '\t') line++;

    /* Parse mode (octal) */
    rule.mode = (uint16_t)strtoul(line, &end, 8);
    if (end == line) rule.mode = 0660; /* default */

    /* Store if we have room */
    if (mdev_num_rules < MDEV_RULES_MAX)
        mdev_rules[mdev_num_rules++] = rule;

    return 1;
}

static void load_mdev_conf(void)
{
    char buf[2048];
    uint32_t size;

    mdev_num_rules = 0;
    if (vfs_read(MDEV_CONF_PATH, buf, sizeof(buf) - 1, &size) < 0)
        return; /* no config — use defaults */

    buf[size] = '\0';

    /* Parse line by line */
    char *line = buf;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        parse_mdev_rule(line);
        if (nl) {
            *nl = '\n';
            line = nl + 1;
        } else {
            break;
        }
    }
}

/* Find matching rule for a device name */
static const struct mdev_rule *find_rule(const char *name)
{
    for (int i = 0; i < mdev_num_rules; i++) {
        if (strcmp(mdev_rules[i].name, name) == 0)
            return &mdev_rules[i];
    }
    return NULL;
}

/* ── Device creation ────────────────────────────────────────────── */

static int create_devnode(const char *name, uint8_t type,
                          uint32_t major, uint32_t minor)
{
    /* Check for custom rule */
    const struct mdev_rule *rule = find_rule(name);
    if (rule) {
        kprintf("[mdev] rule match for %s: mode=%o uid=%u gid=%u\n",
                name, rule->mode, rule->uid, rule->gid);
    }

    char path[96];
    int ret = snprintf(path, sizeof(path), "/dev/%s", name);
    if (ret < 0 || ret >= (int)sizeof(path))
        return -1;

    return devtmpfs_mknod(path, type, major, minor);
}

/* ══════════════════════════════════════════════════════════════════
 *                     Command Entry Points
 * ══════════════════════════════════════════════════════════════════ */

/* mdev -s: Create device nodes for all known devices */
static void mdev_scan(void)
{
    int created = 0;
    int errors = 0;

    kprintf("mdev: populating /dev...\n");

    /* Load custom rules from /etc/mdev.conf */
    load_mdev_conf();

    /* Create all standard device nodes */
    for (size_t i = 0; i < STANDARD_CHR_COUNT; i++) {
        const struct dev_entry *de = &standard_chr[i];
        if (create_devnode(de->name, de->type, de->major, de->minor) == 0)
            created++;
        else
            errors++;
    }

    /* Conditionally create block device nodes based on detected hardware */

    /* ATA (PATA) primary master */
    if (ata_is_present()) {
        if (create_devnode("hda", DT_BLOCK, 3, 0) == 0) created++;
        if (create_devnode("hdb", DT_BLOCK, 3, 64) == 0) created++;
    }

    /* AHCI / SATA */
    if (ahci_is_present()) {
        for (int port = 0; port < 6; port++) {
            char name[16];
            snprintf(name, sizeof(name), "sd%c", (char)('a' + port));
            if (create_devnode(name, DT_BLOCK, 8, (uint32_t)(port * 16)) == 0)
                created++;
        }
    }

    /* NVMe */
    if (nvme_is_present()) {
        for (int ns = 0; ns < 2; ns++) {
            char name[16];
            snprintf(name, sizeof(name), "nvme0n%dp1", ns + 1);
            create_devnode(name, DT_BLOCK, 250, (uint32_t)(ns * 16));
            /* Also create the raw namespace device */
            snprintf(name, sizeof(name), "nvme0n%d", ns + 1);
            if (create_devnode(name, DT_BLOCK, 250, (uint32_t)(ns * 16)) == 0)
                created++;
        }
    }

    /* Virtio block (if available) */
    /* Check via libc — currently no separate virtio_blk_present() */
    {
        for (int v = 0; v < 4; v++) {
            char name[16];
            snprintf(name, sizeof(name), "vd%c", (char)('a' + v));
            if (create_devnode(name, DT_BLOCK, 254, (uint32_t)(v * 16)) == 0)
                created++;
        }
    }

    /* Create partition entries for common block devices */
    /* sda1-4, hda1-4, vda1-4 */
    const char *prefixes[] = { "sd", "hd", "vd", NULL };
    for (int p = 0; prefixes[p]; p++) {
        for (char letter = 'a'; letter <= 'd'; letter++) {
            for (int part = 1; part <= 4; part++) {
                char name[16];
                snprintf(name, sizeof(name), "%s%c%d", prefixes[p], letter, part);
                if (create_devnode(name, DT_BLOCK, 0, 0) == 0)
                    created++;
            }
        }
    }

    kprintf("mdev: created %d device nodes (%d errors)\n", created, errors);
}

/* mdev (hotplug trigger): placeholder for future uevent handling */
static void mdev_hotplug(const char *uevent)
{
    (void)uevent;
    /* In the future, parse the uevent string, find the device,
     * match against /etc/mdev.conf rules, and create/remove the
     * device node dynamically.  For now, just rescan. */
    kprintf("mdev: hotplug event triggered — rescanning\n");
    mdev_scan();
}

/* ── Entry point ────────────────────────────────────────────────── */

void cmd_mdev(const char *args)
{
    /* Parse arguments */
    const char *p = args;
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '-' && *(p + 1) == 's') {
        /* mdev -s: initial scan */
        mdev_scan();
    } else if (*p == '\0') {
        /* mdev: hotplug trigger */
        mdev_hotplug(args);
    } else {
        kprintf("Usage: mdev [-s]\n"
                "  mdev -s    Scan all devices and create /dev entries\n"
                "  mdev       Hotplug trigger (rescan)\n");
    }
}
