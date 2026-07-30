/*
 * SHA-256 Cryptographic Hash Algorithm Implementation
 *
 * SHA-256 (Secure Hash Algorithm 2, 256-bit digest) is a member of the
 * SHA-2 family published by NIST in FIPS PUB 180-4.  It produces a 256-bit
 * (32-byte) message digest from an arbitrary-length input message.
 *
 * ALGORITHM OVERVIEW
 * ==================
 *  1. Pre-processing: Padding the message to a multiple of 512 bits (64 bytes)
 *     with a '1' bit, zeros, and the original message length as a 64-bit
 *     big-endian integer.
 *
 *  2. Processing: Each 512-bit block is expanded into 64 32-bit words (W[0..63])
 *     and processed through 64 rounds of compression using the SHA-256
 *     round constants (K[0..63]) and the working variables (a..h).
 *
 *  3. Output: The final hash is the 256-bit concatenation of the eight 32-bit
 *     state words (H0..H7) in big-endian byte order.
 *
 * DATA STRUCTURES
 * ===============
 *  struct sha256_ctx — Rolling hash context:
 *    - count:   Total bytes processed (used in padding for length field)
 *    - state:   Eight 32-bit working hash values (H0..H7)
 *    - buffer:  64-byte block buffer for partial-block input
 *
 * PUBLIC API
 * ==========
 *  sha256_init()    — Initialize a new SHA-256 hash context
 *  sha256_update()  — Feed data into the running hash (may be called multiple
 *                     times for streaming input)
 *  sha256_final()   — Finalize the hash and produce the 32-byte digest
 *  sha256_hash()    — Convenience function: init + update + final in one call
 *  sha256_init_crypto() — Module-level initialization (prints status)
 */

#include "sha256.h"
#include "string.h"
#include "printf.h"
#include "types.h"

/* SHA-256 initial hash values (H0..H7)
 *
 * These are the fractional parts of the square roots of the first eight primes
 * (2, 3, 5, 7, 11, 13, 17, 19), taken from the first 32 bits of the
 * fractional part.  They form the initial state of the hash computation. */
static const uint32_t H0[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
};

/* SHA-256 round constants (K[0..63])
 *
 * These are the fractional parts of the cube roots of the first 64 primes,
 * taken from the first 32 bits of the fractional part.  Each round uses its
 * corresponding constant to inject non-linearity into the compression
 * function. */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

/*
 * SHA-256 Boolean Functions and Rotations
 *
 * These bitwise operations are the core of the SHA-256 compression function.
 * All operate on 32-bit words.
 *
 *   ROTR32(x, n) — Right-rotate a 32-bit word by n bits.
 *   CH(x,y,z)    — Choose: returns y where x has 1 bits, z where x has 0 bits.
 *   MAJ(x,y,z)   — Majority: returns the majority value (at least 2 of 3 bits).
 *   SIG0(x)      — Upper sigma 0: used in the compression of working variable a.
 *   SIG1(x)      — Upper sigma 1: used in the compression of working variable e.
 *   sigma0(x)    — Lower sigma 0: used in the message schedule expansion.
 *   sigma1(x)    — Lower sigma 1: used in the message schedule expansion.
 */
#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SIG0(x)      (ROTR32(x, 2) ^ ROTR32(x, 13) ^ ROTR32(x, 22))
#define SIG1(x)      (ROTR32(x, 6) ^ ROTR32(x, 11) ^ ROTR32(x, 25))
#define sigma0(x)    (ROTR32(x, 7) ^ ROTR32(x, 18) ^ ((x) >> 3))
#define sigma1(x)    (ROTR32(x, 17) ^ ROTR32(x, 19) ^ ((x) >> 10))

/*
 * sha256_transform — Process a single 512-bit (64-byte) block
 * @ctx:   Hash context whose state will be updated
 * @block: 64-byte input block (big-endian 32-bit words)
 *
 * This is the heart of SHA-256.  It performs:
 *  1. Message schedule: expand the 16 input words into 64 words W[0..63]
 *     using the lower sigma functions.
 *  2. Initialise working variables a..h from the current hash state.
 *  3. Apply 64 rounds of compression using the round constants K[t],
 *     the boolean functions (CH, MAJ, SIG0, SIG1), and the message words.
 *  4. Add the resulting working variables back into the hash state.
 *
 * The compression function is a Davies-Meyer construction:
 *   state_new = state_old + compress(state_old, block)
 */
