/*
 * bpf_maps.c — eBPF Maps (array, hash, perf)
 *
 * Implements the three standard eBPF map types:
 *   BPF_MAP_TYPE_ARRAY       — Fixed-size array, pre-allocated
 *   BPF_MAP_TYPE_HASH        — Hash table with dynamic entries
 *   BPF_MAP_TYPE_PERF_EVENT_ARRAY — Per-CPU perf event array
 */

#define KERNEL_INTERNAL
#include "bpf_maps.h"
#include "string.h"
#include "printf.h"
#include "pmm.h"
#include "errno.h"
#include "spinlock.h"
#include "types.h"

#define BPF_MAP_MAX  64

struct bpf_hash_entry {
    struct bpf_hash_entry *next;
    uint64_t              hash;
    uint32_t              key_size;
    uint8_t               kv[];  /* key followed by value */
};

struct bpf_map {
    int      in_use;
    int      type;
    uint32_t key_size;
    uint32_t value_size;
    uint32_t max_entries;
    uint32_t flags;
    union {
        struct {
            uint64_t page_frame;  /* first frame of pre-allocated array */
            void     *data;
            uint32_t  entry_size;
        } array;
        struct {
            struct bpf_hash_entry **buckets;
            uint32_t               nbuckets;
            uint32_t               count;
            spinlock_t             lock;
        } hash;
        struct {
            int      *fds;
            uint32_t  ncpus;
        } perf;
    };
};

static struct bpf_map g_maps[BPF_MAP_MAX];
static spinlock_t g_maps_lock;

static uint64_t bpf_hash_key(const void *key, uint32_t key_size)
{
    uint64_t h = 0;
    const uint8_t *p = (const uint8_t *)key;
    for (uint32_t i = 0; i < key_size; i++) {
        h += p[i];
        h += (h << 10);
        h ^= (h >> 6);
    }
    h += (h << 3);
    h ^= (h >> 11);
    h += (h << 15);
    return h;
}

int bpf_map_create(int map_type, uint32_t key_size, uint32_t value_size,
                   uint32_t max_entries, uint32_t map_flags)
{
    if (map_type < 1 || map_type > 3) return -EINVAL;
    if (max_entries == 0 || max_entries > 1024) return -EINVAL;
    if (key_size > 64 || value_size > 1024) return -EINVAL;

    spinlock_acquire(&g_maps_lock);
    int fd = -1;
    for (int i = 0; i < BPF_MAP_MAX; i++) {
        if (!g_maps[i].in_use) { fd = i + 1; break; }
    }
    if (fd < 0) { spinlock_release(&g_maps_lock); return -ENOSPC; }

    int idx = fd - 1;
    struct bpf_map *map = &g_maps[idx];
    memset(map, 0, sizeof(*map));
    map->in_use = 1;
    map->type = map_type;
    map->key_size = key_size;
    map->value_size = value_size;
    map->max_entries = max_entries;
    map->flags = map_flags;

    switch (map_type) {
    case 2: { /* ARRAY */
        map->array.entry_size = value_size;
        uint32_t total = max_entries * value_size;
        uint32_t pages = (total + 4095) / 4096;
        uint64_t frame = pmm_alloc_frame();
        for (uint32_t i = 1; frame && i < pages; i++) {
            uint64_t f = pmm_alloc_frame();
            if (!f) { /* partial cleanup */ break; }
        }
        if (!frame) {
            memset(map, 0, sizeof(*map));
            spinlock_release(&g_maps_lock);
            return -ENOMEM;
        }
        map->array.page_frame = frame;
        map->array.data = PHYS_TO_VIRT((void *)(uintptr_t)(frame * 4096));
        memset(map->array.data, 0, total);
        break;
    }
    case 1: { /* HASH */
        map->hash.nbuckets = max_entries < 16 ? 16 : max_entries;
        uint32_t bsize = map->hash.nbuckets * sizeof(void *);
        uint32_t pages = (bsize + 4095) / 4096;
        uint64_t frame = pmm_alloc_frame();
        if (!frame) {
            memset(map, 0, sizeof(*map));
            spinlock_release(&g_maps_lock);
            return -ENOMEM;
        }
        map->hash.buckets = (struct bpf_hash_entry **)
            PHYS_TO_VIRT((void *)(uintptr_t)(frame * 4096));
        memset(map->hash.buckets, 0, bsize);
        spinlock_init(&map->hash.lock);
        break;
    }
    case 3: { /* PERF_EVENT_ARRAY */
        map->perf.ncpus = 1;
        uint32_t fds_size = sizeof(int) * map->perf.ncpus;
        uint64_t frame = pmm_alloc_frame();
        if (!frame) {
            memset(map, 0, sizeof(*map));
            spinlock_release(&g_maps_lock);
            return -ENOMEM;
        }
        map->perf.fds = (int *)PHYS_TO_VIRT((void *)(uintptr_t)(frame * 4096));
        memset(map->perf.fds, 0xFF, fds_size);
        break;
    }
    }

    spinlock_release(&g_maps_lock);
    kprintf("[BPF] Created map fd=%d type=%d key=%u val=%u max=%u\n",
            fd, map_type, key_size, value_size, max_entries);
    return fd;
}

