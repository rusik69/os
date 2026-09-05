#define KERNEL_INTERNAL
#include "firmware.h"

#include "errno.h"
#include "heap.h"
#include "module_compress.h" /* gzip_inflate — compressed firmware support */
#include "printf.h"
#include "spinlock.h"
#include "string.h"
#include "sysfs.h"
#include "timer.h"
#include "types.h"
#include "vfs.h"
#include "workqueue.h"

/* ── Built-in firmware table ─────────────────────────────────────────── */

#define BUILTIN_FW_MAX 16
static struct builtin_fw builtin_fw_table[BUILTIN_FW_MAX];
static int num_builtin_fw = 0;
static spinlock_t fw_lock = SPINLOCK_INIT;

/* ── Firmware cache (LRU, holds recently loaded blobs) ───────────────── */

#define FW_CACHE_SIZE 8
#define FW_CACHE_NAME_LEN 64

/*
 * A single cache entry.  Cache entries hold dynamically allocated copies
 * of firmware data so that repeated requests for the same name don't
 * re-read from disk.
 */
struct fw_cache_entry {
    char   name[FW_CACHE_NAME_LEN];   /* firmware name (key) */
    uint8_t *data;                     /* cached blob data (kmalloc'd) */
    size_t    size;                    /* blob size in bytes */
    uint64_t  last_access;            /* monotonic tick for LRU eviction */
    int       in_use;                  /* 1 = slot occupied */
};

/* Cache entries (statically allocated) */
static struct fw_cache_entry fw_cache[FW_CACHE_SIZE];

/*
 * Monotonic access counter for LRU ordering.
 * Incremented on each cache hit/insert so newer entries have higher stamps.
 */
static uint64_t fw_cache_stamp = 1;

/* ── Forward declarations ────────────────────────────────────────────── */

/* Forward declaration of built-in firmware blobs (if any) */
extern const uint8_t _binary_firmware_start[];
extern const uint8_t _binary_firmware_end[];

/* Internal: try to find firmware in cache */
static struct fw_cache_entry *fw_cache_lookup(const char *name);

/* Internal: insert firmware into cache (LRU eviction if full) */
static int fw_cache_insert(const char *name, const uint8_t *data, size_t size);

/* ── Initialisation ──────────────────────────────────────────────────── */

void __init firmware_init(void)
{
    memset(builtin_fw_table, 0, sizeof(builtin_fw_table));
    num_builtin_fw = 0;
    memset(fw_cache, 0, sizeof(fw_cache));
    fw_cache_stamp = 1;
    kprintf("[OK] firmware: firmware loading API initialized (cache=%d slots)\n",
            FW_CACHE_SIZE);
}

/* ── Built-in firmware registration ──────────────────────────────────── */

int firmware_register_builtin(const char *name, const uint8_t *data, size_t size)
{
    if (!name || !data || size == 0)
        return -EINVAL;

    spinlock_acquire(&fw_lock);

    if (num_builtin_fw >= BUILTIN_FW_MAX) {
        spinlock_release(&fw_lock);
        return -ENOMEM;
    }

    struct builtin_fw *entry = &builtin_fw_table[num_builtin_fw];
    entry->name = name;
    entry->data = data;
    entry->size = size;
    num_builtin_fw++;

    spinlock_release(&fw_lock);
    return 0;
}

/* ── Firmware cache helpers ──────────────────────────────────────────── */

/* Look up a firmware name in the cache. Returns pointer to entry or NULL. */
static struct fw_cache_entry *fw_cache_lookup(const char *name)
{
    if (!name)
        return NULL;

    for (int i = 0; i < FW_CACHE_SIZE; i++) {
        if (fw_cache[i].in_use && strcmp(fw_cache[i].name, name) == 0) {
            /* Update LRU stamp */
            fw_cache[i].last_access = fw_cache_stamp++;
            return &fw_cache[i];
        }
    }
    return NULL;
}

/*
 * Insert a firmware blob into the cache.
 * If the cache is full, the least-recently-used entry is evicted first.
 * The data is NOT copied — ownership of the kmalloc'd buffer is transferred
 * to the cache entry.  Returns 0 on success, -ENOMEM if cache alloc fails.
 */
