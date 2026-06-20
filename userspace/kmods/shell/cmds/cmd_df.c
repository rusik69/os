/* cmd_df.c — df command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"

void cmd_df(const char *args) { (void)args;
    char buf[2048];
    uint32_t sz = 0;

    if (vfs_read("/proc/mounts", buf, sizeof(buf) - 1, &sz) != 0 || sz == 0) {
        /* Fallback to legacy direct API */
        if (!ata_is_present()) { kprintf("No disk\n"); return; }
        uint32_t total_sectors = ata_get_sectors();
        uint32_t used_inodes, total_inodes, used_blocks, data_start;
        fs_get_usage(&used_inodes, &total_inodes, &used_blocks, &data_start);
        uint32_t avail = total_sectors > (data_start + used_blocks)
                         ? total_sectors - data_start - used_blocks : 0;
        kprintf("Filesystem      Blocks  Used    Avail   Inodes\n");
        kprintf("/dev/hda        %-7lu %-7lu %-7lu %lu/%llu\n",
                (unsigned long)(total_sectors - data_start),
                (unsigned long)used_blocks,
                (unsigned long)avail,
                (unsigned long)used_inodes,
                (uint64_t)total_inodes);
        return;
    }
    buf[sz] = '\0';

    kprintf("Filesystem      Type    Size    Used    Avail   Mounted on\n");

    /* Parse /proc/mounts lines: "device mount_point fstype flags" */
    char *line = buf;
    int printed = 0;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* Skip empty lines */
        if (*line) {
            char device[64] = "", mount_point[64] = "", fstype[32] = "", flags[32] = "";
            /* Manual field parsing (no sscanf available) */
            char tmp[256];
            strncpy(tmp, line, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            char *p = tmp;
            char *tok;
            int field = 0;
            int n = 0;
            while ((tok = strsep(&p, " \t")) != NULL && field < 4) {
                if (*tok == '\0') continue;
                if (field == 0) strncpy(device, tok, sizeof(device) - 1);
                if (field == 1) strncpy(mount_point, tok, sizeof(mount_point) - 1);
                if (field == 2) strncpy(fstype, tok, sizeof(fstype) - 1);
                if (field == 3) strncpy(flags, tok, sizeof(flags) - 1);
                field++;
                n = field;
            }

            if (n >= 2) {
                unsigned long long total_kb = 0, used_kb = 0, free_kb = 0;
                int stat_failed = 0;

                /* Try to stat the mount point for size info using libc_fs_stat */
                uint32_t fsize = 0;
                uint8_t ftype = 0;
                if (fs_stat(mount_point, &fsize, &ftype) == 0) {
                    /* For block devices, try to get size info */
                    if (strcmp(device, "none") != 0 && strcmp(device, "devfs") != 0) {
                        /* Query filesystem usage for this mount */
                        uint32_t used_inodes = 0, total_inodes = 0;
                        uint32_t used_blocks = 0, data_start = 0;
                        if (strcmp(mount_point, "/") == 0 || strcmp(mount_point, "/mnt") == 0) {
                            /* For root filesystem use fs_get_usage */
                            fs_get_usage(&used_inodes, &total_inodes, &used_blocks, &data_start);
                            if (ata_is_present()) {
                                uint32_t total_sectors = ata_get_sectors();
                                total_kb = (total_sectors) / 2;  /* sectors to KB (512 bytes/sector) */
                                free_kb = (total_sectors > (data_start + used_blocks))
                                          ? (total_sectors - data_start - used_blocks) / 2 : 0;
                                used_kb = total_kb - free_kb;
                            }
                        }
                    }
                } else {
                    stat_failed = 1;
                }

                if (stat_failed) {
                    kprintf("%-15s %-7s %s %s\n", device, fstype, "-", mount_point);
                } else if (total_kb > 0) {
                    char size_s[16], used_s[16], free_s[16];
                    snprintf(size_s, sizeof(size_s), "%lluK", (unsigned long long)total_kb);
                    snprintf(used_s, sizeof(used_s), "%lluK", (unsigned long long)used_kb);
                    snprintf(free_s, sizeof(free_s), "%lluK", (unsigned long long)free_kb);
                    kprintf("%-15s %-7s %7s %7s %7s %s\n",
                            device, fstype, size_s, used_s, free_s, mount_point);
                } else {
                    kprintf("%-15s %-7s %-7s %-7s %-7s %s\n",
                            device, fstype, "-", "-", "-", mount_point);
                }
                printed++;
            }
        }

        if (nl)
            line = nl + 1;
        else
            break;
    }

    if (!printed) {
        kprintf("(no mount entries)\n");
    }
}
