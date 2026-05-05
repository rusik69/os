#include "shell_cmds.h"
#include "fat32.h"
#include "ata.h"
#include "ahci.h"
#include "string.h"
#include "printf.h"

/*
 * fat <subcommand> [args]
 *   fat mount [ata|ahci|usb] - mount FAT32 from the given disk (auto-detect default)
 *   fat ls [path]           - list directory
 *   fat cat <path>          - dump file contents
 *   fat stat <path>         - show file size
 */
void cmd_fat(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: fat mount [ata|ahci|usb] | ls [path] | cat <path> | stat <path>\n");
        kprintf("  mounted: %s\n", fat32_is_mounted() ? "yes" : "no");
        return;
    }

    /* ── mount ── */
    if (strncmp(args, "mount", 5) == 0) {
        fat32_disk_t disk = FAT32_DISK_ATA;
        const char *rest = args + 5;
        while (*rest == ' ') rest++;
        if (strncmp(rest, "ahci", 4) == 0 && ahci_is_present())
            disk = FAT32_DISK_AHCI;
        else if (strncmp(rest, "usb", 3) == 0)
            disk = FAT32_DISK_USB0;
        else if (!ata_is_present() && ahci_is_present())
            disk = FAT32_DISK_AHCI;

        int rc = fat32_mount(disk, 0);
        if (rc == 0)
            kprintf("FAT32 mounted\n");
        else
            kprintf("fat mount failed: %d\n", (uint64_t)(-rc));
        return;
    }

    if (!fat32_is_mounted()) {
        kprintf("FAT32 not mounted (use: fat mount)\n");
        return;
    }

    /* ── ls ── */
    if (strncmp(args, "ls", 2) == 0) {
        const char *path = args + 2;
        while (*path == ' ') path++;
        if (!*path) path = "/";

        char names[64][FAT32_MAX_NAME];
        int n = fat32_list_dir(path, names, 64);
        if (n < 0) {
            kprintf("fat ls: path not found\n");
            return;
        }
        kprintf("%s:\n", path);
        for (int i = 0; i < n; i++)
            kprintf("  %s\n", names[i]);
        if (n == 0) kprintf("  (empty)\n");
        return;
    }

    /* ── cat ── */
    if (strncmp(args, "cat", 3) == 0) {
        const char *path = args + 3;
        while (*path == ' ') path++;
        if (!*path) { kprintf("Usage: fat cat <path>\n"); return; }

        static char buf[4096];
        int n = fat32_read_file(path, buf, sizeof(buf) - 1);
        if (n < 0) { kprintf("fat cat: not found\n"); return; }
        buf[n] = '\0';
        kprintf("%s", buf);
        if (n > 0 && buf[n-1] != '\n') kprintf("\n");
        return;
    }

    /* ── stat ── */
    if (strncmp(args, "stat", 4) == 0) {
        const char *path = args + 4;
        while (*path == ' ') path++;
        if (!*path) { kprintf("Usage: fat stat <path>\n"); return; }
        int sz = fat32_file_size(path);
        if (sz < 0) { kprintf("fat stat: not found\n"); return; }
        kprintf("%s: %d bytes\n", path, (uint64_t)(uint32_t)sz);
        return;
    }

    kprintf("fat: unknown subcommand '%s'\n", args);
}
