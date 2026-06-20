/*
 * cmd_dmsetup.c — Device mapper control command
 *
 * Provides a Linux-compatible dmsetup command for managing
 * device mapper virtual block devices.
 *
 * Usage:
 *   dmsetup create <name> --table "<table_spec>"
 *   dmsetup remove <name>
 *   dmsetup remove_all
 *   dmsetup suspend <name>
 *   dmsetup resume <name>
 *   dmsetup table <name>
 *   dmsetup status [name]
 *   dmsetup info [name]
 *   dmsetup targets
 *   dmsetup ls
 *
 * Table format: "start_sectors length_sectors target_type args..."
 *   dmsetup create myvol --table "0 2097152 linear 0 0"
 *   dmsetup create data --table "0 1048576 linear 3 0"
 *
 * Item 321: Device mapper framework
 * Item 322: dm-linear target
 */

#define KERNEL_INTERNAL
#include "dm.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "blockdev.h"
#include "vfs.h"

/* ── Helper: find dm device index by name ─────────────────────────── */

static int find_dm_by_name(const char *name)
{
    for (int i = 0; i < DM_MAX_DEVICES; i++) {
        struct dm_device *dm = dm_device_get(i);
        if (dm && strcmp(dm->name, name) == 0)
            return i;
    }
    return -1;
}

/* ── Command handlers ─────────────────────────────────────────────── */

/* dmsetup create <name> --table "<table>" */
static int cmd_create(int argc, const char **argv)
{
    if (argc < 4) {
        kprintf("Usage: dmsetup create <name> --table \"<table>\"\n");
        return -1;
    }

    const char *name = argv[2];
    const char *table = NULL;

    /* Parse --table flag */
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--table") == 0 && i + 1 < argc) {
            table = argv[i + 1];
            break;
        }
    }

    if (!table) {
        kprintf("dmsetup: missing --table argument\n");
        return -1;
    }

    /* Compute device size from table: sum of all length fields */
    uint64_t total_sectors = 0;
    const char *tp = table;
    while (*tp) {
        /* Skip whitespace/newlines */
        while (*tp == ' ' || *tp == '\t' || *tp == '\n') tp++;
        if (!*tp) break;

        /* Parse start sector */
        uint64_t start = 0;
        while (*tp >= '0' && *tp <= '9')
            start = start * 10 + (*tp++ - '0');
        while (*tp == ' ' || *tp == '\t') tp++;
        if (!*tp) break;

        /* Parse length */
        uint64_t len = 0;
        while (*tp >= '0' && *tp <= '9')
            len = len * 10 + (*tp++ - '0');
        while (*tp == ' ' || *tp == '\t') tp++;

        uint64_t end = start + len;
        if (end > total_sectors)
            total_sectors = end;

        /* Skip remaining args on this line */
        while (*tp && *tp != '\n') tp++;
    }

    if (total_sectors == 0) {
        kprintf("dmsetup: empty table\n");
        return -1;
    }

    /* Create the device */
    int dm_id = dm_device_create(name, total_sectors);
    if (dm_id < 0) {
        kprintf("dmsetup: create failed: %d\n", dm_id);
        return -1;
    }

    /* Load the table */
    int ret = dm_table_load(dm_id, table);
    if (ret != 0) {
        kprintf("dmsetup: table load failed: %d, removing device\n", ret);
        dm_device_remove(dm_id);
        return -1;
    }

    struct dm_device *dm = dm_device_get(dm_id);
    kprintf("dmsetup: created '%s' (dm-%d, %llu sectors, %d target(s), dev /dev/%s)\n",
            name, dm_id, (unsigned long long)total_sectors,
            dm ? dm->num_targets : 0,
            blockdev_name(dm ? dm->dev_id : 0));
    return 0;
}