static int fw_cache_insert(const char *name, const uint8_t *data, size_t size)
{
    if (!name || !data || size == 0)
        return -EINVAL;

    /* Ensure name fits in the cache slot */
    size_t name_len = strlen(name);
    if (name_len >= FW_CACHE_NAME_LEN)
        name_len = FW_CACHE_NAME_LEN - 1;

    int target = -1;
    uint64_t lru_stamp = (uint64_t)-1;

    /* Look for a free slot or the LRU victim */
    for (int i = 0; i < FW_CACHE_SIZE; i++) {
        if (!fw_cache[i].in_use) {
            target = i;
            break;
        }
        if (fw_cache[i].last_access < lru_stamp) {
            lru_stamp = fw_cache[i].last_access;
            target = i;
        }
    }

    if (target < 0)
        return -ENOMEM; /* shouldn't happen with the loop above */

    /* If evicting a live entry, free its data first */
    if (fw_cache[target].in_use && fw_cache[target].data) {
        kfree(fw_cache[target].data);
        fw_cache[target].data = NULL;
    }

    /* Populate the slot */
    memset(fw_cache[target].name, 0, FW_CACHE_NAME_LEN);
    memcpy(fw_cache[target].name, name, name_len);
    fw_cache[target].name[name_len] = '\0';

    /* We keep a pointer to the caller's data — they transfer ownership */
    fw_cache[target].data = (uint8_t *)(uintptr_t)data;
    fw_cache[target].size = size;
    fw_cache[target].last_access = fw_cache_stamp++;
    fw_cache[target].in_use = 1;

    return 0;
}

/* ── Core firmware loading logic ─────────────────────────────────────── */

/*
 * Internal: load firmware from /lib/firmware/<name> via VFS.
 * Returns 0 on success with *out_data pointing to a kmalloc'd buffer
 * that the caller owns.
 */
static int fw_load_from_disk(const char *name, uint8_t **out_data, size_t *out_size)
{
    char path[128];
    int ret = snprintf(path, sizeof(path), "/lib/firmware/%s", name);
    if (ret < 0 || ret >= (int)sizeof(path))
        return -ENAMETOOLONG;

    /* Stat the file first to get size */
    struct vfs_stat st;
    ret = vfs_stat(path, &st);
    if (ret != 0)
        return -ENOENT;

    size_t fw_size = st.size;
    if (fw_size == 0)
        return -ENODATA;

    /* Allocate memory for firmware data */
    uint8_t *fw_data = (uint8_t *)kmalloc(fw_size);
    if (!fw_data)
        return -ENOMEM;

    /* Read the file */
    uint32_t bytes_read = 0;
    ret = vfs_read(path, fw_data, (uint32_t)fw_size, &bytes_read);
    if (ret != 0 || bytes_read != fw_size) {
        kfree(fw_data);
        return -EIO;
    }

    /* Compressed firmware support: a gzip wrapper whose first two bytes are
     * the gzip magic (0x1f 0x8b).  Reuse the kernel's gzip inflater; the
     * uncompressed size is the little-endian ISIZE trailer (the last 4 bytes
     * of the stream, mod 2^32).  This lets firmware be shipped compressed on
     * disk and transparently decompressed on load. */
    if (fw_size >= 18 && fw_data[0] == 0x1f && fw_data[1] == 0x8b) {
        uint64_t isize = (uint64_t)fw_data[fw_size - 4] | ((uint64_t)fw_data[fw_size - 3] << 8) |
                         ((uint64_t)fw_data[fw_size - 2] << 16) |
                         ((uint64_t)fw_data[fw_size - 1] << 24);
        if (isize == 0 || isize > (1ULL << 24)) {
            /* ISIZE of 0 or an implausibly large value (16 MB cap) suggests a
             * corrupt or non-gzip stream — reject rather than over-allocate. */
            kfree(fw_data);
            return -EBADF;
        }

        uint8_t *plain = (uint8_t *)kmalloc(isize);
        if (!plain) {
            kfree(fw_data);
            return -ENOMEM;
        }

        uint64_t plain_size = 0;
        int dret = gzip_inflate(fw_data, fw_size, plain, isize, &plain_size);
        if (dret != 0 || plain_size != isize) {
            kfree(fw_data);
            kfree(plain);
            return -EBADF;
        }
        kfree(fw_data);
        fw_data = plain;
        fw_size = (size_t)plain_size;
        kprintf("[FW] decompressed '%s' (%llu->%llu bytes)\n", name, (unsigned long long)bytes_read,
                (unsigned long long)plain_size);
    }

    *out_data = fw_data;
    *out_size = fw_size;
    return 0;
}

