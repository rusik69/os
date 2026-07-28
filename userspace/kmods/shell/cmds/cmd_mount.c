/*
 * cmd_mount.c — mount command for /etc/fstab support
 *
 * Usage:
 *   mount              - list currently mounted filesystems
 *   mount -a           - mount all filesystems from /etc/fstab
 *   mount -t fstype -o options device mountpoint  - mount a specific fs
 */

#include "fstab.h"
#include "printf.h"
#include "shell.h"
#include "shell_cmd_table.h"
#include "string.h"
#include "vfs.h"

/* ── Forward declarations for VFS mount table access ────────────────── */
extern int num_mounts;
extern struct vfs_mount mounts[VFS_MAX_MOUNTS];

/* ── Recursive mount listing helpers ──────────────────── */

/* Return non-zero if mounts[parent] is the direct parent of mounts[child] */
static int is_direct_child(int parent, int child) {
    const char *pp = mounts[parent].mountpoint;
    const char *cp = mounts[child].mountpoint;
    size_t plen = strlen(pp);
    size_t clen = strlen(cp);

    if (clen <= plen + 1)
        return 0; /* child must be longer */

    /* Root mount "/" is special */
    if (plen == 1 && pp[0] == '/') {
        if (cp[0] != '/')
            return 0;
        /* Exactly one path component — no '/' beyond the first */
        for (size_t i = 1; cp[i]; i++)
            if (cp[i] == '/')
                return 0;
        return 1;
    }

    /* Non-root: child must start with parent + "/" */
    if (strncmp(cp, pp, plen) != 0)
        return 0;
    if (cp[plen] != '/')
        return 0;

    /* Ensure no further slashes (direct child, not grandchild) */
    for (size_t i = plen + 1; cp[i]; i++)
        if (cp[i] == '/')
            return 0;

    return 1;
}

/* Return a descriptive filesystem-type string for a mount entry */
static const char *mount_type_str(int i) {
    if (mounts[i].is_bind)
        return "bind";
    if (strcmp(mounts[i].mountpoint, "/proc") == 0)
        return "proc";
    if (strcmp(mounts[i].mountpoint, "/dev") == 0)
        return "devfs";
    if (strcmp(mounts[i].mountpoint, "/dev/shm") == 0)
        return "tmpfs";
    return "smfs";
}

/* Recursively print a mount entry and all its children */
static void print_mount_tree(int idx, const char *prefix, int is_last) {
    /* Print tree connector */
    kprintf("%s", prefix);
    kprintf(is_last ? "└── " : "├── ");

    /* Print mountpoint */
    kprintf("%s", mounts[idx].mountpoint);

    /* Print type in brackets */
    kprintf("  [%s]", mount_type_str(idx));

    /* Print flags */
    if (mounts[idx].flags & MS_RDONLY)
        kprintf(" (ro)");
    else
        kprintf(" (rw)");

    /* Print bind source if applicable */
    if (mounts[idx].is_bind && mounts[idx].bind_source[0])
        kprintf("  <- %s", mounts[idx].bind_source);

    kprintf("\n");

    /* Build prefix for children */
    char child_prefix[128];
    snprintf(child_prefix, sizeof(child_prefix), "%s%s", prefix, is_last ? "    " : "│   ");

    /* Collect direct children */
    int children[VFS_MAX_MOUNTS];
    int nchildren = 0;
    for (int i = 0; i < num_mounts; i++) {
        if (i == idx)
            continue;
        if (is_direct_child(idx, i))
            children[nchildren++] = i;
    }

    /* Sort children by mountpoint for deterministic output */
    for (int i = 0; i < nchildren - 1; i++) {
        for (int j = 0; j < nchildren - i - 1; j++) {
            if (strcmp(mounts[children[j]].mountpoint, mounts[children[j + 1]].mountpoint) > 0) {
                int tmp = children[j];
                children[j] = children[j + 1];
                children[j + 1] = tmp;
            }
        }
    }

    /* Print children recursively */
    for (int i = 0; i < nchildren; i++)
        print_mount_tree(children[i], child_prefix, i == nchildren - 1);
}

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
            while (*p == ' ')
                p++;
            if (*p == '\0')
                break;
            argv[argc++] = p;
            while (*p && *p != ' ')
                p++;
            if (*p)
                *p++ = '\0';
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

    /* ── mount (no args): list current mounts recursively ──────────── */
    if (!device && !mountpoint) {
        kprintf("Mount hierarchy:\n");

        /* Find top-level mounts (those without a parent mount) */
        int tops[VFS_MAX_MOUNTS];
        int ntops = 0;

        for (int i = 0; i < num_mounts; i++) {
            int has_parent = 0;
            for (int j = 0; j < num_mounts; j++) {
                if (i == j)
                    continue;
                if (is_direct_child(j, i)) {
                    has_parent = 1;
                    break;
                }
            }
            if (!has_parent)
                tops[ntops++] = i;
        }

        /* Sort top-level mounts by mountpoint for a stable tree */
        for (int i = 0; i < ntops - 1; i++) {
            for (int j = 0; j < ntops - i - 1; j++) {
                if (strcmp(mounts[tops[j]].mountpoint, mounts[tops[j + 1]].mountpoint) > 0) {
                    int tmp = tops[j];
                    tops[j] = tops[j + 1];
                    tops[j + 1] = tmp;
                }
            }
        }

        /* Print tree */
        for (int i = 0; i < ntops; i++)
            print_mount_tree(tops[i], "", i == ntops - 1);

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
            kprintf("mount: failed to mount %s at %s (error %d)\n", device, mountpoint, ret);
        }
        return;
    }

    kprintf("Usage: mount [-a] [-t fstype] [-o options] [device] [mountpoint]\n");
}