/* dmsetup remove <name> */
static int cmd_remove(int argc, const char **argv)
{
    if (argc < 3) {
        kprintf("Usage: dmsetup remove <name>\n");
        return -1;
    }

    int dm_id = find_dm_by_name(argv[2]);
    if (dm_id < 0) {
        kprintf("dmsetup: device '%s' not found\n", argv[2]);
        return -1;
    }

    int ret = dm_device_remove(dm_id);
    if (ret != 0) {
        kprintf("dmsetup: remove failed: %d\n", ret);
        return -1;
    }
    kprintf("dmsetup: removed '%s'\n", argv[2]);
    return 0;
}

/* dmsetup remove_all */
static int cmd_remove_all(void)
{
    int count = 0;
    for (int i = 0; i < DM_MAX_DEVICES; i++) {
        struct dm_device *dm = dm_device_get(i);
        if (dm) {
            kprintf("  removing '%s' (dm-%d)\n", dm->name, i);
            dm_device_remove(i);
            count++;
        }
    }
    kprintf("dmsetup: removed %d device(s)\n", count);
    return 0;
}

/* dmsetup suspend <name> */
static int cmd_suspend(const char *name)
{
    int dm_id = find_dm_by_name(name);
    if (dm_id < 0) {
        kprintf("dmsetup: device '%s' not found\n", name);
        return -1;
    }
    return dm_device_suspend(dm_id);
}

/* dmsetup resume <name> */
static int cmd_resume(const char *name)
{
    int dm_id = find_dm_by_name(name);
    if (dm_id < 0) {
        kprintf("dmsetup: device '%s' not found\n", name);
        return -1;
    }
    return dm_device_resume(dm_id);
}

/* dmsetup table <name> — show mapping table */
static int cmd_table(const char *name)
{
    int dm_id = find_dm_by_name(name);
    if (dm_id < 0) {
        kprintf("dmsetup: device '%s' not found\n", name);
        return -1;
    }

    struct dm_device *dm = dm_device_get(dm_id);
    if (!dm) return -1;

    kprintf("Table for '%s' (dm-%d, %llu sectors, %d target(s)):\n",
            name, dm_id, (unsigned long long)dm->sector_count, dm->num_targets);

    char buf[1024];
    dm_device_status(dm_id, buf, sizeof(buf));
    kprintf("%s", buf);
    return 0;
}

/* dmsetup status [name] — show status of all or one device */
static int cmd_status(const char *name)
{
    if (name) {
        int dm_id = find_dm_by_name(name);
        if (dm_id < 0) {
            kprintf("dmsetup: device '%s' not found\n", name);
            return -1;
        }
        char buf[1024];
        dm_device_status(dm_id, buf, sizeof(buf));
        kprintf("%s", buf);
    } else {
        for (int i = 0; i < DM_MAX_DEVICES; i++) {
            struct dm_device *dm = dm_device_get(i);
            if (dm) {
                char buf[512];
                dm_device_status(i, buf, sizeof(buf));
                kprintf("Device dm-%d:\n%s", i, buf);
            }
        }
    }
    return 0;
}

/* dmsetup info [name] — show device info */
static int cmd_info(const char *name)
{
    if (name) {
        int dm_id = find_dm_by_name(name);
        if (dm_id < 0) {
            kprintf("dmsetup: device '%s' not found\n", name);
            return -1;
        }
        struct dm_device *dm = dm_device_get(dm_id);
        if (!dm) return -1;
        kprintf("Name:       %s\n"
                "Dev ID:     %d (/dev/%s)\n"
                "Sectors:    %llu\n"
                "Size:       %llu MB\n"
                "Targets:    %d\n"
                "Suspended:  %d\n",
                dm->name, dm->dev_id, blockdev_name(dm->dev_id),
                (unsigned long long)dm->sector_count,
                (unsigned long long)(dm->sector_count / 2048),
                dm->num_targets, dm->suspended);
    } else {
        kprintf("Device Mapper devices:\n");
        int count = 0;
        for (int i = 0; i < DM_MAX_DEVICES; i++) {
            struct dm_device *dm = dm_device_get(i);
            if (dm) {
                kprintf("  dm-%d: %s  (%llu sectors, %d target(s), %s)\n",
                        i, dm->name,
                        (unsigned long long)dm->sector_count,
                        dm->num_targets,
                        dm->suspended ? "SUSPENDED" : "active");
                count++;
            }
        }
        if (count == 0)
            kprintf("  (none)\n");
        kprintf("Total: %d device(s)\n", count);
    }
    return 0;
}

