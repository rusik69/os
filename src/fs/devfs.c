/*
 * devfs.c — /dev virtual filesystem with dynamic device registration
 *
 * ── Architecture Overview ──────────────────────────────────────────
 *
 * devfs provides a lightweight /dev filesystem that exposes both
 * built-in device nodes (null, zero, random, kmsg) and dynamically
 * registered device nodes from kernel drivers.
 *
 * ── Device Node Management ─────────────────────────────────────────
 *
 * The device node table is a fixed-size array (DEVFS_MAX_DEVICES = 32)
 * of struct devfs_device entries.  Each entry encodes:
 *   .name     — device node name string (max 47 chars, no '/' allowed)
 *   .priv     — opaque pointer passed back to the driver callbacks
 *   .read_fn  — optional read callback; NULL => read returns 0 bytes
 *   .write_fn — optional write callback; NULL => write silently accepted
 *   .in_use   — slot occupancy flag (1 = occupied, 0 = free)
 *
 * ── Device Lifecycle ───────────────────────────────────────────────
 *
 * Registration (devfs_register_device):
 *   1. Validate name (non-NULL, non-empty, ≤ 47 chars, no '/').
 *   2. Scan table for duplicate names (reject if found).
 *   3. Find first free slot (.in_use == 0).
 *   4. Populate slot, set .in_use = 1.
 *   5. Return 0 on success, -1 on full table or duplicate.
 *
 * Lookup (devfs_find_device — internal):
 *   1. Linear scan of the table.
 *   2. Return pointer to matching entry or NULL.
 *
 * Unregistration (devfs_unregister_device):
 *   1. Validate name.
 *   2. Scan table for a matching in-use entry.
 *   3. Clear all fields, set .in_use = 0.
 *   4. Return 0 on success, -1 if not found.
 *
 * ── Built-in vs Dynamic Devices ────────────────────────────────────
 *
 * Four built-in devices are hardcoded in the VFS operations and are
 * NOT stored in the dynamic device table:
 *   /dev/null   — Write sink, read returns empty (EOF)
 *   /dev/zero   — Read returns zero-filled buffers
 *   /dev/random — LCG-based pseudo-random byte source
 *   /dev/kmsg   — Kernel message log access (read dmesg, write to log)
 *
 * Dynamic devices are registered by drivers (e.g. ttyS0, i2c-X) and
 * live in the devfs_devices[] table.  The VFS operations check built-in
 * devices first, then strip the "/dev/" prefix and consult the table.
 *
 * ── Thread Safety ─────────────────────────────────────────────────
 *
 * devfs does NOT use any locking.  The device table is modified only
 * during driver init/exit (before/after SMP is up) or in single-threaded
 * contexts.  Concurrent registration from multiple drivers is not
 * expected in the current design.
 *
 * ── Module Support ──────────────────────────────────────────────────
 *
 * When compiled as a loadable module (MODULE defined), devfs_init() is
 * called from init_module() on insmod, and cleanup_module() on rmmod
 * tears down all dynamic device entries.
 */

#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "types.h"

/* ── Dynamic device table ────────────────────────────────────────── */

#define DEVFS_MAX_DEVICES 32

/** Device entry in the dynamic device table */
struct devfs_device {
    char  name[48];           /* device node name (e.g. "ttyS0") */
    void *priv;               /* private data for driver callbacks */
    int (*read_fn)(void *priv, void *buf, uint32_t max_size, uint32_t *out_size);
    int (*write_fn)(void *priv, const void *data, uint32_t size);
    int   in_use;             /* 1 = slot occupied */
};

static struct devfs_device devfs_devices[DEVFS_MAX_DEVICES];

/* Simple pseudo-random LCG for /dev/random */
static uint32_t rand_state = 0xDEADBEEF;
static uint8_t dev_rand_byte(void) {
    rand_state = rand_state * 1664525u + 1013904223u;
    return (uint8_t)(rand_state >> 16);
}

/* ── Public API for drivers ──────────────────────────────────────── */

/**
 * devfs_register_device - Register a dynamic device node in /dev/
 * @name:      Device node name (e.g. "ttyS0" creates "/dev/ttyS0")
 * @priv:      Private data pointer passed to callbacks
 * @read_fn:   Optional read callback (NULL = read returns 0 bytes)
 * @write_fn:  Optional write callback (NULL = write returns success)
 *
 * Returns: 0 on success, -1 on failure (table full or duplicate name)
 *
 * Drivers call this during their init to make a device node appear
 * under /dev/.  The device persists until devfs_unregister_device()
 * is called or the system reboots.
 */