/*
 * Internal: check if name matches a built-in firmware entry.
 * Returns pointer to the builtin entry or NULL.
 */
static const struct builtin_fw *fw_find_builtin(const char *name)
{
    for (int i = 0; i < num_builtin_fw; i++) {
        if (strcmp(builtin_fw_table[i].name, name) == 0)
            return &builtin_fw_table[i];
    }
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int request_firmware(const struct firmware **fw_ptr, const char *name)
{
    if (!fw_ptr || !name)
        return -EINVAL;

    /* Initialize output */
    *fw_ptr = NULL;

    /* 1. Check cache first (fastest path) */
    {
        /* The lookup and the copy of the cached blob must happen under
         * fw_lock: a concurrent LRU eviction (fw_cache_insert) or
         * firmware_cache_flush() can kfree() the entry's data as soon
         * as the lock is dropped, so reading ce->data/ce->size outside
         * the lock is a use-after-free race. */
        struct fw_cache_entry *ce;
        spinlock_acquire(&fw_lock);
        ce = fw_cache_lookup(name);
        if (ce) {
            /* Allocate a lightweight firmware descriptor with a
             * dedicated copy of the data.  The caller owns the copy,
             * so the cache can evict/swap independently without
             * causing use-after-free in any outstanding references. */
            struct firmware *fw = (struct firmware *)kmalloc(sizeof(struct firmware));
            if (!fw) {
                spinlock_release(&fw_lock);
                return -ENOMEM;
            }
            uint8_t *copy = (uint8_t *)kmalloc(ce->size);
            if (!copy) {
                kfree(fw);
                spinlock_release(&fw_lock);
                return -ENOMEM;
            }
            memcpy(copy, ce->data, ce->size);
            fw->data = copy;
            fw->size = ce->size;
            spinlock_release(&fw_lock);
            *fw_ptr = fw;
            return 0;
        }
        spinlock_release(&fw_lock);
    }

    /* 2. Check built-in table (no need to cache — it's already in ROM) */
    {
        const struct builtin_fw *bf;
        spinlock_acquire(&fw_lock);
        bf = fw_find_builtin(name);
        spinlock_release(&fw_lock);

        if (bf) {
            struct firmware *fw = (struct firmware *)kmalloc(sizeof(struct firmware));
            if (!fw)
                return -ENOMEM;
            /* Make a dedicated copy just like the other paths:
             * release_firmware() always kfree()s fw->data, so handing
             * out a pointer into the static builtin table would corrupt
             * the heap on release. */
            uint8_t *copy = (uint8_t *)kmalloc(bf->size);
            if (!copy) {
                kfree(fw);
                return -ENOMEM;
            }
            memcpy(copy, bf->data, bf->size);
            fw->data = copy;
            fw->size = bf->size;
            *fw_ptr = fw;
            return 0;
        }
    }

    /* 3. Load from disk and cache */
    {
        uint8_t *disk_data = NULL;
        size_t   disk_size = 0;
        int ret = fw_load_from_disk(name, &disk_data, &disk_size);
        if (ret != 0) {
            /* Disk lookup missed — fall back to a userspace helper.  Expose
             * /sys/class/firmware/<name>/ (loading + data) so a userspace
             * helper can supply the blob via the sysfs upload ABI.  The
             * upload, when committed, drops the blob into the firmware cache
             * — so a later request_firmware() for the same name (or a retry
             * by the caller) serves it from the cache lookup at step 1
             * without re-touching the filesystem. */
            firmware_sysfs_register(name);
            kprintf("[FW] '%s' not in cache/builtin/disk; userspace-helper "
                    "upload available via /sys/class/firmware/%s/{loading,data}\n",
                    name, name);
            return ret;
        }

        /* Insert into cache (transfers ownership of disk_data) */
        spinlock_acquire(&fw_lock);
        ret = fw_cache_insert(name, disk_data, disk_size);
        spinlock_release(&fw_lock);

        if (ret != 0) {
            /* Cache insert failed — free data and return error */
            kfree(disk_data);
            return ret;
        }

        /* Give the caller a dedicated copy of the data so cache
         * eviction does not invalidate their pointer. */
        uint8_t *caller_copy = (uint8_t *)kmalloc(disk_size);
        if (!caller_copy)
            return -ENOMEM;
        memcpy(caller_copy, disk_data, disk_size);

        struct firmware *fw = (struct firmware *)kmalloc(sizeof(struct firmware));
        if (!fw) {
            kfree(caller_copy);
            return -ENOMEM;
        }
        fw->data = caller_copy;
        fw->size = disk_size;
        *fw_ptr = fw;

        kprintf("[FW] loaded '%s' (%llu bytes, cached)\n", name, (unsigned long long)disk_size);
        return 0;
    }
}

void release_firmware(const struct firmware *fw)
{
    if (!fw)
        return;

    /*
     * The firmware data is a dedicated copy allocated in
     * request_firmware() — free both the data and the descriptor.
     */
    if (fw->data)
        kfree((void *)(uintptr_t)fw->data);
    kfree((void *)(uintptr_t)fw);
}

/* ── Async firmware loading ─────────────────────────────────────────── */

struct async_fw_work {
    const struct firmware **fw_ptr;
    char   name[64];
    firmware_cont_t cont;
    void  *context;
};

static void async_fw_work_handler(void *arg)
{
    struct async_fw_work *aw = (struct async_fw_work *)arg;
    if (!aw) return;

    const struct firmware *fw = NULL;
    int ret = request_firmware(&fw, aw->name);

    (void)ret;

    if (aw->fw_ptr)
        *aw->fw_ptr = fw;

    if (aw->cont)
        aw->cont(fw, aw->context);
    else if (fw && !aw->fw_ptr)
        /* Only auto-release when nobody can consume the result: if the
         * caller supplied fw_ptr, the descriptor lives on in *fw_ptr
         * and the caller owns it (release_firmware), so freeing it
         * here would leave a dangling pointer. */
        release_firmware(fw);

    kfree(aw);
}

int request_firmware_nowait(const struct firmware **fw_ptr, const char *name,
                             firmware_cont_t cont, void *context)
{
    if (!name || !name[0])
        return -EINVAL;

    struct async_fw_work *aw = (struct async_fw_work *)kmalloc(sizeof(*aw));
    if (!aw)
        return -ENOMEM;

    aw->fw_ptr = fw_ptr;
    strncpy(aw->name, name, sizeof(aw->name) - 1);
    aw->name[sizeof(aw->name) - 1] = '\0';
    aw->cont = cont;
    aw->context = context;

    int ret = workqueue_schedule(async_fw_work_handler, aw);
    if (ret < 0) {
        kfree(aw);
        return -EAGAIN;
    }

    kprintf("[FW] Async firmware request initiated for '%s'\n", name);
    return 0;
}

int firmware_load(const char *name, struct firmware *fw)
{
    if (!fw)
        return -EINVAL;

    const struct firmware *rfw = NULL;
    int ret = request_firmware(&rfw, name);
    if (ret != 0) {
        fw->data = NULL;
        fw->size = 0;
        return ret;
    }

    /*
     * The legacy API returns a direct pointer to the data.
     * We kmalloc a copy so the caller owns it and can free it
     * independently of the cache.
     */
    if (rfw->size > 0) {
        uint8_t *copy = (uint8_t *)kmalloc(rfw->size);
        if (!copy) {
            release_firmware(rfw);
            fw->data = NULL;
            fw->size = 0;
            return -ENOMEM;
        }
        memcpy(copy, rfw->data, rfw->size);
        fw->data = copy;
        fw->size = rfw->size;
    } else {
        fw->data = NULL;
        fw->size = 0;
    }

    release_firmware(rfw);
    return 0;
}

void firmware_release(struct firmware *fw)
{
    if (!fw)
        return;

    /*
     * The legacy API returns a kmalloc'd copy (see firmware_load above),
     * so we always free it here.
     */
    if (fw->data) {
        kfree((void *)(uintptr_t)fw->data);
    }

    fw->data = NULL;
    fw->size = 0;
}

int firmware_cache_flush(void)
{
    int flushed = 0;
    spinlock_acquire(&fw_lock);

    for (int i = 0; i < FW_CACHE_SIZE; i++) {
        if (fw_cache[i].in_use && fw_cache[i].data) {
            kfree(fw_cache[i].data);
            fw_cache[i].data = NULL;
            memset(fw_cache[i].name, 0, FW_CACHE_NAME_LEN);
            fw_cache[i].size = 0;
            fw_cache[i].in_use = 0;
            flushed++;
        }
    }

    spinlock_release(&fw_lock);
    return flushed;
}

/* ── Firmware loader sysfs interface ─────────────────────────────────
 *
 * Exposes the Linux firmware-class ABI under /sys/class/firmware/:
 *
 *   /sys/class/firmware/<name>/loading   — write "1" to begin an upload,
 *                                          "0" to commit it, "-1" to abort
 *   /sys/class/firmware/<name>/data       — write the firmware blob bytes
 *
 * This lets environment (or a userspace helper) provide blobs that the
 * kernel firmware loader then returns from request_firmware() — used when
 * a device's firmware is not embedded and not present on the firmware
 * filesystem, mirroring Linux's CONFIG_FW_LOADER_USER_HELPER fallback.
 */

#define FIRMWARE_UPLOAD_MAX 4
#define FIRMWARE_UPLOAD_NAME_LEN 48

/* Per-upload dynamic sysfs state. */
struct firmware_upload {
    char name[FIRMWARE_UPLOAD_NAME_LEN];
    uint8_t *buf; /* accumulated data (kmalloc'd) */
    size_t len;   /* bytes accumulated so far */
    size_t cap;   /* allocated capacity */
    int in_progress;
    int in_use;
};

static struct firmware_upload g_fw_uploads[FIRMWARE_UPLOAD_MAX];

/* Find the upload slot for a given firmware name, or NULL. */
static struct firmware_upload *fw_upload_lookup(const char *name) {
    for (int i = 0; i < FIRMWARE_UPLOAD_MAX; i++) {
        if (g_fw_uploads[i].in_use && strcmp(g_fw_uploads[i].name, name) == 0)
            return &g_fw_uploads[i];
    }
    return NULL;
}

/*
 * Commit a fully-uploaded blob into the firmware cache so that
 * request_firmware(name) subsequently serves it.  A copy is made so the
 * upload buffer can be released independently of the cache.
 */
static int firmware_sysfs_commit(struct firmware_upload *u) {
    if (!u || !u->buf || u->len == 0)
        return -EINVAL;

    uint8_t *copy = (uint8_t *)kmalloc(u->len);
    if (!copy)
        return -ENOMEM;
    memcpy(copy, u->buf, u->len);

    spinlock_acquire(&fw_lock);
    int ret = fw_cache_insert(u->name, copy, u->len);
    spinlock_release(&fw_lock);
    if (ret != 0)
        kfree(copy);
    return ret;
}

/* Reset (or allocate) an upload's accumulation buffer. */
static int fw_upload_reset(struct firmware_upload *u) {
    if (u->buf) {
        kfree(u->buf);
        u->buf = NULL;
        u->len = 0;
        u->cap = 0;
    }
    /* Start with a modest buffer; grow as writes come in. */
    u->cap = 512;
    u->buf = (uint8_t *)kmalloc(u->cap);
    if (!u->buf) {
        u->cap = 0;
        return -ENOMEM;
    }
    u->len = 0;
    return 0;
}

/* Append bytes to an upload's buffer. */
static int fw_upload_append(struct firmware_upload *u, const char *data, uint32_t size) {
    if (!u || !data || size == 0)
        return -EINVAL;

    if (u->len + size > u->cap) {
        size_t newcap = u->cap ? u->cap : 512;
        while (newcap < u->len + size)
            newcap *= 2;
        uint8_t *nbuf = (uint8_t *)kmalloc(newcap);
        if (!nbuf)
            return -ENOMEM;
        if (u->len)
            memcpy(nbuf, u->buf, u->len);
        if (u->buf)
            kfree(u->buf);
        u->buf = nbuf;
        u->cap = newcap;
    }
    memcpy(u->buf + u->len, data, size);
    u->len += size;
    return 0;
}

/* write callback for .../loading */
static int firmware_loading_write(const char *data, uint32_t size, void *priv) {
    struct firmware_upload *u = (struct firmware_upload *)priv;
    /* Interpret the leading character: '1' begin, '0' commit, else abort. */
    char marker = (size > 0) ? data[0] : '0';

    if (marker == '1') {
        u->in_progress = 1;
        return fw_upload_reset(u);
    }
    if (marker == '0') {
        u->in_progress = 0;
        int ret = firmware_sysfs_commit(u);
        /* Release the upload buffer after commit. */
        if (u->buf) {
            kfree(u->buf);
            u->buf = NULL;
            u->len = 0;
            u->cap = 0;
        }
        return ret;
    }
    /* Anything else aborts the upload. */
    if (u->buf) {
        kfree(u->buf);
        u->buf = NULL;
        u->len = 0;
        u->cap = 0;
    }
    u->in_progress = 0;
    return 0;
}

/* write callback for .../data */
static int firmware_data_write(const char *data, uint32_t size, void *priv) {
    struct firmware_upload *u = (struct firmware_upload *)priv;
    if (!u->in_progress)
        return 0; /* ignore writes unless a load was requested */
    return fw_upload_append(u, data, size);
}

/*
 * Register a firmware name for sysfs-driven loading.
 * Creates /sys/class/firmware/<name>/ with loading and data files.
 * Returns 0 on success, negative on error.
 */
int firmware_sysfs_register(const char *name) {
    if (!name || name[0] == '\0')
        return -EINVAL;

    /* Re-registering an existing name is a no-op. */
    if (fw_upload_lookup(name))
        return 0;

    struct firmware_upload *u = NULL;
    for (int i = 0; i < FIRMWARE_UPLOAD_MAX; i++) {
        if (!g_fw_uploads[i].in_use) {
            u = &g_fw_uploads[i];
            break;
        }
    }
    if (!u)
        return -ENOMEM;

    memset(u, 0, sizeof(*u));
    snprintf(u->name, sizeof(u->name), "%s", name);
    u->in_use = 1;

    /* Create the per-firmware directory. */
    char dirpath[128];
    snprintf(dirpath, sizeof(dirpath), "/sys/class/firmware/%s", name);
    if (sysfs_create_dir(dirpath) < 0) {
        u->in_use = 0;
        return -EEXIST;
    }

    char path[160];
    snprintf(path, sizeof(path), "%s/loading", dirpath);
    sysfs_create_writable_file(path, "0", u, NULL, firmware_loading_write);

    snprintf(path, sizeof(path), "%s/data", dirpath);
    sysfs_create_writable_file(path, "", u, NULL, firmware_data_write);

    return 0;
}

/* Create the /sys/class/firmware/ container (idempotent). */
void firmware_sysfs_init(void) {
    sysfs_create_dir("/sys/class/firmware");
}

/* ── Firmware flush ─────────────────────────────────────────────── */
int firmware_request(struct firmware **fw, const char *name, void *device)
{
    (void)fw; (void)name; (void)device;
    kprintf("[FIRMWARE] firmware_request: not yet implemented\n");
    return 0;
}

/* ── Stub: firmware_request_nowarn ─────────────────────────────────── */
static int firmware_request_nowarn(struct firmware **fw, const char *name, void *device)
{
    (void)fw; (void)name; (void)device;
    kprintf("[FIRMWARE] firmware_request_nowarn: not yet implemented\n");
    return 0;
}

/* ── Stub: firmware_request_direct ─────────────────────────────────── */
static int firmware_request_direct(struct firmware **fw, const char *name)
{
    (void)fw; (void)name;
    kprintf("[FIRMWARE] firmware_request_direct: not yet implemented\n");
    return 0;
}

/* ── Stub: firmware_send ───────────────────────────────────────────── */
static int firmware_send(const void *data, size_t size)
{
    (void)data; (void)size;
    kprintf("[FIRMWARE] firmware_send: not yet implemented\n");
    return 0;
}

/* ── Stub: firmware_free ───────────────────────────────────────────── */
static void firmware_free(struct firmware *fw)
{
    (void)fw;
    kprintf("[FIRMWARE] firmware_free: not yet implemented\n");
}
