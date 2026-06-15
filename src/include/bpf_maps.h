#ifndef BPF_MAPS_H
#define BPF_MAPS_H

#include "types.h"

/* eBPF map flags */
#define BPF_NOEXIST  1  /* create new entry, fail if exists */
#define BPF_EXIST    2  /* update existing entry */
#define BPF_ANY      0  /* create or update */

/* Map types */
#define BPF_MAP_TYPE_UNSPEC          0
#define BPF_MAP_TYPE_HASH            1
#define BPF_MAP_TYPE_ARRAY           2
#define BPF_MAP_TYPE_PERF_EVENT_ARRAY 3

/* Create a new eBPF map.
 * Returns fd (>= 1) on success, negative errno on failure. */
int bpf_map_create(int map_type, uint32_t key_size, uint32_t value_size,
                   uint32_t max_entries, uint32_t map_flags);

/* Look up a key in an eBPF map.
 * Returns 0 on success, negative on error. */
int bpf_map_lookup_elem(int fd, const void *key, void *value);

/* Update/create an entry in an eBPF map.
 * @flags: BPF_ANY, BPF_NOEXIST, or BPF_EXIST
 * Returns 0 on success, negative on error. */
int bpf_map_update_elem(int fd, const void *key, const void *value,
                        uint64_t flags);

/* Delete an entry from an eBPF map.
 * Returns 0 on success, negative on error. */
int bpf_map_delete_elem(int fd, const void *key);

/* Get the next key after @key (for iteration).
 * If @key is NULL, gets the first key.
 * Returns 0 on success, -ENOENT if no more keys. */
int bpf_map_get_next_key(int fd, const void *key, void *next_key);

/* Close and free an eBPF map. */
int bpf_map_delete_fd(int fd);

/* Initialize the BPF maps subsystem. */
void bpf_maps_init(void);

#endif /* BPF_MAPS_H */