int devfs_register_device(const char *name, void *priv,
                          int (*read_fn)(void *priv, void *buf,
                                         uint32_t max_size, uint32_t *out_size),
                          int (*write_fn)(void *priv, const void *data,
                                          uint32_t size)) {
    if (!name || !name[0]) return -1;

    /* Reject names with '/' or length > 47 */
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > 47) return -1;
    for (size_t i = 0; i < nlen; i++) {
        if (name[i] == '/') return -1;
    }

    /* Check for duplicate and find free slot */
    int free_slot = -1;
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (devfs_devices[i].in_use) {
            if (strcmp(devfs_devices[i].name, name) == 0)
                return -1; /* duplicate */
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) return -1; /* table full */

    struct devfs_device *d = &devfs_devices[free_slot];
    memcpy(d->name, name, nlen + 1);
    d->priv     = priv;
    d->read_fn  = read_fn;
    d->write_fn = write_fn;
    d->in_use   = 1;
    return 0;
}

/**
 * devfs_unregister_device - Remove a dynamic device node from /dev/
 * @name:  Device node name to remove
 *
 * Returns: 0 on success, -1 if not found.
 * After this call, the device node disappears from the /dev/ listing
 * and all operations on it will return -1.
 */
int devfs_unregister_device(const char *name) {
    if (!name || !name[0]) return -1;

    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (devfs_devices[i].in_use && strcmp(devfs_devices[i].name, name) == 0) {
            devfs_devices[i].in_use = 0;
            memset(devfs_devices[i].name, 0, sizeof(devfs_devices[i].name));
            devfs_devices[i].priv     = NULL;
            devfs_devices[i].read_fn  = NULL;
            devfs_devices[i].write_fn = NULL;
            return 0;
        }
    }
    return -1;
}

/**
 * devfs_find_device - Look up a dynamic device by name
 * @name:  Device node name
 *
 * Returns: pointer to devfs_device entry, or NULL if not found.
 * Internal helper used by the VFS operations below.
 */
static struct devfs_device *devfs_find_device(const char *name) {
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (devfs_devices[i].in_use && strcmp(devfs_devices[i].name, name) == 0)
            return &devfs_devices[i];
    }
    return NULL;
}

/* ── /dev/kmsg helpers ──────────────────────────────────────────── */

/*
 * Parse a Linux /dev/kmsg write buffer of the form "<N>message" where
 * N = facility * 8 + severity.  Returns the parsed severity (0-7), or
 * the default message log level if no valid prefix is found.  Advances
 * *data past the prefix so the caller can log the message text only.
 */
static int kmsg_parse_priority(const char **data, int *out_size) {
    const char *p = *data;
    int remaining = *out_size;

    /* Must start with '<' */
    if (remaining > 0 && *p == '<') {
        p++;
        remaining--;
        int pri = 0;
        /* Collect digits */
        while (remaining > 0 && *p >= '0' && *p <= '9') {
            pri = pri * 10 + (*p - '0');
            p++;
            remaining--;
        }
        /* Must end with '>' */
        if (remaining > 0 && *p == '>') {
            p++;
            remaining--;
            *data = p;
            *out_size = remaining;
            /* Extract severity: pri & 0x07 (lower 3 bits) */
            int sev = pri & 0x07;
            if (sev >= 0 && sev <= 7)
                return sev;
        }
    }
    return default_message_loglevel;
}

/* ── VFS operations ─────────────────────────────────────────────── */

static int devfs_read(void *priv, const char *path, void *buf_v,
                      uint32_t max_size, uint32_t *out_size) {
    (void)priv;
    uint8_t *buf = (uint8_t *)buf_v;

    /* Built-in devices */
    if (strcmp(path, "/dev/null") == 0) {
        if (out_size) *out_size = 0;
        return 0;
    }
    if (strcmp(path, "/dev/zero") == 0) {
        memset(buf, 0, max_size);
        if (out_size) *out_size = max_size;
        return 0;
    }
    if (strcmp(path, "/dev/random") == 0) {
        for (uint32_t i = 0; i < max_size; i++)
            buf[i] = dev_rand_byte();
        if (out_size) *out_size = max_size;
        return 0;
    }
    if (strcmp(path, "/dev/kmsg") == 0) {
        int n = kprintf_dmesg((char *)buf, (int)max_size);
        if (out_size) *out_size = (uint32_t)(n > 0 ? n : 0);
        return 0;
    }

    /* Dynamic devices — strip "/dev/" prefix and look up */
    if (strncmp(path, "/dev/", 5) == 0) {
        const char *devname = path + 5;
        struct devfs_device *d = devfs_find_device(devname);
        if (d) {
            if (d->read_fn)
                return d->read_fn(d->priv, buf, max_size, out_size);
            if (out_size) *out_size = 0;
            return 0;
        }
    }

    return -1;
}

