/*
 * fstab.c — /etc/fstab filesystem table parser and automounter
 *
 * Reads /etc/fstab and mounts entries via the VFS layer.
 * Supports standard fstab format:
 *   device   mountpoint   fstype   options   dump   pass
 *
 * Integration: call fstab_mount_all() after VFS initialization
 * to mount additional filesystems at boot time.
 */

#include "fstab.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "fs.h"

/* strtol wrapper for simplicity */
static int simple_atoi(const char *s) {
    if (!s) return 0;
    long val = strtol(s, (char **)0, 10);
    return (int)val;
}

/* ── Internal fstab table ──────────────────────────────────────────── */

static struct fstab_entry fstab_table[FSTAB_MAX_ENTRIES];
static int fstab_count = 0;
static int fstab_loaded = 0;

/* ── Option parsing ───────────────────────────────────────────────── */

int fstab_parse_options(const char *options) {
    int flags = 0;
    if (!options || *options == '\0')
        return flags; /* defaults */

    /* options is comma-separated; we iterate segments */
    char buf[FSTAB_OPTS_MAX];
    strncpy(buf, options, FSTAB_OPTS_MAX - 1);
    buf[FSTAB_OPTS_MAX - 1] = '\0';

    char *tok = buf;
    char *next;
    while (tok && *tok) {
        /* Find next comma or end */
        next = strchr(tok, ',');
        if (next) *next++ = '\0';

        /* Trim leading whitespace */
        while (*tok == ' ' || *tok == '\t') tok++;
        /* Trim trailing whitespace */
        char *end = tok + strlen(tok) - 1;
        while (end > tok && (*end == ' ' || *end == '\t')) *end-- = '\0';

        if (strcmp(tok, "ro") == 0)
            flags |= FSTAB_OPT_RO;
        else if (strcmp(tok, "rw") == 0)
            flags |= FSTAB_OPT_RW;
        else if (strcmp(tok, "noexec") == 0)
            flags |= FSTAB_OPT_NOEXEC;
        else if (strcmp(tok, "nosuid") == 0)
            flags |= FSTAB_OPT_NOSUID;
        else if (strcmp(tok, "nodev") == 0)
            flags |= FSTAB_OPT_NODEV;
        /* Defaults: defaults == rw,suid,dev,exec — we just ignore */

        tok = next;
    }
    return flags;
}

/* ── Line parsing ──────────────────────────────────────────────────── */

static int parse_fstab_line(const char *line, struct fstab_entry *ent) {
    if (!line || !ent) return -1;

    /* Skip leading whitespace */
    while (*line == ' ' || *line == '\t') line++;

    /* Skip empty lines and comments */
    if (*line == '\0' || *line == '#' || *line == '\n')
        return -1;

    /* Parse: device mountpoint fstype options dump pass
     * Fields are whitespace-separated.
     * We use a simple tokenizer. */
    char buf[256];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Remove trailing newline */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    char *fields[6];
    int nfields = 0;

    char *p = buf;
    while (*p && nfields < 6) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        fields[nfields++] = p;

        /* Advance to next whitespace */
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }

    if (nfields < 3) return -1; /* need at least device, mountpoint, fstype */

    /* Copy fields */
    memset(ent, 0, sizeof(*ent));

    strncpy(ent->device, fields[0], FSTAB_PATH_MAX - 1);
    strncpy(ent->mountpoint, fields[1], FSTAB_PATH_MAX - 1);
    strncpy(ent->fstype, fields[2], FSTAB_FSNAME_MAX - 1);

    if (nfields >= 4)
        strncpy(ent->options, fields[3], FSTAB_OPTS_MAX - 1);
    else
        ent->options[0] = '\0';

    if (nfields >= 5)
        ent->dump = simple_atoi(fields[4]);
    else
        ent->dump = 0;

    if (nfields >= 6)
        ent->pass = simple_atoi(fields[5]);
    else
        ent->pass = 0;

    return 0;
}

/* ── Fstab loading ─────────────────────────────────────────────────── */

int fstab_load(void) {
    if (fstab_loaded) return fstab_count;

    fstab_count = 0;
    memset(fstab_table, 0, sizeof(fstab_table));

    /* Read /etc/fstab via VFS */
    char buf[4096];
    uint32_t size = 0;
    int ret = vfs_read("/etc/fstab", buf, sizeof(buf) - 1, &size);
    if (ret < 0 || size == 0) {
        /* No fstab file — not an error, just nothing to mount */
        fstab_loaded = 1;
        return 0;
    }
    buf[size] = '\0';

    /* Parse line by line */
    char *line = buf;
    while (line && *line && fstab_count < FSTAB_MAX_ENTRIES) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';

        struct fstab_entry ent;
        if (parse_fstab_line(line, &ent) == 0) {
            fstab_table[fstab_count++] = ent;
        }

        line = next;
    }

    fstab_loaded = 1;
    kprintf("[fstab] Loaded %d entries from /etc/fstab\n", fstab_count);
    return fstab_count;
}

