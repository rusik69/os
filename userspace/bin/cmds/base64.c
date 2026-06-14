/* base64.c — base64 encode/decode */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void encode(void) {
    char buf[4096];
    unsigned char in[3];
    int nread, idx = 0;
    while ((nread = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < nread; i++) {
            in[idx++] = (unsigned char)buf[i];
            if (idx == 3) {
                unsigned int val = (in[0] << 16) | (in[1] << 8) | in[2];
                char out[4];
                out[0] = b64[(val >> 18) & 0x3F];
                out[1] = b64[(val >> 12) & 0x3F];
                out[2] = b64[(val >> 6) & 0x3F];
                out[3] = b64[val & 0x3F];
                write(STDOUT_FILENO, out, 4);
                idx = 0;
            }
        }
    }
    if (idx > 0) {
        if (idx == 1) {
            unsigned int val = in[0] << 16;
            char out[4];
            out[0] = b64[(val >> 18) & 0x3F];
            out[1] = b64[(val >> 12) & 0x3F];
            out[2] = '=';
            out[3] = '=';
            write(STDOUT_FILENO, out, 4);
        } else {
            unsigned int val = (in[0] << 16) | (in[1] << 8);
            char out[4];
            out[0] = b64[(val >> 18) & 0x3F];
            out[1] = b64[(val >> 12) & 0x3F];
            out[2] = b64[(val >> 6) & 0x3F];
            out[3] = '=';
            write(STDOUT_FILENO, out, 4);
        }
    }
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void decode(void) {
    char buf[4096];
    int buf_len = 0, nread;
    unsigned char in[4];
    int idx = 0;
    while ((nread = read(STDIN_FILENO, buf + buf_len, sizeof(buf) - buf_len - 1)) > 0) {
        buf_len += nread;
        for (int i = 0; i < buf_len; i++) {
            int v = b64_val(buf[i]);
            if (v >= 0) {
                in[idx++] = (unsigned char)v;
                if (idx == 4) {
                    unsigned char out[3];
                    out[0] = (in[0] << 2) | (in[1] >> 4);
                    out[1] = (in[1] << 4) | (in[2] >> 2);
                    out[2] = (in[2] << 6) | in[3];
                    write(STDOUT_FILENO, out, 3);
                    idx = 0;
                }
            } else if (buf[i] == '=') {
                break;
            }
        }
    }
}

int main(int argc, char *argv[]) {
    int decode_flag = 0;
    const char *file = 0;
    int fd_stdin_override = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) decode_flag = 1;
        else file = argv[i];
    }
    if (file) {
        fd_stdin_override = open(file, O_RDONLY, 0);
        if (fd_stdin_override < 0) { printf("base64: %s: No such file\n", file); return 1; }
        /* Redirect stdin by reading from file */
        /* Simple approach: just duplicate fd */
        dup2(fd_stdin_override, STDIN_FILENO);
        close(fd_stdin_override);
    }
    if (decode_flag) decode();
    else encode();
    return 0;
}
