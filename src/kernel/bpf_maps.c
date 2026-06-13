/*
 * bpf_maps.c — eBPF map implementations (hash map and array map)
 *
 * Provides the map data structures used by eBPF programs for
 * communicating with userspace and storing state between invocations.
 *
 * Supported map types:
 *   - BPF_MAP_TYPE_HASH: General-purpose hash table (key/value)
 *   - BPF_MAP_TYPE_ARRAY: Fixed-size array indexed by key
 *
 * Item 133 — eBPF hash and array maps
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"

/* ── Map types ───────────────────────────────────────────────────────── */

#define BPF_MAP_TYPE_UNSPEC 0
#define BPF_MAP_TYPE_HASH   1
#define BPF_MAP_TYPE_ARRAY  2
#define BPF_MAP_TYPE_MAX    3

/* ── Map flags ───────────────────────────────────────────────────────── */

#define BPF_F_NO_PREALLOC     (1UL << 0)

/* ── Map creation attributes ─────────────────────────────────────────── */

struct bpf_map_attr {
    uint32_t map_type;
    uint32_t key_size;
    uint32_t value_size;
    uint32_t max_entries;
    uint32_t map_flags;
};

/* ── Maximum number of maps ──────────────────────────────────────────── */

#define BPF_MAX_MAPS 32

/* ── Hash table entry ───────────────────────────────────────────────── */

struct bpf_hash_entry {
    struct bpf_hash_entry *next;  /* Collision chain */
    void *key;
    void *value;
};

/* ── Map descriptor ─────────────────────────────────────────────────── */

struct bpf_map {
    int      in_use;
    uint32_t map_type;
    uint32_t key_size;
    uint32_t value_size;
    uint32_t max_entries;
    uint32_t map_flags;

    /* Type-specific data */
    union {
        struct {
            struct bpf_hash_entry **buckets;
            uint32_t num_buckets;
        } hash;
        struct {
            void *data;      /* Flat array of (key_size + value_size) entries */
            uint32_t entry_size;
        } array;
    };
};

/* ── Global map table ───────────────────────────────────────────────── */

static struct bpf_map bpf_maps[BPF_MAX_MAPS];
static int bpf_maps_initialized = 0;

/* ── Internal helpers ────────────────────────────────────────────────── */

static uint32_t hash_ptr(const void *key, uint32_t key_size, uint32_t num_buckets)
{
    /* Simple djb2 hash over key bytes */
    uint32_t hash = 5381;
    const uint8_t *p = (const uint8_t *)key;
    for (uint32_t i = 0; i < key_size; i++) {
        hash = ((hash << 5) + hash) + p[i];
    }
    return hash % num_buckets;
}

/* ── Public API ─────────────────────────────────────────────────────── */

void bpf_maps_init(void)
{
    if (bpf_maps_initialized) return;
    memset(bpf_maps, 0, sizeof(bpf_maps));
    bpf_maps_initialized = 1;
    kprintf("[bpf_maps] eBPF map framework initialized\n");
}

/*
 * Create a new eBPF map.
 *
 * @attr:   Map creation attributes.
 * @returns Map file descriptor (positive int) on success, or negative error.
 */
int bpf_map_create(const struct bpf_map_attr *attr)
{
    if (!bpf_maps_initialized || !attr)
        return -EINVAL;

    if (attr->map_type <= BPF_MAP_TYPE_UNSPEC || attr->map_type >= BPF_MAP_TYPE_MAX)
        return -EINVAL;

    if (attr->key_size == 0 || attr->value_size == 0 || attr->max_entries == 0)
        return -EINVAL;

    /* Find a free slot */
    int fd = -1;
    for (int i = 0; i < BPF_MAX_MAPS; i++) {
        if (!bpf_maps[i].in_use) {
            fd = i + 1;  /* fd = index + 1 (0 is reserved) */
            break;
        }
    }

    if (fd < 0)
        return -ENOSPC;

    int idx = fd - 1;
    struct bpf_map *map = &bpf_maps[idx];
    map->in_use = 1;
    map->map_type = attr->map_type;
    map->key_size = attr->key_size;
    map->value_size = attr->value_size;
    map->max_entries = attr->max_entries;
    map->map_flags = attr->map_flags;

    int ret = 0;

    switch (attr->map_type) {
    case BPF_MAP_TYPE_HASH: {
        /* Allocate hash buckets */
        map->hash.num_buckets = attr->max_entries;
        map->hash.buckets = (struct bpf_hash_entry **)
            kmalloc(map->hash.num_buckets * sizeof(struct bpf_hash_entry *));
        if (!map->hash.buckets) {
            ret = -ENOMEM;
            goto fail;
        }
        memset(map->hash.buckets, 0,
               map->hash.num_buckets * sizeof(struct bpf_hash_entry *));
        break;
    }

    case BPF_MAP_TYPE_ARRAY: {
        /* Allocate flat array */
        map->array.entry_size = attr->key_size + attr->value_size;
        map->array.data = kmalloc(attr->max_entries * map->array.entry_size);
        if (!map->array.data) {
            ret = -ENOMEM;
            goto fail;
        }
        memset(map->array.data, 0, attr->max_entries * map->array.entry_size);
        break;
    }

    default:
        ret = -EINVAL;
        goto fail;
    }

    kprintf("[bpf_maps] Created map fd=%d type=%u key=%u val=%u max=%u\n",
            fd, attr->map_type, attr->key_size,
            attr->value_size, attr->max_entries);
    return fd;

fail:
    map->in_use = 0;
    return ret;
}

