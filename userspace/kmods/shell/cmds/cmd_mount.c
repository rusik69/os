/*
 * cmd_mount.c — mount command for /etc/fstab support
 *
 * Usage:
 *   mount              - list currently mounted filesystems
 *   mount -a           - mount all filesystems from /etc/fstab
 *   mount -t fstype -o options device mountpoint  - mount a specific fs
 */

#include "shell.h"
#include "shell_cmd_table.h"
#include "vfs.h"
#include "fstab.h"
#include "printf.h"
#include "string.h"

/* ── Forward declarations for VFS mount table access ────────────────── */
extern int num_mounts;
extern struct vfs_mount mounts[VFS_MAX_MOUNTS];

void cmd_mount(const char *args) {
    /* Simple argument parsing */
    int mount_all = 0;
    const char *fstype = NULL;
    const char *device = NULL;
    const char *mountpoint = NULL;
    const char *options = NULL;

    /* Parse args manually from the string */
    char buf[256];
    char *argv[16];
    int argc = 0;

    if (args) {
        strncpy(buf, args, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char *p = buf;
        while (*p && argc < 16) {
            while (*p == ' ') p++;
            if (*p == '\0') break;
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
        }
    }

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            mount_all = 1;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            fstype = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            options = argv[++i];
        } else if (argv[i][0] == '-') {
            kprintf("mount: unknown option '%s'\n", argv[i]);
            kprintf("Usage: mount [-a] [-t fstype] [-o options] [device] [mountpoint]\n");
            return;
        } else if (!device) {
            device = argv[i];
        } else if (!mountpoint) {
            mountpoint = argv[i];
        }
    }

    /* ── mount -a: mount all from /etc/fstab ───────────────────────── */
    if (mount_all) {
        int count = fstab_mount_all();
        if (count > 0)
            kprintf("mount: %d filesystem(s) mounted from /etc/fstab\n", count);
        else
            kprintf("mount: no entries in /etc/fstab or all already mounted\n");
        return;
    }

    /* ── mount (no args): list current mounts ──────────────────────── */
    if (!device && !mountpoint) {
        kprintf("Mountpoint              Type     Flags   Source\n");
        kprintf("──────────────────────────────────────────────────────\n");

        for (int i = 0; i < num_mounts; i++) {
            kprintf("%-23s ", mounts[i].mountpoint);

            /* Determine type from mount characteristics */
            const char *type = "smfs";
            if (mounts[i].is_bind)
                type = "bind";
            else if (strcmp(mounts[i].mountpoint, "/proc") == 0)
                type = "proc";
            else if (strcmp(mounts[i].mountpoint, "/dev") == 0)
                type = "devfs";
            else if (strcmp(mounts[i].mountpoint, "/dev/shm") == 0)
                type = "tmpfs";

            kprintf("%-8s ", type);

            /* Show flags */
            const char *flagstr = (mounts[i].flags & MS_RDONLY) ? "ro" : "rw";
            if (mounts[i].is_bind)
                flagstr = "rw,bind";

            kprintf("%-10s ", flagstr);

            /* Show source */
            if (mounts[i].is_bind && mounts[i].bind_source[0])
                kprintf("%s", mounts[i].bind_source);
            else
                kprintf("-");

            kprintf("\n");
        }
        if (num_mounts == 0)
            kprintf("(no filesystems mounted)\n");
        return;
    }

    /* ── mount device mountpoint: mount a specific filesystem ──────── */
    if (device && mountpoint) {
        if (!fstype) {
            kprintf("mount: -t <fstype> is required when specifying device and mountpoint\n");
            return;
        }

        struct fstab_entry ent;
        memset(&ent, 0, sizeof(ent));
        strncpy(ent.device, device, FSTAB_PATH_MAX - 1);
        strncpy(ent.mountpoint, mountpoint, FSTAB_PATH_MAX - 1);
        strncpy(ent.fstype, fstype, FSTAB_FSNAME_MAX - 1);
        if (options)
            strncpy(ent.options, options, FSTAB_OPTS_MAX - 1);

        int ret = fstab_mount_entry(&ent);
        if (ret == 0) {
            kprintf("mount: %s mounted at %s\n", device, mountpoint);
        } else {
            kprintf("mount: failed to mount %s at %s (error %d)\n",
                    device, mountpoint, ret);
        }
        return;
    }

    kprintf("Usage: mount [-a] [-t fstype] [-o options] [device] [mountpoint]\n");
}
