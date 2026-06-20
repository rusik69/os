/*
 * cmd_lvcreate.c — Logical Volume Management (LVM) creation command
 *
 * Usage:
 *   lvcreate -l <extents> -n <name> <vg_name>       — create logical volume
 *   lvcreate -L <size>   -n <name> <vg_name>        — create with size
 *   lvcreate --help                               — show help
 *
 * Examples:
 *   lvcreate -l 100 -n mylv vg0                    — 100 extents
 *   lvcreate -L 1G -n mylv vg0                     — 1 GiB volume
 *
 * Integrates with dm-linear device mapper for the actual mapping.
 */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "dm.h"

static void cmd_lvcreate_usage(void)
{
    kprintf("Usage:\n");
    kprintf("  lvcreate -l <extents> -n <name> <vg_name>\n");
    kprintf("  lvcreate -L <size>   -n <name> <vg_name>\n");
    kprintf("  lvcreate --help\n");
    kprintf("Examples:\n");
    kprintf("  lvcreate -l 100 -n mylv vg0          — 100 extents\n");
    kprintf("  lvcreate -L 1G -n mylv vg0           — 1 GiB volume\n");
}

static uint64_t parse_size(const char *s)
{
    uint64_t val = (uint64_t)strtoul(s, NULL, 10);
    /* Check suffix */
    const char *p = s;
    while (*p && (*p >= '0' && *p <= '9')) p++;
    if (*p) {
        switch (*p) {
            case 'K': case 'k': val *= 1024ULL; break;
            case 'M': case 'm': val *= 1024ULL * 1024ULL; break;
            case 'G': case 'g': val *= 1024ULL * 1024ULL * 1024ULL; break;
            case 'T': case 't': val *= 1024ULL * 1024ULL * 1024ULL * 1024ULL; break;
            default: break;
        }
    }
    return val;
}

void cmd_lvcreate(const char *args)
{
    if (!args || args[0] == '\0') {
        cmd_lvcreate_usage();
        return;
    }

    char buf[256];
    strncpy(buf, args, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *argv[16];
    int argc = 0;
    char *p = buf;
    while (*p && argc < 16) {
        while (*p == ' ') p++;
        if (*p == '\0') break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) { *p = '\0'; p++; }
    }

    if (argc >= 1 && strcmp(argv[0], "--help") == 0) {
        cmd_lvcreate_usage();
        return;
    }

    uint64_t extents = 0;
    uint64_t size = 0;
    const char *name = NULL;
    const char *vg_name = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            extents = (uint64_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            size = parse_size(argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else {
            vg_name = argv[i];
        }
    }

    if (!name || !vg_name) {
        kprintf("lvcreate: missing required options (-n <name> and <vg_name>)\n");
        return;
    }

    kprintf("lvcreate: creating LV '%s' in VG '%s'\n", name, vg_name);

    if (extents > 0) {
        kprintf("lvcreate:   %llu extents\n", (unsigned long long)extents);
    } else if (size > 0) {
        kprintf("lvcreate:   %llu bytes (%llu sectors)\n",
                (unsigned long long)size,
                (unsigned long long)(size / 512));
    } else {
        kprintf("lvcreate: no size specified (use -l or -L)\n");
        return;
    }

    /* Calculate size in sectors (512-byte sectors) */
    uint64_t sector_count = 0;
    if (extents > 0) {
        /* Each extent is 4 MiB = 8192 sectors (assuming 4MiB extent size) */
        sector_count = extents * 8192;
    } else if (size > 0) {
        sector_count = size / 512;
    }

    if (sector_count == 0) {
        kprintf("lvcreate: invalid size (0 sectors)\n");
        return;
    }

    /* Create the dm-linear device */
    int dm_id = dm_device_create(name, sector_count);
    if (dm_id < 0) {
        kprintf("lvcreate: failed to create dm device '%s': err=%d\n", name, dm_id);
        return;
    }

    /* Build the mapping table: start length linear <vg_name> <offset>
     * For the first LV in a VG, we use the first available chunk at offset 0.
     * In a full LVM implementation, this would consult the VG metadata to
     * find free extents. */
    char table[128];
    int n = snprintf(table, sizeof(table), "0 %llu linear %s 0",
                     (unsigned long long)sector_count, vg_name);
    if (n < 0 || (size_t)n >= sizeof(table)) {
        kprintf("lvcreate: table string too long\n");
        dm_device_remove(dm_id);
        return;
    }

    int ret = dm_table_load(dm_id, table);
    if (ret < 0) {
        kprintf("lvcreate: failed to load dm table for '%s': err=%d\n", name, ret);
        dm_device_remove(dm_id);
        return;
    }

    kprintf("lvcreate: LV '%s' created as dm-%d (%llu sectors, table: %s)\n",
            name, dm_id, (unsigned long long)sector_count, table);
}