int bpf_map_lookup_elem(int fd, const void *key, void *value)
{
    if (fd < 1 || fd > BPF_MAP_MAX) return -EBADF;
    struct bpf_map *map = &g_maps[fd - 1];
    if (!map->in_use) return -EBADF;

    switch (map->type) {
    case 2: { /* ARRAY */
        uint32_t k = *(const uint32_t *)key;
        if (k >= map->max_entries) return -E2BIG;
        memcpy(value, (uint8_t *)map->array.data + (uint64_t)k * map->value_size, map->value_size);
        return 0;
    }
    case 1: { /* HASH */
        spinlock_acquire(&map->hash.lock);
        uint64_t h = bpf_hash_key(key, map->key_size);
        uint32_t bucket = h % map->hash.nbuckets;
        for (struct bpf_hash_entry *e = map->hash.buckets[bucket]; e; e = e->next) {
            if (e->hash == h && e->key_size == map->key_size && memcmp(e->kv, key, map->key_size) == 0) {
                memcpy(value, e->kv + map->key_size, map->value_size);
                spinlock_release(&map->hash.lock);
                return 0;
            }
        }
        spinlock_release(&map->hash.lock);
        return -ENOENT;
    }
    }
    return -EINVAL;
}

int bpf_map_update_elem(int fd, const void *key, const void *value, uint64_t flags)
{
    if (fd < 1 || fd > BPF_MAP_MAX) return -EBADF;
    struct bpf_map *map = &g_maps[fd - 1];
    if (!map->in_use) return -EBADF;

    switch (map->type) {
    case 2: { /* ARRAY */
        uint32_t k = *(const uint32_t *)key;
        if (k >= map->max_entries) return -E2BIG;
        memcpy((uint8_t *)map->array.data + (uint64_t)k * map->value_size, value, map->value_size);
        return 0;
    }
    case 1: { /* HASH */
        spinlock_acquire(&map->hash.lock);
        uint64_t h = bpf_hash_key(key, map->key_size);
        uint32_t bucket = h % map->hash.nbuckets;

        for (struct bpf_hash_entry *e = map->hash.buckets[bucket]; e; e = e->next) {
            if (e->hash == h && e->key_size == map->key_size && memcmp(e->kv, key, map->key_size) == 0) {
                if (flags == 1) { spinlock_release(&map->hash.lock); return -EEXIST; }
                memcpy(e->kv + map->key_size, value, map->value_size);
                spinlock_release(&map->hash.lock);
                return 0;
            }
        }
        if (flags == 2) { spinlock_release(&map->hash.lock); return -ENOENT; }
        if (map->hash.count >= map->max_entries) {
            spinlock_release(&map->hash.lock);
            return -E2BIG;
        }
        /* Allocate entry using pmm_alloc_frame */
        uint64_t frame = pmm_alloc_frame();
        if (!frame) { spinlock_release(&map->hash.lock); return -ENOMEM; }
        struct bpf_hash_entry *e = (struct bpf_hash_entry *)
            PHYS_TO_VIRT((void *)(uintptr_t)(frame * 4096));
        uint32_t entry_size = sizeof(struct bpf_hash_entry) + map->key_size + map->value_size;
        memset(e, 0, entry_size);
        e->hash = h;
        e->key_size = map->key_size;
        memcpy(e->kv, key, map->key_size);
        memcpy(e->kv + map->key_size, value, map->value_size);
        e->next = map->hash.buckets[bucket];
        map->hash.buckets[bucket] = e;
        map->hash.count++;
        spinlock_release(&map->hash.lock);
        return 0;
    }
    }
    return -EINVAL;
}

int bpf_map_delete_elem(int fd, const void *key)
{
    if (fd < 1 || fd > BPF_MAP_MAX) return -EBADF;
    struct bpf_map *map = &g_maps[fd - 1];
    if (!map->in_use) return -EBADF;
    if (map->type != 1) return -EINVAL;

    spinlock_acquire(&map->hash.lock);
    uint64_t h = bpf_hash_key(key, map->key_size);
    uint32_t bucket = h % map->hash.nbuckets;
    for (struct bpf_hash_entry *e = map->hash.buckets[bucket], **prev = &map->hash.buckets[bucket];
         e; prev = &e->next, e = e->next) {
        if (e->hash == h && e->key_size == map->key_size && memcmp(e->kv, key, map->key_size) == 0) {
            *prev = e->next;
            map->hash.count--;
            spinlock_release(&map->hash.lock);
            return 0;
        }
    }
    spinlock_release(&map->hash.lock);
    return -ENOENT;
}

int bpf_map_get_next_key(int fd, const void *key, void *next_key)
{
    if (fd < 1 || fd > BPF_MAP_MAX) return -EBADF;
    struct bpf_map *map = &g_maps[fd - 1];
    if (!map->in_use) return -EBADF;

    switch (map->type) {
    case 2: {
        uint32_t k = key ? *(const uint32_t *)key : (uint32_t)-1;
        k++;
        if (k >= map->max_entries) return -ENOENT;
        *(uint32_t *)next_key = k;
        return 0;
    }
    case 1: {
        spinlock_acquire(&map->hash.lock);
        int found = (key == NULL);
        for (uint32_t b = 0; b < map->hash.nbuckets; b++) {
            for (struct bpf_hash_entry *e = map->hash.buckets[b]; e; e = e->next) {
                if (found) {
                    memcpy(next_key, e->kv, map->key_size);
                    spinlock_release(&map->hash.lock);
                    return 0;
                }
                if (key && memcmp(e->kv, key, map->key_size) == 0)
                    found = 1;
            }
        }
        spinlock_release(&map->hash.lock);
        return -ENOENT;
    }
    }
    return -EINVAL;
}

int bpf_map_delete_fd(int fd)
{
    if (fd < 1 || fd > BPF_MAP_MAX) return -EBADF;
    spinlock_acquire(&g_maps_lock);
    memset(&g_maps[fd - 1], 0, sizeof(struct bpf_map));
    spinlock_release(&g_maps_lock);
    return 0;
}

void bpf_maps_init(void)
{
    memset(g_maps, 0, sizeof(g_maps));
    spinlock_init(&g_maps_lock);
    kprintf("[OK] BPF maps initialized (%d max)\n", BPF_MAP_MAX);
}
