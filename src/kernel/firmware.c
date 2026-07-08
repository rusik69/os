#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "firmware.h"
#include "vfs.h"
#include "heap.h"
#include "spinlock.h"
#include "errno.h"
#include "timer.h"
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
        struct fw_cache_entry *ce;
        spinlock_acquire(&fw_lock);
        ce = fw_cache_lookup(name);
        spinlock_release(&fw_lock);

        if (ce) {
            /* Allocate a lightweight firmware descriptor */
            struct firmware *fw = (struct firmware *)kmalloc(sizeof(struct firmware));
            if (!fw)
                return -ENOMEM;
            fw->data = ce->data;
            fw->size = ce->size;
            *fw_ptr = fw;
            return 0;
        }
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
            fw->data = bf->data;
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
        if (ret != 0)
            return ret;

        /* Insert into cache (transfers ownership of disk_data) */
        spinlock_acquire(&fw_lock);
        ret = fw_cache_insert(name, disk_data, disk_size);
        spinlock_release(&fw_lock);

        if (ret != 0) {
            /* Cache insert failed — free data and return error */
            kfree(disk_data);
            return ret;
        }

        /* Allocate firmware descriptor pointing into cache */
        struct fw_cache_entry *ce;
        spinlock_acquire(&fw_lock);
        ce = fw_cache_lookup(name);
        spinlock_release(&fw_lock);

        if (!ce) {
            /* Extremely unlikely after insert, but handle it */
            return -ENOMEM;
        }

        struct firmware *fw = (struct firmware *)kmalloc(sizeof(struct firmware));
        if (!fw)
            return -ENOMEM;
        fw->data = ce->data;
        fw->size = ce->size;
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
     * The firmware data itself is owned by the cache or built-in table.
     * We only free the lightweight descriptor allocated in request_firmware.
     * The cache entry persists for future requests.
     */
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
    else if (fw)
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

    kprintf("[FW] Async firmware request initiated for '%s'\\n", name);
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

/* ── Stub: firmware_request ────────────────────────────────────────── */
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
