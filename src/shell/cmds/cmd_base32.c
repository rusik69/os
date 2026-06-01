/* cmd_base32.c — base32 encode/decode */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

static const char base32_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

static void encode_block(const uint8_t in[5], char out[8], int len) {
    uint64_t val = 0;
    for (int i = 0; i < len; i++)
        val = (val << 8) | in[i];
    val <<= (5 - len) * 8;
    for (int i = 0; i < 8; i++) {
        int idx = (val >> (35 - i * 5)) & 0x1F;
        out[i] = (i < (len * 8 + 4) / 5) ? base32_alphabet[idx] : '=';
    }
}

static int decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '2' && c <= '7') return c - '2' + 26;
    return -1;
}

static int decode_block(const char in[8], uint8_t out[5]) {
    uint64_t val = 0;
    int pad = 0;
    for (int i = 0; i < 8; i++) {
        if (in[i] == '=') { pad++; continue; }
        int d = decode_char(in[i]);
        if (d < 0) return -1;
        val = (val << 5) | d;
    }
    int bytes = (40 - pad * 5) / 8;
    for (int i = 0; i < bytes; i++)
        out[i] = (val >> ((bytes - 1 - i) * 8)) & 0xFF;
    return bytes;
}

int cmd_base32(int argc, char **argv) {
    int decode = 0;
    const char *str = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--decode") == 0)
            decode = 1;
        else if (argv[i][0] != '-')
            str = argv[i];
    }

    if (!str) {
        kprintf("Usage: base32 [-d] <string>\n");
        return 1;
    }

    if (decode) {
        /* Decode */
        size_t slen = strlen(str);
        uint8_t out[64];
        int outpos = 0;
        for (size_t i = 0; i + 8 <= slen; i += 8) {
            uint8_t block[5];
            int n = decode_block(str + i, block);
            if (n < 0) { kprintf("base32: invalid input\n"); return 1; }
            for (int j = 0; j < n; j++) out[outpos++] = block[j];
        }
        out[outpos] = '\0';
        kprintf("%s\n", (char *)out);
    } else {
        /* Encode */
        size_t slen = strlen(str);
        size_t i = 0;
        while (i < slen) {
            uint8_t in[5] = {0};
            int len = 0;
            while (len < 5 && i < slen) in[len++] = (uint8_t)str[i++];
            char out[9] = {0};
            encode_block(in, out, len);
            kprintf("%s", out);
        }
        kprintf("\n");
    }
    return 0;
}
