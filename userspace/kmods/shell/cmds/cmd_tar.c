/* cmd_tar.c — tar command with -c, -x, -z support */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"
#include "types.h"

#define TAR_MAGIC "TARS"
#define TAR_BUF_SIZE (256 * 1024)

/* ── GZIP decompression (RLE-based, from cmd_gunzip.c) ──────────── */
static int gzip_decompress(const unsigned char *in, uint32_t in_size,
                           unsigned char *out, uint32_t out_max, uint32_t *out_size) {
    /* Expect gzip header: 1f 8b 08 */
    if (in_size < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8)
        return -1;

    uint32_t pos = 10;
    uint8_t flg = in[3];
    if (flg & 4) { /* FEXTRA */
        if (pos + 2 > in_size) return -1;
        uint16_t xlen = (uint16_t)(in[pos] | (in[pos+1] << 8));
        pos += 2 + xlen;
    }
    if (flg & 8) { /* FNAME */
        while (pos < in_size && in[pos]) pos++;
        pos++;
    }
    if (flg & 16) { /* FCOMMENT */
        while (pos < in_size && in[pos]) pos++;
        pos++;
    }
    if (flg & 2) { /* FHCRC */
        pos += 2;
    }
    if (pos >= in_size) return -1;

    uint32_t opos = 0;
    while (pos < in_size - 8 && opos < out_max) {
        if (in[pos] == 0x00 && pos + 2 < in_size - 8) {
            unsigned char run_len = in[pos + 1];
            unsigned char byte_val = in[pos + 2];
            for (int k = 0; k < run_len && opos < out_max; k++)
                out[opos++] = byte_val;
            pos += 3;
        } else {
            out[opos++] = in[pos++];
        }
    }
    *out_size = opos;
    return 0;
}

/* ── GZIP compression (RLE-based, from cmd_gzip.c) ──────────────── */
static int gzip_compress(const unsigned char *in, uint32_t in_size,
                         unsigned char *out, uint32_t out_max, uint32_t *out_size) {
    if (out_max < 18) return -1;

    uint32_t opos = 0;
    /* GZIP header: ID1=0x1f, ID2=0x8b, CM=8 (deflate), FLG=0, MTIME=0, XFL=0, OS=255 */
    out[opos++] = 0x1f; out[opos++] = 0x8b; out[opos++] = 8; out[opos++] = 0;
    out[opos++] = 0; out[opos++] = 0; out[opos++] = 0; out[opos++] = 0;
    out[opos++] = 0; out[opos++] = 255;

    /* Simple RLE: 0x00 marker, count, byte (for runs >= 4), otherwise literal bytes */
    uint32_t i = 0;
    while (i < in_size && opos < out_max - 256) {
        uint32_t run_start = i;
        while (i + 1 < in_size && in[i + 1] == in[run_start] && (i - run_start + 1) < 255)
            i++;
        uint32_t run_len = i - run_start + 1;
        if (run_len >= 4) {
            if (opos + 3 > out_max) break;
            out[opos++] = 0x00;
            out[opos++] = (unsigned char)run_len;
            out[opos++] = in[run_start];
        } else {
            for (uint32_t k = 0; k < run_len && opos < out_max; k++)
                out[opos++] = in[run_start + k];
        }
        i++;
    }

    /* Append stored block marker (BFINAL=1, BTYPE=0) */
    if (opos + 5 > out_max) return -1;
    out[opos++] = 0x01;
    uint16_t len = (uint16_t)(in_size % 65536);
    uint16_t nlen = ~len;
    out[opos++] = len & 0xFF; out[opos++] = (len >> 8) & 0xFF;
    out[opos++] = nlen & 0xFF; out[opos++] = (nlen >> 8) & 0xFF;

    /* Append original data as stored block content */
    for (uint32_t k = 0; k < in_size && opos < out_max; k++)
        out[opos++] = in[k];

    /* CRC32 (dummy) and original size */
    if (opos + 8 > out_max) return -1;
    uint32_t crc = 0;
    out[opos++] = (unsigned char)(crc);
    out[opos++] = (unsigned char)(crc >> 8);
    out[opos++] = (unsigned char)(crc >> 16);
    out[opos++] = (unsigned char)(crc >> 24);
    out[opos++] = (unsigned char)(in_size);
    out[opos++] = (unsigned char)(in_size >> 8);
    out[opos++] = (unsigned char)(in_size >> 16);
    out[opos++] = (unsigned char)(in_size >> 24);

    *out_size = opos;
    return 0;
}

