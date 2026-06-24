/*
 * zcomp.c — Compression algorithm abstraction and per-CPU stream management
 *
 * Provides registration for multiple compression algorithms and
 * per-CPU stream infrastructure for lock-free concurrent compression.
 */

#include "zcomp.h"
#include "string.h"
#include "errno.h"
#include "printf.h"
#include "smp.h"
#include "heap.h"
#include "err.h"

/* Forward declaration — zcomp_fast_init() defined in zcomp_fast.c */
extern int zcomp_fast_init(void);

/* ── Registered algorithms table ───────────────────────────────────── */

static const struct zcomp_ops *zcomp_algorithms[ZCOMP_MAX_ALGOS];
static int zcomp_num_algorithms = 0;

/* ── Registration ──────────────────────────────────────────────────── */

int zcomp_register(const struct zcomp_ops *ops)
{
    if (!ops || !ops->compress || !ops->decompress)
        return -EINVAL;
    if (!ops->name || !ops->name[0])
        return -EINVAL;
    if (zcomp_num_algorithms >= ZCOMP_MAX_ALGOS)
        return -ENOSPC;

    /* Check for duplicate name or ID */
    for (int i = 0; i < zcomp_num_algorithms; i++) {
        if (zcomp_algorithms[i]->algo_id == ops->algo_id)
            return -EEXIST;
        if (strcmp(zcomp_algorithms[i]->name, ops->name) == 0)
            return -EEXIST;
    }

    zcomp_algorithms[zcomp_num_algorithms++] = ops;
    return 0;
}

/* ── Lookup ────────────────────────────────────────────────────────── */

const struct zcomp_ops *zcomp_find(uint32_t algo_id)
{
    for (int i = 0; i < zcomp_num_algorithms; i++) {
        if (zcomp_algorithms[i]->algo_id == algo_id)
            return zcomp_algorithms[i];
    }
    return ERR_PTR(-ENOENT);
}

const struct zcomp_ops *zcomp_find_by_name(const char *name)
{
    if (!name)
        return ERR_PTR(-EINVAL);

    for (int i = 0; i < zcomp_num_algorithms; i++) {
        if (strcmp(zcomp_algorithms[i]->name, name) == 0)
            return zcomp_algorithms[i];
    }
    return ERR_PTR(-ENOENT);
}

const char *zcomp_get_algo_name(uint32_t algo_id)
{
    const struct zcomp_ops *ops = zcomp_find(algo_id);
    return IS_ERR(ops) ? "unknown" : ops->name;
}

/* ── Per-CPU stream management ─────────────────────────────────────── */

int zcomp_streams_init(struct zcomp_stream *streams, int max_cpus,
                       const struct zcomp_ops *ops)
{
    if (!streams || !ops || max_cpus <= 0)
        return -EINVAL;

    for (int i = 0; i < max_cpus; i++) {
        streams[i].ops = ops;
        streams[i].workspace = NULL;

        if (ops->create_workspace) {
            streams[i].workspace = ops->create_workspace();
            if (!streams[i].workspace) {
                /* Clean up previously allocated workspaces */
                for (int j = 0; j < i; j++) {
                    if (ops->destroy_workspace)
                        ops->destroy_workspace(streams[j].workspace);
                    streams[j].ops = NULL;
                    streams[j].workspace = NULL;
                }
                return -ENOMEM;
            }
        }
    }
    return 0;
}

struct zcomp_stream *zcomp_stream_get(struct zcomp_stream *streams,
                                      int num_cpus)
{
    uint32_t cpu_id = (uint32_t)smp_get_cpu_id();
    if (cpu_id >= (uint32_t)num_cpus)
        cpu_id = 0; /* Fallback to CPU 0 */
    return &streams[cpu_id];
}

void zcomp_streams_destroy(struct zcomp_stream *streams, int num_cpus)
{
    if (!streams)
        return;

    for (int i = 0; i < num_cpus; i++) {
        if (streams[i].ops && streams[i].ops->destroy_workspace)
            streams[i].ops->destroy_workspace(streams[i].workspace);
        streams[i].ops = NULL;
        streams[i].workspace = NULL;
    }
}

/* ── Pass-through (no compression) ─────────────────────────────────── */

