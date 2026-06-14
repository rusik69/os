/* base32.c — base32 encode/decode */
#include "unistd.h"
#include "string.h"
#include "stdlib.h"

static const char b32_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

static void encode_block(unsigned char *buf, int nread) {
    char out[8];
    int i;
    /* zero pad */
    for (i = nread; i < 5; i++) buf[i] = 0;
    /* encode */
    out[0] = b32_alphabet[buf[0] >> 3];
    out[1] = b32_alphabet[((buf[0] & 0x07) << 2) | (buf[1] >> 6)];
    out[2] = b32_alphabet[(buf[1] >> 1) & 0x1F];
    out[3] = b32_alphabet[((buf[1] & 0x01) << 4) | (buf[2] >> 4)];
    out[4] = b32_alphabet[((buf[2] & 0x0F) << 1) | (buf[3] >> 7)];
    out[5] = b32_alphabet[(buf[3] >> 2) & 0x1F];
    out[6] = b32_alphabet[((buf[3] & 0x03) << 3) | (buf[4] >> 5)];
    out[7] = b32_alphabet[buf[4] & 0x1F];
    /* padding */
    if (nread < 5) {
        int npad = (8 * (5 - nread) + 4) / 5;
        for (i = 8 - npad; i < 8; i++) out[i] = '=';
    }
    write(1, out, 8);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    unsigned char buf[5];
    int nread;
    while ((nread = read(0, buf, 5)) > 0) {
        encode_block(buf, nread);
    }
    write(1, "\n", 1);
    return 0;
}
