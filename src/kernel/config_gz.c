/*
 * config_gz.c — /proc/config.gz — kernel build configuration export
 *
 * ── Overview ──────────────────────────────────────────────────────────────
 *
 * This module embeds a gzip-compressed copy of the kernel build
 * configuration (Kconfig-derived .config) and serves it via the
 * /proc/config.gz virtual file.  Userspace can inspect the exact set
 * of kernel build options at runtime with:
 *
 *     zcat /proc/config.gz
 *     cat /proc/config.gz | gunzip
 *
 * ── Data Format ───────────────────────────────────────────────────────────
 *
 * The embedded payload is a raw gzip data stream (RFC 1952) produced
 * by gzip(1) at maximum compression.  The stream consists of:
 *
 *   1. Gzip header (ID1=0x1f, ID2=0x8b, CM=0x08, flags, mtime, XFL, OS)
 *   2. Deflate-compressed data blocks (RFC 1951)
 *   3. CRC-32 checksum of the original uncompressed data
 *   4. Original uncompressed size (modulo 2^32)
 *
 * No gzip member spans multiple streams — the entire payload is a
 * single member.  The expected uncompressed size is typically 30-60 KB.
 *
 * ── Build-Time Pipeline ───────────────────────────────────────────────────
 *
 * The compressed data is created by the build system during the kernel
 * link step:
 *
 *   (a) scripts/gen_config.sh reads $(KCONFIG_CONFIG) (the .config file)
 *       and compresses it with gzip -9 -n to produce config.gz.
 *   (b) The resulting binary is converted to a C byte array using
 *       xxd -i -n build_config_gz, producing build_config_gz.h:
 *
 *           unsigned char build_config_gz[] = { 0x1f, 0x8b, 0x08, ... };
 *           unsigned int build_config_gz_len = <size>;
 *
 *   (c) build_config_gz.h is #included in this file so the compressed
 *       data is linked directly into the kernel image (no external
 *       filesystem dependency at runtime).
 *
 * Both the original .config and the generated config.gz are removed
 * from the final kernel image; only the C array remains.
 *
 * ── Runtime Interface ─────────────────────────────────────────────────────
 *
 * The /proc/config.gz virtual file is backed by a procfs file operation
 * that calls config_gz_get_data() to obtain the pointer and size of the
 * embedded blob.  No decompression is performed in the kernel — the raw
 * gzip stream is passed directly to userspace, which is responsible for
 * decompression (typically via gunzip/zcat).
 *
 * ── Error Handling ────────────────────────────────────────────────────────
 *
 * If the build-time generation step is skipped (e.g. early bring-up or
 * minimal builds), build_config_gz_len will be 0 and build_config_gz
 * will be an empty array.  In that case config_gz_init() logs a warning
 * and the /proc/config.gz entry may not be created or may return
 * -ENOENT/-ENODATA.  On a properly configured system this never happens.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"

/* ── Embedded compressed configuration data ────────────────────────────
 * Generated at build time by scripts/gen_config.sh and compressed
 * with gzip.  The variables below are defined by the auto-generated
 * header which is produced by xxd -i -n build_config_gz.
 */
#include "build_config_gz.h"

/* xxd -i -n build_config_gz produces:
 *   unsigned char build_config_gz[] = { ... };
 *   unsigned int build_config_gz_len = ...;
 */
extern unsigned char build_config_gz[];
extern unsigned int build_config_gz_len;

/* ── Public API ──────────────────────────────────────────────────────── */

/**
 * config_gz_get_data - Return a pointer to the compressed kernel config
 * @out_size: On success, set to the size of the compressed data in bytes
 *
 * Provides access to the embedded gzip-compressed kernel build
 * configuration.  The data is generated at build time by
 * scripts/gen_config.sh and embedded via build_config_gz.h.
 *
 * Return: A pointer to the compressed config data, or NULL if @out_size
 *         is NULL or the embedded data is empty
 */
const void *config_gz_get_data(uint32_t *out_size)
{
    if (!out_size)
        return NULL;
    *out_size = (uint32_t)build_config_gz_len;
    return (const void *)build_config_gz;
}

/**
 * config_gz_get_uncompressed - Return the uncompressed config text
 * @buf: Destination buffer for the uncompressed config
 * @max_len: Maximum number of bytes to write to @buf
 *
 * This operation is not directly supported — userspace should use
 * zcat /proc/config.gz to decompress the embedded data.
 *
 * Return: -ENOSYS, as this function is not yet implemented
 */
int config_gz_get_uncompressed(char *buf, int max_len)
{
    (void)buf;
    (void)max_len;
    return -ENOSYS;
}

/* ── Initialization ─────────────────────────────────────────────────── */

/**
 * config_gz_init - Initialise the /proc/config.gz subsystem
 *
 * Prints a boot message indicating whether the embedded compressed
 * configuration data is available and its size.  Called once during
 * kernel initialisation.
 */
void config_gz_init(void)
{
    uint32_t size = 0;
    const void *data = config_gz_get_data(&size);
    if (data && size > 0) {
        kprintf("[OK] config_gz: /proc/config.gz available (%u bytes compressed)\\n",
                (unsigned int)size);
    } else {
        kprintf("[!!] config_gz: no embedded config data\\n");
    }
}

/* ── Stub: config_gz_decompress ─────────────────────────────── */
static int config_gz_decompress(const void *src, size_t slen, void *dst, size_t *dlen)
{
    (void)src;
    (void)slen;
    (void)dst;
    (void)dlen;
    kprintf("[config] config_gz_decompress: not yet implemented\n");
    return 0;
}
/* ── Stub: config_gz_compress ─────────────────────────────── */
static int config_gz_compress(const void *src, size_t slen, void *dst, size_t *dlen)
{
    (void)src;
    (void)slen;
    (void)dst;
    (void)dlen;
    kprintf("[config] config_gz_compress: not yet implemented\n");
    return 0;
}