/*
 * Look up an entry in an eBPF map.
 *
 * @map_fd:  Map file descriptor.
 * @key:     Pointer to key.
 * @value:   Output buffer for value.
 *
 * Returns 0 on success, -ENOENT if not found, negative error otherwise.
 */
int bpf_map_lookup_elem(int map_fd, const void *key, void *value)
{
    if (!bpf_maps_initialized)
        return -ENOSYS;

    int idx = map_fd - 1;
    if (idx < 0 || idx >= BPF_MAX_MAPS || !bpf_maps[idx].in_use)
        return -EINVAL;

    struct bpf_map *map = &bpf_maps[idx];
    if (!key || !value)
        return -EINVAL;

    switch (map->map_type) {
    case BPF_MAP_TYPE_HASH: {
        uint32_t bucket = hash_ptr(key, map->key_size, map->hash.num_buckets);
        struct bpf_hash_entry *entry = map->hash.buckets[bucket];
        while (entry) {
            if (memcmp(entry->key, key, map->key_size) == 0) {
                memcpy(value, entry->value, map->value_size);
                return 0;
            }
            entry = entry->next;
        }
        return -ENOENT;
    }

    case BPF_MAP_TYPE_ARRAY: {
        /* Array maps: key is a uint32_t index */
        uint32_t idx_key;
        if (map->key_size < sizeof(uint32_t))
            return -EINVAL;
        memcpy(&idx_key, key, sizeof(uint32_t));

        if (idx_key >= map->max_entries)
            return -ENOENT;

        uint8_t *entry = (uint8_t *)map->array.data +
                          idx_key * map->array.entry_size;
        /* Skip key portion in the stored entry */
        memcpy(value, entry + map->key_size, map->value_size);
        return 0;
    }

    default:
        return -EINVAL;
    }
}

/*
 * Update/create an entry in an eBPF map.
 *
 * @map_fd:  Map file descriptor.
 * @key:     Pointer to key.
 * @value:   Pointer to value.
 * @flags:   BPF_ANY (create or update), BPF_NOEXIST (create only),
 *           BPF_EXIST (update only).
 *
 * Returns 0 on success, negative error otherwise.
 */
int bpf_map_update_elem(int map_fd, const void *key, const void *value, uint64_t flags)
{
    (void)flags;

    if (!bpf_maps_initialized)
        return -ENOSYS;

    int idx = map_fd - 1;
    if (idx < 0 || idx >= BPF_MAX_MAPS || !bpf_maps[idx].in_use)
        return -EINVAL;

    struct bpf_map *map = &bpf_maps[idx];
    if (!key || !value)
        return -EINVAL;

    switch (map->map_type) {
    case BPF_MAP_TYPE_HASH: {
        uint32_t bucket = hash_ptr(key, map->key_size, map->hash.num_buckets);

        /* Check if key exists */
        struct bpf_hash_entry *entry = map->hash.buckets[bucket];
        while (entry) {
            if (memcmp(entry->key, key, map->key_size) == 0) {
                memcpy(entry->value, value, map->value_size);
                return 0;
            }
            entry = entry->next;
        }

        /* Create new entry */
        entry = (struct bpf_hash_entry *)kmalloc(sizeof(struct bpf_hash_entry));
        if (!entry) return -ENOMEM;

        entry->key = kmalloc(map->key_size);
        entry->value = kmalloc(map->value_size);
        if (!entry->key || !entry->value) {
            if (entry->key) kfree(entry->key);
            if (entry->value) kfree(entry->value);
            kfree(entry);
            return -ENOMEM;
        }

        memcpy(entry->key, key, map->key_size);
        memcpy(entry->value, value, map->value_size);

        /* Insert at head of bucket chain */
        entry->next = map->hash.buckets[bucket];
        map->hash.buckets[bucket] = entry;
        return 0;
    }

    case BPF_MAP_TYPE_ARRAY: {
        uint32_t idx_key;
        if (map->key_size < sizeof(uint32_t))
            return -EINVAL;
        memcpy(&idx_key, key, sizeof(uint32_t));

        if (idx_key >= map->max_entries)
            return -ENOENT;

        uint8_t *entry = (uint8_t *)map->array.data +
                          idx_key * map->array.entry_size;
        memcpy(entry + map->key_size, value, map->value_size);
        return 0;
    }

    default:
        return -EINVAL;
    }
}

