#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

/* Self-contained SHA-256 implementation (FIPS 180-4).
 * Reads the named file (up to 64 KiB, matching the e2e workload) and prints a
 * 64-char hex digest + filename, GNU sha256sum style. */

#define SHA256_BLOCK 64
#define SHA256_DIGEST 32

typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[SHA256_BLOCK];
    int buflen;
} sha256_ctx;

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(sha256_ctx *c, const unsigned char *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1 = rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
        w[i] = w[i-16]+s0+w[i-7]+s1;
    }
    uint32_t a=c->h[0], b=c->h[1], cc=c->h[2], d=c->h[3];
    uint32_t e=c->h[4], f=c->h[5], g=c->h[6], h=c->h[7];
    for (int t = 0; t < 64; t++) {
        uint32_t S1 = rotr(e,6)^rotr(e,11)^rotr(e,25);
        uint32_t ch = (e&f)^((~e)&g);
        uint32_t t1 = h+S1+ch+K[t]+w[t];
        uint32_t S0 = rotr(a,2)^rotr(a,13)^rotr(a,22);
        uint32_t maj = (a&b)^(a&cc)^(b&cc);
        uint32_t t2 = S0+maj;
        h=g; g=f; f=e; e=d+t1; d=a; a=t1+t2; b=cc; cc=d;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

static void sha256_init(sha256_ctx *c) {
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85; c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c; c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->len=0; c->buflen=0;
}

static void sha256_update(sha256_ctx *c, const unsigned char *data, uint32_t len) {
    c->len += len;
    while (len > 0) {
        int take = SHA256_BLOCK - c->buflen;
        if ((int)len < take) take = len;
        memcpy(c->buf + c->buflen, data, take);
        c->buflen += take; data += take; len -= take;
        if (c->buflen == SHA256_BLOCK) { sha256_block(c, c->buf); c->buflen = 0; }
    }
}

static void sha256_final(sha256_ctx *c, unsigned char out[SHA256_DIGEST]) {
    uint64_t bits = c->len * 8;
    unsigned char pad = 0x80;
    sha256_update(c, &pad, 1);
    unsigned char zero = 0;
    while (c->buflen != 56) sha256_update(c, &zero, 1);
    unsigned char lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (unsigned char)(bits >> (56 - 8*i));
    sha256_update(c, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (unsigned char)(c->h[i]>>24);
        out[i*4+1] = (unsigned char)(c->h[i]>>16);
        out[i*4+2] = (unsigned char)(c->h[i]>>8);
        out[i*4+3] = (unsigned char)(c->h[i]);
    }
}

void cmd_sha256sum(const char *args) {
    if (!args || !args[0]) { kprintf("Usage: sha256sum <file>\n"); return; }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); }
    else strncpy(path, args, 63);
    path[63] = '\0';
    int pl = strlen(path);
    while (pl > 0 && path[pl-1] == ' ') path[--pl] = '\0';

    static char buf[65536];
    uint32_t size = 0;
    if (vfs_read(path, buf, sizeof(buf), &size) != 0) {
        kprintf("sha256sum: cannot read '%s'\n", path);
        return;
    }

    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (unsigned char *)buf, size);

    unsigned char digest[SHA256_DIGEST];
    sha256_final(&ctx, digest);

    for (int i = 0; i < SHA256_DIGEST; i++)
        kprintf("%02x", digest[i]);
    kprintf("  %s\n", path);
}