static int none_compress(const uint8_t *src, size_t src_len,
                          uint8_t *dst, size_t dst_len,
                          void *workspace)
{
    (void)workspace;
    if (dst_len < src_len)
        return -ENOSPC;
    memcpy(dst, src, src_len);
    return (int)src_len;
}

static int none_decompress(const uint8_t *src, size_t src_len,
                            uint8_t *dst, size_t dst_len,
                            void *workspace)
{
    (void)workspace;
    if (dst_len < src_len)
        return -ENOSPC;
    memcpy(dst, src, src_len);
    return (int)src_len;
}

static const struct zcomp_ops none_ops = {
    .name              = "none",
    .algo_id           = ZCOMP_ALGO_NONE,
    .compress          = none_compress,
    .decompress        = none_decompress,
    .create_workspace  = NULL,
    .destroy_workspace = NULL,
};

/* ── Initialization ────────────────────────────────────────────────── */

void __init zcomp_init(void)
{
    /* Reset registration table */
    zcomp_num_algorithms = 0;

    /* Register built-in algorithms */
    zcomp_register(&none_ops);

    /* Register fast LZ77 compressor (defined in zcomp_fast.c) */
    zcomp_fast_init();

    kprintf("[zcomp] Compression subsystem initialized: %d algorithms\n",
            zcomp_num_algorithms);
}
#include "module.h"
module_init(zcomp_init);

/* ── Pool structure for zcomp_create_pool / zcomp_destroy_pool ── */
struct zcomp_pool {
    const struct zcomp_ops *ops;
    void                   *workspace;
};

/* ── zcomp_compress — Compress data using a pool's algorithm ─── */
int zcomp_compress(void *pool, const void *src, size_t slen, void *dst, size_t *dlen)
{
    if (!pool || !src || !dst || !dlen)
        return -EINVAL;

    struct zcomp_pool *p = (struct zcomp_pool *)pool;
    if (!p->ops || !p->ops->compress)
        return -EINVAL;

    int ret = p->ops->compress((const uint8_t *)src, slen,
                                (uint8_t *)dst, *dlen, p->workspace);
    if (ret > 0) {
        *dlen = (size_t)ret;
        return 0;
    }
    return ret;
}

/* ── zcomp_decompress — Decompress data using a pool's algorithm ─ */
int zcomp_decompress(void *pool, const void *src, size_t slen, void *dst, size_t *dlen)
{
    if (!pool || !src || !dst || !dlen)
        return -EINVAL;

    struct zcomp_pool *p = (struct zcomp_pool *)pool;
    if (!p->ops || !p->ops->decompress)
        return -EINVAL;

    int ret = p->ops->decompress((const uint8_t *)src, slen,
                                  (uint8_t *)dst, *dlen, p->workspace);
    if (ret > 0) {
        *dlen = (size_t)ret;
        return 0;
    }
    return ret;
}

/* ── zcomp_create_pool — Create a compression pool ──────────── */
void* zcomp_create_pool(const char *alg)
{
    if (!alg)
        return ERR_PTR(-EINVAL);

    const struct zcomp_ops *ops = zcomp_find_by_name(alg);
    if (IS_ERR(ops)) {
        kprintf("[zcomp] zcomp_create_pool: algorithm '%s' not found\n", alg);
        return ERR_CAST(ops);
    }

    struct zcomp_pool *pool = (struct zcomp_pool *)kmalloc(sizeof(struct zcomp_pool));
    if (!pool)
        return ERR_PTR(-ENOMEM);

    pool->ops = ops;
    pool->workspace = NULL;

    /* Create a workspace if the algorithm needs one */
    if (ops->create_workspace)
        pool->workspace = ops->create_workspace();

    kprintf("[zcomp] zcomp_create_pool: created pool for '%s'\n", alg);
    return (void *)pool;
}

/* ── zcomp_destroy_pool — Destroy a compression pool ────────── */
int zcomp_destroy_pool(void *pool)
{
    if (!pool)
        return -EINVAL;

    struct zcomp_pool *p = (struct zcomp_pool *)pool;

    if (p->ops && p->ops->destroy_workspace && p->workspace)
        p->ops->destroy_workspace(p->workspace);

    kfree(pool);
    kprintf("[zcomp] zcomp_destroy_pool: pool destroyed\n");
    return 0;
}