/*
 * Delete an entry from an eBPF map.
 *
 * @map_fd:  Map file descriptor.
 * @key:     Pointer to key.
 *
 * Returns 0 on success, -ENOENT if not found.
 */
int bpf_map_delete_elem(int map_fd, const void *key)
{
    if (!bpf_maps_initialized)
        return -ENOSYS;

    int idx = map_fd - 1;
    if (idx < 0 || idx >= BPF_MAX_MAPS || !bpf_maps[idx].in_use)
        return -EINVAL;

    struct bpf_map *map = &bpf_maps[idx];
    if (!key)
        return -EINVAL;

    switch (map->map_type) {
    case BPF_MAP_TYPE_HASH: {
        uint32_t bucket = hash_ptr(key, map->key_size, map->hash.num_buckets);
        struct bpf_hash_entry *entry = map->hash.buckets[bucket];
        struct bpf_hash_entry *prev = NULL;

        while (entry) {
            if (memcmp(entry->key, key, map->key_size) == 0) {
                if (prev)
                    prev->next = entry->next;
                else
                    map->hash.buckets[bucket] = entry->next;

                kfree(entry->key);
                kfree(entry->value);
                kfree(entry);
                return 0;
            }
            prev = entry;
            entry = entry->next;
        }
        return -ENOENT;
    }

    case BPF_MAP_TYPE_ARRAY: {
        /* Array entries cannot be deleted, only zeroed */
        uint32_t idx_key;
        if (map->key_size < sizeof(uint32_t))
            return -EINVAL;
        memcpy(&idx_key, key, sizeof(uint32_t));

        if (idx_key >= map->max_entries)
            return -ENOENT;

        uint8_t *entry = (uint8_t *)map->array.data +
                          idx_key * map->array.entry_size;
        memset(entry + map->key_size, 0, map->value_size);
        return 0;
    }

    default:
        return -EINVAL;
    }
}

/*
 * Get the next key in a map (for iteration).
 *
 * @map_fd:    Map file descriptor.
 * @key:       Current key (NULL for first key).
 * @next_key:  Output buffer for next key.
 *
 * Returns 0 on success, -ENOENT if no more keys.
 */
int bpf_map_get_next_key(int map_fd, const void *key, void *next_key)
{
    if (!bpf_maps_initialized)
        return -ENOSYS;

    int idx = map_fd - 1;
    if (idx < 0 || idx >= BPF_MAX_MAPS || !bpf_maps[idx].in_use)
        return -EINVAL;

    struct bpf_map *map = &bpf_maps[idx];

    switch (map->map_type) {
    case BPF_MAP_TYPE_HASH: {
        /* Simple linear scan (inefficient but works for small maps) */
        int found_key = (key == NULL);  /* If key is NULL, return first entry */
        int first_found = 0;

        for (uint32_t b = 0; b < map->hash.num_buckets; b++) {
            struct bpf_hash_entry *entry = map->hash.buckets[b];
            while (entry) {
                if (found_key && !first_found) {
                    memcpy(next_key, entry->key, map->key_size);
                    return 0;
                }
                if (!found_key && key && memcmp(entry->key, key, map->key_size) == 0) {
                    found_key = 1;
                }
                entry = entry->next;
            }
        }
        return -ENOENT;
    }

    case BPF_MAP_TYPE_ARRAY: {
        uint32_t curr = 0;
        if (key) {
            if (map->key_size < sizeof(uint32_t))
                return -EINVAL;
            memcpy(&curr, key, sizeof(uint32_t));
            curr++;
        }
        if (curr >= map->max_entries)
            return -ENOENT;

        uint8_t *buf = (uint8_t *)next_key;
        memcpy(buf, &curr, sizeof(uint32_t));
        return 0;
    }

    default:
        return -EINVAL;
    }
}

/*
 * Close/free an eBPF map.
 */
int bpf_map_close(int map_fd)
{
    if (!bpf_maps_initialized)
        return -ENOSYS;

    int idx = map_fd - 1;
    if (idx < 0 || idx >= BPF_MAX_MAPS || !bpf_maps[idx].in_use)
        return -EINVAL;

    struct bpf_map *map = &bpf_maps[idx];

    switch (map->map_type) {
    case BPF_MAP_TYPE_HASH:
        /* Free all hash entries */
        for (uint32_t b = 0; b < map->hash.num_buckets; b++) {
            struct bpf_hash_entry *entry = map->hash.buckets[b];
            while (entry) {
                struct bpf_hash_entry *next = entry->next;
                kfree(entry->key);
                kfree(entry->value);
                kfree(entry);
                entry = next;
            }
        }
        kfree(map->hash.buckets);
        break;

    case BPF_MAP_TYPE_ARRAY:
        kfree(map->array.data);
        break;
    }

    memset(map, 0, sizeof(*map));
    return 0;
}
