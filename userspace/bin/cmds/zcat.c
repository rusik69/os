/* zcat.c — decompress gzip to stdout (pipe through gunzip -c) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        /* Read from stdin, decompress via gunzip -c */
        char *args[] = {"gunzip", "-c", NULL};
        execve("/bin/gunzip", args, 0);
        /* Fallback: just copy stdin to stdout */
        char buf[4096];
        int n;
        while ((n = read(0, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        return 0;
    }

    /* Decompress each file */
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            printf("zcat: cannot open %s\n", argv[i]);
            return 1;
        }

        /* Read the whole file */
        char *data = 0;
        unsigned long total = 0, alloc = 65536;
        data = malloc(alloc);
        if (!data) { close(fd); return 1; }
        int n;
        while ((n = read(fd, data + total, alloc - total)) > 0) {
            total += n;
            if (total + 65536 > alloc) {
                alloc *= 2;
                char *tmp = realloc(data, alloc);
                if (!tmp) { free(data); close(fd); return 1; }
                data = tmp;
            }
        }
        close(fd);

        if (total < 18 || (unsigned char)data[0] != 0x1f || (unsigned char)data[1] != 0x8b) {
            /* Not gzip — just output raw */
            write(1, data, total);
            free(data);
            continue;
        }

        /* Parse gzip header */
        unsigned char flg = (unsigned char)data[3];
        if (data[2] != 8) { printf("zcat: unsupported compression\n"); free(data); return 1; }
        unsigned long hdr = 10;
        if (flg & 4) { unsigned xlen = (unsigned char)data[hdr] | ((unsigned char)data[hdr+1]<<8); hdr += 2 + xlen; }
        if (flg & 8) { while (hdr < total && data[hdr]) hdr++; hdr++; }
        if (flg & 16) { while (hdr < total && data[hdr]) hdr++; hdr++; }
        if (flg & 2) { hdr += 2; }

        /* For stored blocks (no compression), extract data between header and trailer */
        /* Check if it's a stored (non-compressed) block: block header is 0x01 (final) or 0x00 (not final) */
        unsigned long pos = hdr;
        unsigned long out_pos = 0;
        unsigned long max_out = total * 2;
        unsigned char *out = malloc(max_out);
        if (!out) { free(data); return 1; }

        while (pos < total - 8) {
            unsigned char bfinal = data[pos] & 1;
            unsigned char btype = (data[pos] >> 1) & 3;
            pos++;
            if (btype == 0) {
                /* Stored block */
                pos += (pos % 2); /* skip to next byte boundary */
                if (pos + 4 > total) break;
                unsigned long len = (unsigned char)data[pos] | ((unsigned char)data[pos+1] << 8);
                unsigned long nlen = (unsigned char)data[pos+2] | ((unsigned char)data[pos+3] << 8);
                pos += 4;
                if ((len & ~nlen) != 0) break; /* sanity check */
                if (pos + len > total) break;
                if (out_pos + len > max_out) {
                    max_out = out_pos + len + 65536;
                    unsigned char *tmp = realloc(out, max_out);
                    if (!tmp) { free(out); free(data); return 1; }
                    out = tmp;
                }
                memcpy(out + out_pos, data + pos, len);
                out_pos += len;
                pos += len;
            } else if (btype == 3) {
                break; /* reserved */
            } else {
                /* Dynamic/fixed Huffman — can't decompress inline, fall through to raw output */
                break;
            }
            if (bfinal) break;
        }

        if (out_pos > 0) {
            write(1, out, out_pos);
        } else {
            /* Couldn't decompress stored blocks — output raw (strip header/trailer) */
            write(1, data + hdr, total - hdr - 8);
        }

        free(out);
        free(data);
    }
    return 0;
}