void cmd_tar(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: tar -c[f] <archive> <file1> [file2 ...]\n");
        kprintf("       tar -x[f] <archive>\n");
        kprintf("  -c  create archive\n");
        kprintf("  -x  extract archive\n");
        kprintf("  -z  filter through gzip\n");
        return;
    }

    char arg_copy[512];
    strncpy(arg_copy, args, 511);
    arg_copy[511] = '\0';

    /* Parse flags */
    int mode_create = 0;
    int mode_extract = 0;
    int flag_z = 0;

    char *token = strtok(arg_copy, " ");
    if (!token) return;

    if (token[0] == '-') {
        for (int i = 1; token[i]; i++) {
            switch (token[i]) {
                case 'c': mode_create = 1; break;
                case 'x': mode_extract = 1; break;
                case 'z': flag_z = 1; break;
                case 'f': /* archive name follows */ break;
                default:
                    kprintf("tar: unknown flag -%c\n", token[i]);
                    return;
            }
        }
        token = strtok(NULL, " ");
        if (!token) { kprintf("tar: missing archive name\n"); return; }
    } else {
        /* Backward compat: first arg is archive name, rest are files */
        mode_create = 1;
    }

    char archive_name[64];
    strncpy(archive_name, token, 63);
    archive_name[63] = '\0';

    /* Collect file names (for create mode) */
    char *files[32];
    int file_count = 0;
    while ((token = strtok(NULL, " ")) && file_count < 32) {
        files[file_count++] = token;
    }

    /* ── EXTRACT MODE ────────────────────────────────────────────── */
    if (mode_extract) {
        unsigned char *buf = (unsigned char *)libc_malloc(TAR_BUF_SIZE);
        if (!buf) { kprintf("tar: out of memory\n"); return; }
        uint32_t size = 0;
        if (libc_vfs_read(archive_name, buf, TAR_BUF_SIZE - 1, &size) != 0) {
            kprintf("tar: %s: not found\n", archive_name);
            libc_free(buf);
            return;
        }

        unsigned char *data = buf;
        uint32_t data_size = size;
        unsigned char *decompressed = NULL;

        if (flag_z) {
            /* Check if it's already a gzip file */
            if (size >= 3 && buf[0] == 0x1f && buf[1] == 0x8b && buf[2] == 8) {
                decompressed = (unsigned char *)libc_malloc(TAR_BUF_SIZE);
                if (!decompressed) {
                    libc_free(buf);
                    kprintf("tar: out of memory\n");
                    return;
                }
                uint32_t dsize = 0;
                if (gzip_decompress(buf, size, decompressed, TAR_BUF_SIZE - 1, &dsize) != 0) {
                    kprintf("tar: gzip decompression failed\n");
                    libc_free(buf);
                    libc_free(decompressed);
                    return;
                }
                data = decompressed;
                data_size = dsize;
            } else {
                kprintf("tar: not in gzip format\n");
                libc_free(buf);
                return;
            }
        }

        /* Parse TARS archive */
        if (data_size < 4 || memcmp(data, TAR_MAGIC, 4) != 0) {
            kprintf("tar: invalid archive format\n");
            libc_free(buf);
            if (decompressed) libc_free(decompressed);
            return;
        }

        uint32_t pos = 4;
        int extracted = 0;
        while (pos + 8 <= data_size) {
            uint32_t name_len = *(uint32_t *)(data + pos);
            pos += 4;
            if (pos + name_len + 4 > data_size) break;

            char fname[64];
            uint32_t ncopy = name_len < 63 ? name_len : 63;
            memcpy(fname, data + pos, ncopy);
            fname[ncopy] = '\0';
            pos += name_len;

            if (pos + 4 > data_size) break;
            uint32_t fsize = *(uint32_t *)(data + pos);
            pos += 4;

            if (pos + fsize > data_size) break;

            /* Ensure path has leading slash */
            char outpath[128];
            if (fname[0] != '/') {
                outpath[0] = '/';
                strncpy(outpath + 1, fname, sizeof(outpath) - 2);
                outpath[sizeof(outpath) - 1] = '\0';
            } else {
                strncpy(outpath, fname, sizeof(outpath) - 1);
                outpath[sizeof(outpath) - 1] = '\0';
            }

            if (libc_vfs_write(outpath, data + pos, fsize) != 0) {
                kprintf("tar: failed to write %s\n", fname);
            } else {
                kprintf("  %s\n", fname);
                extracted++;
            }
            pos += fsize;
        }

        kprintf("Extracted %d files from %s\n", extracted, archive_name);
        libc_free(buf);
        if (decompressed) libc_free(decompressed);
        return;
    }

    /* ── CREATE MODE ─────────────────────────────────────────────── */
    if (file_count == 0) {
        kprintf("tar: no files to archive\n");
        return;
    }

    /* First pass: calculate total archive size */
    uint32_t sizes[32];
    uint32_t total = 4; /* magic */
    for (int i = 0; i < file_count; i++) {
        uint32_t sz = 0;
        char path[64];
        if (files[i][0] != '/') { path[0] = '/'; strncpy(path + 1, files[i], 62); path[63] = '\0'; }
        else { strncpy(path, files[i], 63); path[63] = '\0'; }

        if (libc_fs_read_file(path, NULL, 0, &sz) != 0) {
            kprintf("tar: Could not stat file %s, skipping\n", files[i]);
            sizes[i] = (uint32_t)-1;
            continue;
        }
        sizes[i] = sz;
        uint32_t name_len = strlen(files[i]);
        total += 4 + name_len + 4 + sz;
    }

    /* Allocate archive buffer */
    unsigned char *arch = (unsigned char *)libc_malloc(total);
    if (!arch) {
        kprintf("tar: out of memory\n");
        return;
    }

    /* Fill buffer */
    uint32_t pos = 0;
    memcpy(arch, TAR_MAGIC, 4);
    pos += 4;

    int archived = 0;
    for (int i = 0; i < file_count; i++) {
        if (sizes[i] == (uint32_t)-1) continue;

        char path[64];
        if (files[i][0] != '/') { path[0] = '/'; strncpy(path + 1, files[i], 62); path[63] = '\0'; }
        else { strncpy(path, files[i], 63); path[63] = '\0'; }

        uint32_t name_len = strlen(files[i]);
        memcpy(arch + pos, &name_len, 4); pos += 4;
        memcpy(arch + pos, files[i], name_len); pos += name_len;
        memcpy(arch + pos, &sizes[i], 4); pos += 4;

        uint32_t read_bytes = 0;
        if (libc_fs_read_file(path, arch + pos, sizes[i], &read_bytes) != 0) {
            kprintf("tar: Could not read file %s\n", files[i]);
            libc_free(arch);
            return;
        }
        pos += sizes[i];
        archived++;
    }

    unsigned char *out_data = arch;
    uint32_t out_size = pos;
    unsigned char *compressed = NULL;

    if (flag_z) {
        compressed = (unsigned char *)libc_malloc(TAR_BUF_SIZE);
        if (!compressed) {
            kprintf("tar: out of memory\n");
            libc_free(arch);
            return;
        }
        if (gzip_compress(arch, pos, compressed, TAR_BUF_SIZE - 1, &out_size) != 0) {
            kprintf("tar: gzip compression failed\n");
            libc_free(arch);
            libc_free(compressed);
            return;
        }
        out_data = compressed;
    }

    libc_vfs_write(archive_name, out_data, out_size);
    kprintf("Archive %s created successfully with %d files (%s)\n",
            archive_name, archived, flag_z ? "gzip compressed" : "uncompressed");

    libc_free(arch);
    if (compressed) libc_free(compressed);
}
