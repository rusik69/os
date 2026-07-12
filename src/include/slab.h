#ifndef SLAB_H
#define SLAB_H

#include "types.h"

/*
 * Slab allocator for fixed-size kernel objects.
 *
 * Usage:
 *   struct kmem_cache *my_cache = kmem_cache_create("my_object", sizeof(struct my_obj), 0, NULL);
 *   struct my_obj *obj = kmem_cache_alloc(my_cache);
 *   // ... use obj ...
 *   kmem_cache_free(my_cache, obj);
 *
 * Built-in caches (reserved for future use — currently none are created
 * at boot because process and socket structures use static tables rather
 * than slab-backed allocators, and kobject/inode/dentry types are not
 * yet defined in this kernel):
 */

struct kmem_cache;

/* Constructor: called on each newly allocated object (from a freshly allocated slab) */
typedef void (*kmem_cache_ctor_t)(void *obj);

/* Create a cache for objects of the given size.
 * If align is non-zero, objects are aligned to that boundary (power of 2).
 * ctor is called on fresh slab objects; NULL = no constructor.
 * Returns NULL on failure (out of memory). */
struct kmem_cache *kmem_cache_create(const char *name, size_t obj_size, size_t align, kmem_cache_ctor_t ctor);

/* Allocate an object from the cache. Returns NULL on OOM. */
void *kmem_cache_alloc(struct kmem_cache *cache);

/* Return an object to its cache. The object must have been allocated from this cache. */
void kmem_cache_free(struct kmem_cache *cache, void *obj);

/* Destroy a cache and free all its slabs. Only safe when all objects have been freed. */
void kmem_cache_destroy(struct kmem_cache *cache);

/* Return all empty slabs to the page allocator. Called by the kernel when memory is tight. */
void kmem_cache_reap(void);

/* Slab statistics */
struct slab_stats {
    uint64_t total_objects;
    uint64_t used_objects;
    uint64_t cache_count;
    uint64_t memory_used;
};
void slab_get_stats(struct slab_stats *s);

/* Initialize the slab subsystem and create built-in caches. Must be called after heap_init. */
void slab_init(void);

/* CPU hotplug hooks: drain/clear per-CPU slab caches when a CPU goes offline/online. */
int slab_cpu_offline(int cpu_id);
void slab_cpu_online(int cpu_id);

/* Built-in caches (currently none are created at boot — see slab_init) */

#endif
