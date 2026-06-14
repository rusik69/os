#ifndef MODULE_COMPRESS_H
#define MODULE_COMPRESS_H

#include "types.h"

/*
 * module_compress.h — Module compression detection and decompression
 *
 * Detects and decompresses gzip (.ko.gz) and xz (.ko.xz) compressed
 * kernel module files before ELF loading.
 */

enum module_compress_type {
    MODULE_COMPRESS_NONE = 0,
    MODULE_COMPRESS_GZIP,
    MODULE_COMPRESS_XZ,
};

/* Check if data starts with gzip magic (1f 8b) */
int module_is_gzip(const uint8_t *data, uint64_t size);

/* Check if data starts with xz magic (fd 37 7a 58 5a 00) */
int module_is_xz(const uint8_t *data, uint64_t size);

/* Detect compression type; returns 1 if compressed, 0 otherwise.
 * Sets *out_type to the detected type. */
int module_is_compressed(const uint8_t *data, uint64_t size,
                         enum module_compress_type *out_type);

/*
 * Decompress a gzip stream.
 *
 * @in:     Pointer to gzip-compressed data
 * @in_size: Size of compressed data
 * @out:    Pre-allocated output buffer
 * @out_size: Size of output buffer
 * @decompressed_size: Output: actual decompressed size
 *
 * Returns 0 on success, negative errno on failure.
 */
int gzip_inflate(const uint8_t *in, uint64_t in_size,
                 uint8_t *out, uint64_t out_size,
                 uint64_t *decompressed_size);

/*
 * Decompress a kernel module ELF file if compressed.
 *
 * If the input is not compressed, *output points to the input directly
 * and the return value is 0 (no allocation needed).
 *
 * If the input IS compressed, it allocates a new buffer, decompresses
 * into it, and returns 1 (caller must free via module_decompress_free).
 *
 * Returns 0 (not compressed), 1 (compressed, new buffer), or negative errno.
 */
int module_decompress(const uint8_t *input, uint64_t input_size,
                       uint8_t **output, uint64_t *output_size);

/*
 * Free a decompressed buffer if it was allocated by module_decompress.
 * Safe to call with NULL or was_compressed=0.
 */
void module_decompress_free(uint8_t *buf, int was_compressed);

#endif /* MODULE_COMPRESS_H */