static void sha256_transform(struct sha256_ctx *ctx, const uint8_t block[SHA256_BLOCK_SIZE])
{
    uint32_t W[64] = {0}, a, b, c, d, e, f, g, h;
    int t;

    /*
     * Message schedule — Part 1: Copy the 16 big-endian words directly
     * from the input block into W[0..15].
     */
    for (t = 0; t < 16; t++) {
        W[t] = ((uint32_t)block[4*t] << 24) |
               ((uint32_t)block[4*t+1] << 16) |
               ((uint32_t)block[4*t+2] << 8)  |
               ((uint32_t)block[4*t+3]);
    }
    /*
     * Message schedule — Part 2: Expand to W[16..63] using the lower
     * sigma functions (sigma0, sigma1) to mix bits across the schedule:
     *   W[t] = sigma1(W[t-2]) + W[t-7] + sigma0(W[t-15]) + W[t-16]
     */
    for (t = 16; t < 64; t++)
        W[t] = sigma1(W[t-2]) + W[t-7] + sigma0(W[t-15]) + W[t-16];

    /* Initialise working variables from the current hash state. */
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    /*
     * 64 rounds of the SHA-256 compression function.
     *
     * Each round computes:
     *   T1 = h + SIG1(e) + CH(e, f, g) + K[t] + W[t]
     *   T2 = SIG0(a) + MAJ(a, b, c)
     * Then the working variables are rotated: (a,b,c,d,e,f,g,h) ->
     *   (T1+T2, a, b, c, d+T1, e, f, g)
     *
     * This combines bit-mixing (SIG0, SIG1), non-linear selection (CH),
     * majority voting (MAJ), and the round constants / message words to
     * produce avalanche diffusion across all state bits.
     */
    for (t = 0; t < 64; t++) {
        uint32_t T1 = h + SIG1(e) + CH(e, f, g) + K[t] + W[t];
        uint32_t T2 = SIG0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + T1;
        d = c;
        c = b;
        b = a;
        a = T1 + T2;
    }

    /* Add the compressed result back into the hash state. */
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

/*
 * sha256_init — Initialise a SHA-256 hash context
 * @ctx: Context to initialise (must be writable, non-NULL)
 *
 * Sets the byte count to zero and loads the eight initial hash values (H0..H7)
 * into the state.  After this call the context is ready for sha256_update(). */
void sha256_init(struct sha256_ctx *ctx)
{
    ctx->count = 0;
    for (int i = 0; i < 8; i++)
        ctx->state[i] = H0[i];
}

/*
 * sha256_update — Feed data into an ongoing SHA-256 hash computation
 * @ctx:  Hash context (initialised by sha256_init)
 * @data: Pointer to the input data
 * @len:  Number of bytes to process
 *
 * Processes the input data in 64-byte blocks.  Any partial block leftover
 * is buffered in ctx->buffer for the next call.  This allows streaming
 * input of arbitrary length without requiring the entire message in memory
 * at once. */
void sha256_update(struct sha256_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t idx = (size_t)(ctx->count & 0x3F);
    ctx->count += len;

    /* If there's buffered data from a previous call, try to fill a block. */
    if (idx) {
        size_t fill = SHA256_BLOCK_SIZE - idx;
        if (len < fill) {
            memcpy(&ctx->buffer[idx], p, len);
            return;
        }
        memcpy(&ctx->buffer[idx], p, fill);
        sha256_transform(ctx, ctx->buffer);
        p += fill;
        len -= fill;
    }

    /* Process full 64-byte blocks directly from the input. */
    while (len >= SHA256_BLOCK_SIZE) {
        sha256_transform(ctx, p);
        p += SHA256_BLOCK_SIZE;
        len -= SHA256_BLOCK_SIZE;
    }

    /* Buffer any remaining bytes for the next call. */
    if (len)
        memcpy(ctx->buffer, p, len);
}

/*
 * sha256_final — Finalise the hash and produce the 256-bit digest
 * @digest: Output buffer (must be at least SHA256_DIGEST_SIZE = 32 bytes)
 * @ctx:    Hash context (consumed by this call)
 *
 * Performs SHA-256 padding per FIPS PUB 180-4:
 *  1. Append a '1' bit (0x80 byte) to the message.
 *  2. Append '0' bits until the message length ≡ 56 (mod 64).
 *  3. Append the original message length (in bits) as a 64-bit big-endian integer.
 *  4. Process the final padded block(s).
 *  5. Extract the 256-bit digest from the state (big-endian 32-bit words).
 */
void sha256_final(uint8_t digest[SHA256_DIGEST_SIZE], struct sha256_ctx *ctx)
{
    uint64_t bits = ctx->count * 8;
    size_t idx = (size_t)(ctx->count & 0x3F);
    size_t pad_len = (idx < 56) ? (56 - idx) : (120 - idx);

    /* Padding: 0x80 followed by zero bytes. */
    uint8_t padding[128];
    memset(padding, 0, pad_len);
    padding[0] = 0x80;
    sha256_update(ctx, padding, pad_len);

    /* Append the 64-bit length in big-endian byte order. */
    uint8_t len_buf[8];
    for (int i = 0; i < 8; i++)
        len_buf[i] = (uint8_t)(bits >> (56 - 8*i));

    sha256_update(ctx, len_buf, 8);

    /* Extract the digest as 8 big-endian 32-bit words. */
    for (int i = 0; i < 8; i++) {
        digest[4*i]   = (uint8_t)(ctx->state[i] >> 24);
        digest[4*i+1] = (uint8_t)(ctx->state[i] >> 16);
        digest[4*i+2] = (uint8_t)(ctx->state[i] >> 8);
        digest[4*i+3] = (uint8_t)(ctx->state[i]);
    }
}

/*
 * sha256_hash — Compute SHA-256 digest of a single contiguous buffer
 * @digest: Output buffer (at least SHA256_DIGEST_SIZE = 32 bytes)
 * @data:   Input data buffer
 * @len:    Length of input data in bytes
 *
 * Convenience wrapper that calls init, update, and final in sequence.
 * For streaming or multi-buffer use, call the individual functions directly. */
void sha256_hash(uint8_t digest[SHA256_DIGEST_SIZE],
                 const void *data, size_t len)
{
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(digest, &ctx);
}

/*
 * sha256_init_crypto — Module-level initialisation
 *
 * Prints a status message confirming the SHA-256 module is ready.
 * Called once during kernel initialisation. */
void sha256_init_crypto(void)
{
    kprintf("[OK] SHA-256 initialized\n");
}