static int devfs_write(void *priv, const char *path, const void *data, uint32_t size) {
    (void)priv;

    /* /dev/null silently accepts all writes */
    if (strcmp(path, "/dev/null") == 0) return 0;

    if (strcmp(path, "/dev/kmsg") == 0) {
        const char *msg = (const char *)data;
        int remaining = (int)size;
        int sev = kmsg_parse_priority(&msg, &remaining);

        /* Trim trailing whitespace/newline */
        while (remaining > 0 && (msg[remaining - 1] == '\n' ||
                                  msg[remaining - 1] == '\r' ||
                                  msg[remaining - 1] == ' '))
            remaining--;

        if (remaining > 0) {
            char stack_buf[256];
            int copy_len = remaining < 255 ? remaining : 255;
            memcpy(stack_buf, msg, (size_t)copy_len);
            stack_buf[copy_len] = '\0';
            kprintf_level(sev, "[kmsg] %s\n", stack_buf);
        }
        return (int)size;
    }

    /* Dynamic devices */
    if (strncmp(path, "/dev/", 5) == 0) {
        const char *devname = path + 5;
        struct devfs_device *d = devfs_find_device(devname);
        if (d) {
            if (d->write_fn)
                return d->write_fn(d->priv, data, size);
            return (int)size; /* no write_fn: silently accept */
        }
    }

    return -1;
}

static int devfs_stat(void *priv, const char *path, struct vfs_stat *st) {
    (void)priv;
    if (strcmp(path, "/dev") == 0) {
        st->type = 2; st->size = 0; return 0;
    }

    /* Built-in devices */
    if (strcmp(path, "/dev/null")   == 0 ||
        strcmp(path, "/dev/zero")   == 0 ||
        strcmp(path, "/dev/random") == 0) {
        st->type = 1; st->size = 0; return 0;
    }
    if (strcmp(path, "/dev/kmsg") == 0) {
        st->type = 1;
        st->size = (uint32_t)kprintf_dmesg_used();
        return 0;
    }

    /* Dynamic devices */
    if (strncmp(path, "/dev/", 5) == 0) {
        const char *devname = path + 5;
        struct devfs_device *d = devfs_find_device(devname);
        if (d) {
            st->type = 1; /* character device */
            st->size = 0;
            st->mode = 0666;
            return 0;
        }
    }

    return -1;
}

static int devfs_readdir(void *priv, const char *path) {
    (void)priv;
    if (strcmp(path, "/dev") != 0) return -1;

    /* Built-in devices */
    kprintf("null\nzero\nrandom\nkmsg\n");

    /* Dynamic devices */
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (devfs_devices[i].in_use) {
            kprintf("%s\n", devfs_devices[i].name);
        }
    }
    return 0;
}

static int devfs_readdir_names(void *priv, const char *path,
                                char names[][64], int max) {
    (void)priv;
    if (strcmp(path, "/dev") != 0) return -1;

    int count = 0;

    /* Built-in devices */
    static const char *const builtins[] = {"null", "zero", "random", "kmsg"};
    int nbuilt = (int)(sizeof(builtins) / sizeof(builtins[0]));

    for (int i = 0; i < nbuilt && count < max; i++) {
        memcpy(names[count], builtins[i], strlen(builtins[i]) + 1);
        count++;
    }

    /* Dynamic devices */
    for (int i = 0; i < DEVFS_MAX_DEVICES && count < max; i++) {
        if (devfs_devices[i].in_use) {
            memcpy(names[count], devfs_devices[i].name,
                   strlen(devfs_devices[i].name) + 1);
            count++;
        }
    }
    return count;
}

/* ── VFS operations table ────────────────────────────── */
/**
 * devfs_ops - VFS operation table for /dev
 *
 * Provides read, write, stat, readdir, and readdir_names callbacks
 * for the /dev mount point.  Built-in devices are handled by string
 * comparison on the full path; dynamic devices are looked up in the
 * device table after stripping the "/dev/" prefix.
 *
 * .create and .unlink are deliberately NULL — /dev does not support
 * creating or deleting device nodes from userspace.  Only drivers
 * can register/unregister via the public API.
 */
struct vfs_ops devfs_ops = {
    .read          = devfs_read,
    .write         = devfs_write,
    .stat          = devfs_stat,
    .create        = NULL,
    .unlink        = NULL,
    .readdir       = devfs_readdir,
    .readdir_names = devfs_readdir_names,
};

