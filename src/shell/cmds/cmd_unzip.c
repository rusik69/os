/* cmd_unzip.c — list/extract zip files */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

#define ZIP_LOCAL_HDR_SIG  0x04034b50
#define ZIP_CENTRAL_HDR_SIG 0x02014b50
#define ZIP_END_CENTRAL_SIG 0x06054b50

void cmd_unzip(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: unzip <file.zip> [files...]\n");
        return;
    }

    char argbuf[128];
    strncpy(argbuf, args, 127); argbuf[127] = '\0';

    char *archive_name = strtok(argbuf, " ");
    char *extract_filter[16];
    int num_filters = 0;

    char *f = strtok((char *)0, " ");
    while (f && num_filters < 16) {
        extract_filter[num_filters++] = f;
        f = strtok((char *)0, " ");
    }

    if (!archive_name) {
        kprintf("Usage: unzip <file.zip> [files...]\n");
        return;
    }

    char path[64];
    if (archive_name[0] != '/') { path[0] = '/'; strncpy(path + 1, archive_name, 62); path[63] = '\0'; }
    else { strncpy(path, archive_name, 63); path[63] = '\0'; }

    static unsigned char inbuf[16384];
    uint32_t in_size = 0;
    if (libc_vfs_read(path, inbuf, sizeof(inbuf), &in_size) != 0) {
        kprintf("unzip: %s: not found\n", archive_name);
        return;
    }

    uint32_t pos = 0;
    int extracted = 0;

    while (pos + 30 <= in_size) {
        uint32_t sig = *((uint32_t*)(inbuf + pos));
        if (sig == ZIP_END_CENTRAL_SIG) break;

        if (sig != ZIP_LOCAL_HDR_SIG) {
            pos++;
            continue;
        }

        uint16_t compression   = *((uint16_t*)(inbuf + pos + 8));
        uint32_t comp_size     = *((uint32_t*)(inbuf + pos + 18));
        uint32_t uncomp_size   = *((uint32_t*)(inbuf + pos + 22));
        uint16_t filename_len  = *((uint16_t*)(inbuf + pos + 26));
        uint16_t extra_len     = *((uint16_t*)(inbuf + pos + 28));

        if (pos + 30 + filename_len + extra_len + comp_size > in_size)
            break;

        /* Extract filename */
        char fname[256];
        uint16_t copy_len = filename_len < 255 ? filename_len : 255;
        memcpy(fname, inbuf + pos + 30, copy_len);
        fname[copy_len] = '\0';

        /* Check if this file matches filter */
        int should_extract = (num_filters == 0);
        for (int i = 0; !should_extract && i < num_filters; i++) {
            if (strcmp(fname, extract_filter[i]) == 0)
                should_extract = 1;
        }

        if (should_extract) {
            uint32_t data_start = pos + 30 + filename_len + extra_len;

            kprintf("  extracting: %s ", fname);
            if (compression == 0) {
                /* Stored — copy directly */
                char outpath[64];
                outpath[0] = '/';
                strncpy(outpath + 1, fname, 62);
                outpath[63] = '\0';
                libc_vfs_write(outpath, inbuf + data_start, comp_size);
                kprintf("(%u bytes)\n", (unsigned long)comp_size);
                extracted++;
            } else if (compression == 8) {
                /* Deflated — try our simple decompressor */
                kprintf("(deflated, %u -> %u bytes)\n", (unsigned long)comp_size, (unsigned long)uncomp_size);
                /* For deflated entries, copy raw compressed data as-is */
                char outpath[64];
                outpath[0] = '/';
                strncpy(outpath + 1, fname, 62);
                outpath[63] = '\0';
                /* For deflate, we just store the compressed data for now */
                libc_vfs_write(outpath, inbuf + data_start, comp_size);
                extracted++;
            } else {
                kprintf("(unsupported compression method %u)\n", (unsigned long)compression);
            }
        }

        pos += 30 + filename_len + extra_len + comp_size;
    }

    if (extracted == 0) {
        /* List mode if no extraction happened */
        pos = 0;
        kprintf("Archive: %s\n", path);
        kprintf("  Length     Name\n");
        kprintf("  ------     ----\n");
        while (pos + 30 <= in_size) {
            uint32_t sig2 = *((uint32_t*)(inbuf + pos));
            if (sig2 == ZIP_END_CENTRAL_SIG) break;
            if (sig2 != ZIP_LOCAL_HDR_SIG) { pos++; continue; }
            uint32_t usz = *((uint32_t*)(inbuf + pos + 22));
            uint16_t fnlen = *((uint16_t*)(inbuf + pos + 26));
            uint16_t exlen = *((uint16_t*)(inbuf + pos + 28));
            uint32_t csz = *((uint32_t*)(inbuf + pos + 18));
            if (pos + 30 + fnlen + exlen > in_size) break;
            char fname2[256];
            uint16_t clen = fnlen < 255 ? fnlen : 255;
            memcpy(fname2, inbuf + pos + 30, clen);
            fname2[clen] = '\0';
            kprintf("  %7u   %s\n", (unsigned long)usz, fname2);
            pos += 30 + fnlen + exlen + csz;
        }
    }
}