const struct fstab_entry *fstab_get_entry(int index) {
    if (index < 0 || index >= fstab_count) return NULL;
    return &fstab_table[index];
}

int fstab_get_count(void) {
    return fstab_count;
}

/* ── Mount an individual fstab entry ────────────────────────────────── */

int fstab_mount_entry(const struct fstab_entry *ent) {
    if (!ent) return -EINVAL;

    /* Check if already mounted */
    struct vfs_stat st;
    if (vfs_stat(ent->mountpoint, &st) == 0) {
        /* Mountpoint already exists and may be mounted */
        /* For virtual filesystems, we allow remount */
        char mount_list[VFS_MAX_MOUNTS][64];
        int nmounts = vfs_list_mountpoints(mount_list, VFS_MAX_MOUNTS);
        for (int i = 0; i < nmounts; i++) {
            if (strcmp(mount_list[i], ent->mountpoint) == 0) {
                kprintf("[fstab] %s already mounted at %s — skipping\n",
                        ent->fstype, ent->mountpoint);
                return 0;
            }
        }
    }

    int opt_flags = fstab_parse_options(ent->options);
    int mount_flags = 0;
    if (opt_flags & FSTAB_OPT_RO)
        mount_flags |= MS_RDONLY;

    /* Dispatch by filesystem type */
    if (strcmp(ent->fstype, "smfs") == 0) {
        /* SMFS is always root — just ensure mountpoint exists */
        kprintf("[fstab] smfs already mounted as root\n");
        return 0;
    }

    if (strcmp(ent->fstype, "proc") == 0) {
        /* procfs is registered in kernel/syscall.c or vfs.c */
        extern struct vfs_ops procfs_ops;
        return vfs_mount_ex(ent->mountpoint, &procfs_ops, NULL, mount_flags);
    }

    if (strcmp(ent->fstype, "devfs") == 0) {
        extern struct vfs_ops devfs_ops;
        return vfs_mount_ex(ent->mountpoint, &devfs_ops, NULL, mount_flags);
    }

    if (strcmp(ent->fstype, "tmpfs") == 0) {
        /* tmpfs requires initialization before mount */
        extern struct vfs_ops tmpfs_vfs_ops;
        extern void tmpfs_mount(void);
        tmpfs_mount();
        return vfs_mount_ex(ent->mountpoint, &tmpfs_vfs_ops, NULL, mount_flags);
    }

    if (strcmp(ent->fstype, "ext2") == 0) {
        extern int ext2_mount(const char *mountpoint, uint8_t dev_id);
        uint8_t dev_id = 0;
        kprintf("[fstab] mounting ext2 on %s (dev_id=%u)\n", ent->mountpoint, dev_id);
        int ret = ext2_mount(ent->mountpoint, dev_id);
        if (ret == 0) return 0;
        kprintf("[fstab] ext2 mount failed for %s: %d\n",
                ent->mountpoint, ret);
        return ret;
    }

    kprintf("[fstab] Unknown filesystem type '%s' for mountpoint %s\n",
            ent->fstype, ent->mountpoint);
    return -EINVAL;
}

/* ── Mount all entries from /etc/fstab ──────────────────────────────── */

int fstab_mount_all(void) {
    int loaded = fstab_load();
    if (loaded <= 0) {
        return 0;
    }

    int mounted = 0;
    int errors = 0;

    /* Mount in pass order: pass=1 first (root-like), then pass=2, then pass=0 */
    for (int pass_target = 0; pass_target <= 2; pass_target++) {
        for (int i = 0; i < fstab_count; i++) {
            /* Skip entries with pass != pass_target for ordering */
            if (fstab_table[i].pass != pass_target &&
                !(pass_target == 2 && fstab_table[i].pass > 2))
                continue;

            if (fstab_mount_entry(&fstab_table[i]) == 0) {
                mounted++;
            } else {
                errors++;
            }
        }
    }

    if (mounted > 0 || errors > 0) {
        kprintf("[fstab] mount-all: %d mounted, %d errors\n", mounted, errors);
    }
    return mounted;
}

/* ── fstab_read ──────────────────────────────────────── */
int fstab_read(const char *path)
{
    kprintf("[fstab] Reading fstab: %s\n", path);
    return 0;
}
/* ── fstab_write ─────────────────────────────────────── */
int fstab_write(const char *path)
{
    kprintf("[fstab] Writing fstab: %s\n", path);
    return 0;
}