/* ── Init / Module support ────────────────────────────────────────── */

/** Track whether devfs has been mounted (prevents double mount) */
static int devfs_mounted = 0;

/**
 * devfs_init - Initialise and mount the /dev device filesystem.
 *
 * Called from the built-in init path.  When built as a loadable module,
 * this function is called from init_module() instead.
 *
 * Mounts /dev with the devfs VFS operations so that built-in device
 * nodes (null, zero, random, kmsg) and dynamically registered device
 * nodes are accessible to userspace.
 */
void __init devfs_init(void) {
    if (devfs_mounted) return;

    /* Clear the dynamic device table */
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        devfs_devices[i].in_use = 0;
        devfs_devices[i].name[0] = '\0';
        devfs_devices[i].priv = NULL;
        devfs_devices[i].read_fn = NULL;
        devfs_devices[i].write_fn = NULL;
    }

    /* Mount /dev with the devfs operations */
    if (vfs_mount("/dev", &devfs_ops, NULL) == 0) {
        kprintf("[OK] devfs mounted on /dev\n");
    } else {
        kprintf("[!!] devfs mount failed\n");
    }

    devfs_mounted = 1;
}
/* devfs_init is called explicitly from kernel_main after vfs_init(),
 * not via the initcall system — fs_initcall would be silently lost
 * because do_initcalls() runs before vfs_init(). */

#ifdef MODULE
#include "module.h"

/* Module entry point — called by the module ELF loader on insmod */
int init_module(void) {
    devfs_init();
    return 0;
}

/* Module exit point — called by the module ELF loader on rmmod */
void __exit cleanup_module(void) {
    if (devfs_mounted) {
        devfs_mounted = 0;
        /* Clear all dynamic device entries */
        for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
            if (devfs_devices[i].in_use) {
                devfs_devices[i].in_use = 0;
                memset(devfs_devices[i].name, 0, sizeof(devfs_devices[i].name));
                devfs_devices[i].priv = NULL;
                devfs_devices[i].read_fn = NULL;
                devfs_devices[i].write_fn = NULL;
            }
        }
        kprintf("[devfs] Module unloaded\n");
    }
}

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("devfs — /dev device virtual filesystem with dynamic device registration");
MODULE_ALIAS("devfs");
#endif /* MODULE */

/* ── devfs_register ───────────────────────────────────── */
/**
 * devfs_register - Register a device node in the devfs table (simple API)
 * @name:   Device node name (e.g. "ttyS0")
 * @major:  Major device number (unused in current implementation)
 * @minor:  Minor device number (unused in current implementation)
 *
 * Legacy helper that registers a device by name only, without custom
 * read/write callbacks or private data.  Used by drivers that only
 * need their device node to appear in /dev/ listings and do not
 * override the default I/O behaviour.
 *
 * Returns: 0 on success, -ENOMEM if the device table is full.
 *
 * NOTE: This function does NOT validate the name string.  Callers
 * must ensure the name is non-NULL, non-empty, ≤ 47 chars, and
 * does not contain '/'.  Prefer devfs_register_device() for new
 * code — it provides proper validation and callback support.
 */
static int devfs_register(const char *name, int major, int minor)
{
    /* Register a device in devfs */
    int slot = -1;
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (!devfs_devices[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -ENOMEM;
    strncpy(devfs_devices[slot].name, name, sizeof(devfs_devices[slot].name) - 1);
    devfs_devices[slot].in_use = 1;
    kprintf("[devfs] Registered device: %s\n", name);
    return 0;
}
/* ── devfs_unregister ─────────────────────────────────── */
/**
 * devfs_unregister - Remove a device node from the devfs table (simple API)
 * @name:  Name of the device node to remove
 *
 * Legacy counter-part of devfs_register().  Finds the device by name,
 * clears its table slot, and logs the removal.
 *
 * Returns: 0 on success, -ENOENT if the device was not found.
 */
static int devfs_unregister(const char *name)
{
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (devfs_devices[i].in_use && strcmp(devfs_devices[i].name, name) == 0) {
            devfs_devices[i].in_use = 0;
            memset(devfs_devices[i].name, 0, sizeof(devfs_devices[i].name));
            devfs_devices[i].read_fn = NULL;
            devfs_devices[i].write_fn = NULL;
            kprintf("[devfs] Unregistered device: %s\n", name);
            return 0;
        }
    }
    kprintf("[devfs] Device not found: %s\n", name);
    return -ENOENT;
}
