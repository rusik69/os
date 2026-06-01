#ifndef COMPRESS_H
#define COMPRESS_H

#include "types.h"

/*
 * ── LZSS Compression / Decompression ─────────────────────────────────
 *
 * Lightweight LZSS implementation suitable for in-kernel use.
 * Designed for compressing panic/oops logs before persistent storage.
 *
 * Algorithm:
 *   - Sliding window of 4096 bytes (12-bit offset)
 *   - Minimum match length: 3 bytes
 *   - Maximum match length: 18 bytes (4-bit length)
 *   - Control byte codes 8 operations at a time
 *   - Hash-chain match finder for O(n) compression
 *
 * Format per control-byte group (8 ops):
 *   Bit = 0 : literal  (1 byte follows)
 *   Bit = 1 : reference (2 bytes follow: 12-bit offset | 4-bit length-3)
 *
 * Wire format:
 *   [control byte][op0][op1]...[op7]
 *   where each op is either 1 byte (literal) or 2 bytes (ref)
 */

/* ── Constants ─────────────────────────────────────────────────────── */

#define LZSS_WINDOW_SIZE    4096
#define LZSS_MIN_MATCH      3
#define LZSS_MAX_MATCH      18
#define LZSS_HASH_SIZE      4096   /* must be power of 2 */

/* Maximum safe input for stack-allocated compression buffers */
#define LZSS_MAX_INPUT      1024

/* Worst-case output: literals expand by 1/8 (control bytes) */
#define LZSS_WORST_CASE(n)  ((n) + (n) / 8 + 16)

/* ── API ──────────────────────────────────────────────────────────── */

/**
 * lzss_compress - Compress data using LZSS
 * @input:     Input data buffer
 * @input_len: Length of input data (0..LZSS_MAX_INPUT)
 * @output:    Output buffer (must be at least LZSS_WORST_CASE(input_len))
 * @output_len: Size of output buffer
 *
 * Returns: Compressed size on success, negative errno on error.
 *   -ENOMEM if output buffer too small
 *   -EINVAL if input_len is 0 or exceeds LZSS_MAX_INPUT
 */
int lzss_compress(const uint8_t *input, int input_len,
                  uint8_t *output, int output_len);

/**
 * lzss_decompress - Decompress LZSS-compressed data
 * @input:     Compressed input data
 * @input_len: Length of compressed data
 * @output:    Output buffer for decompressed data
 * @output_len: Size of output buffer
 *
 * Returns: Decompressed size on success, negative errno on error.
 *   -ENOSPC if output buffer too small for decompressed data
 *   -EINVAL on corrupt input
 */
int lzss_decompress(const uint8_t *input, int input_len,
                    uint8_t *output, int output_len);

#endif /* COMPRESS_H */
