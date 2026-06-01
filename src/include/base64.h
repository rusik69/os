#ifndef BASE64_H
#define BASE64_H

#include "types.h"

/* RFC 4648 base64 encoding/decoding */

/**
 * base64_encode - encode binary data to base64 string
 * @out:   output buffer (should be at least base64_encode_len(in_len) bytes)
 * @in:    input binary data
 * @in_len: length of input data in bytes
 * Returns: number of bytes written to out (excluding NUL terminator)
 *
 * The output string is NUL-terminated.
 */
size_t base64_encode(char *out, const uint8_t *in, size_t in_len);

/**
 * base64_decode - decode base64 string to binary data
 * @out:   output buffer (should be at least in_len * 3/4 bytes)
 * @in:    input base64 string (NUL-terminated)
 * @in_len: length of input string (excluding NUL)
 * Returns: number of bytes written to out, or (size_t)-1 on error
 */
size_t base64_decode(uint8_t *out, const char *in, size_t in_len);

/**
 * base64_encode_len - calculate required output buffer size for encoding
 */
static inline size_t base64_encode_len(size_t in_len)
{
    return ((in_len + 2) / 3) * 4 + 1;
}

/**
 * base64_decode_len - calculate maximum output size for decoding
 */
static inline size_t base64_decode_len(size_t in_len)
{
    return (in_len / 4) * 3;
}

/* Initialize base64 module */
void base64_init(void);

#endif /* BASE64_H */