/* dmsetup targets — list registered target types */
static int cmd_targets(void)
{
    kprintf("Registered target types:\n");
    kprintf("  linear  — Linear mapping (contiguous range)\n");
    kprintf("  (more targets can be registered by modules)\n");
    return 0;
}

/* dmsetup ls — list devices (compact) */
static int cmd_ls(void)
{
    kprintf("Device Mapper devices:\n");
    int count = 0;
    for (int i = 0; i < DM_MAX_DEVICES; i++) {
        struct dm_device *dm = dm_device_get(i);
        if (dm) {
            kprintf("  %-16s (dm-%d, %7llu sectors, %d target(s))\n",
                    dm->name, i,
                    (unsigned long long)dm->sector_count,
                    dm->num_targets);
            count++;
        }
    }
    if (count == 0)
        kprintf("  (none)\n");
    return 0;
}

/* ── Main command dispatch ────────────────────────────────────────── */

void cmd_dmsetup(const char *args)
{
    /* Parse command line into argv */
    const char *argv[16];
    int argc = 0;
    char cmdbuf[256];

    /* Copy args to a mutable buffer */
    strncpy(cmdbuf, args, sizeof(cmdbuf) - 1);
    cmdbuf[sizeof(cmdbuf) - 1] = '\0';

    /* Tokenize */
    char *tok = cmdbuf;
    char *q = cmdbuf;
    int in_quote = 0;

    while (*q && argc < 16) {
        /* Skip leading whitespace */
        while (*q == ' ' || *q == '\t') q++;
        if (!*q) break;

        if (*q == '"' || *q == '\'') {
            in_quote = *q;
            q++;
            tok = q;
            while (*q && *q != in_quote) q++;
            if (*q) { *q = '\0'; q++; }
            argv[argc++] = tok;
            in_quote = 0;
        } else {
            tok = q;
            while (*q && *q != ' ' && *q != '\t') q++;
            if (*q) { *q = '\0'; q++; }
            argv[argc++] = tok;
        }
    }

    if (argc < 2) {
        kprintf("Usage: dmsetup <command> [args...]\n"
                "Commands:\n"
                "  create <name> --table \"<table>\"\n"
                "  remove <name>\n"
                "  remove_all\n"
                "  suspend <name>\n"
                "  resume <name>\n"
                "  table [name]\n"
                "  status [name]\n"
                "  info [name]\n"
                "  targets\n"
                "  ls\n");
        return;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "create") == 0) {
        cmd_create(argc, argv);
    } else if (strcmp(cmd, "remove") == 0) {
        cmd_remove(argc, argv);
    } else if (strcmp(cmd, "remove_all") == 0) {
        cmd_remove_all();
    } else if (strcmp(cmd, "suspend") == 0) {
        if (argc < 3) kprintf("Usage: dmsetup suspend <name>\n");
        else cmd_suspend(argv[2]);
    } else if (strcmp(cmd, "resume") == 0) {
        if (argc < 3) kprintf("Usage: dmsetup resume <name>\n");
        else cmd_resume(argv[2]);
    } else if (strcmp(cmd, "table") == 0) {
        cmd_table(argc >= 3 ? argv[2] : NULL);
    } else if (strcmp(cmd, "status") == 0) {
        cmd_status(argc >= 3 ? argv[2] : NULL);
    } else if (strcmp(cmd, "info") == 0) {
        cmd_info(argc >= 3 ? argv[2] : NULL);
    } else if (strcmp(cmd, "targets") == 0) {
        cmd_targets();
    } else if (strcmp(cmd, "ls") == 0) {
        cmd_ls();
    } else {
        kprintf("dmsetup: unknown command '%s'\n", cmd);
        kprintf("Try: create, remove, remove_all, suspend, resume, table, status, info, targets, ls\n");
    }
}
