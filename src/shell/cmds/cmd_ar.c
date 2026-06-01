/* cmd_ar.c — archive utility (simple tar-like format) */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

#define BLOCK_SIZE 512
#define AR_NAME_LEN 100

struct ar_header {
    char name[AR_NAME_LEN];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char type;
    char linkname[AR_NAME_LEN];
    char zero_pad[255];
} __attribute__((packed));

static unsigned int octal_to_int(const char *s, int len) {
    unsigned int n = 0;
    for (int i = 0; i < len && s[i] >= '0' && s[i] <= '7'; i++)
        n = n * 8 + (s[i] - '0');
    return n;
}

static void write_octal(char *buf, unsigned int val, int len) {
    for (int i = len - 1; i >= 0; i--) {
        buf[i] = (val % 8) + '0';
        val /= 8;
    }
}

int cmd_ar(int argc, char **argv) {
    if (argc < 3) {
        kprintf("Usage: ar {c|x|t} <archive> [files...]\n");
        return 1;
    }
    char op = argv[1][0];
    const char *archive = argv[2];

    if (op == 'c') {
        /* Create archive */
        int fd = libc_vfs_create(archive, 1); /* type=file */
        if (fd < 0) fd = 0;
        /* Write end-of-archive marker (two zero blocks) */
        char zero[BLOCK_SIZE];
        memset(zero, 0, BLOCK_SIZE);
        libc_fs_write_file(archive, zero, BLOCK_SIZE);
        libc_fs_write_file(archive, zero, BLOCK_SIZE);
        kprintf("ar: created '%s'\n", archive);
    } else if (op == 't') {
        /* List archive contents */
        uint32_t size;
        uint8_t type;
        if (libc_fs_stat(archive, &size, &type) < 0) {
            kprintf("ar: cannot stat '%s'\n", archive);
            return 1;
        }
        char *buf = (char *)libc_malloc(size + 1);
        if (!buf) return 1;
        uint32_t out_size;
        if (libc_fs_read_file(archive, buf, size, &out_size) < 0) {
            libc_free(buf);
            return 1;
        }
        uint32_t off = 0;
        while (off + BLOCK_SIZE <= out_size) {
            struct ar_header *h = (struct ar_header *)(buf + off);
            if (h->name[0] == '\0') break;
            unsigned int fsize = octal_to_int(h->size, 12);
            kprintf("%s\n", h->name);
            off += BLOCK_SIZE;
            off += ((fsize + BLOCK_SIZE - 1) / BLOCK_SIZE) * BLOCK_SIZE;
        }
        libc_free(buf);
    } else if (op == 'x') {
        /* Extract archive */
        uint32_t size;
        uint8_t type;
        if (libc_fs_stat(archive, &size, &type) < 0) return 1;
        char *buf = (char *)libc_malloc(size + 1);
        if (!buf) return 1;
        uint32_t out_size;
        if (libc_fs_read_file(archive, buf, size, &out_size) < 0) {
            libc_free(buf);
            return 1;
        }
        uint32_t off = 0;
        while (off + BLOCK_SIZE <= out_size) {
            struct ar_header *h = (struct ar_header *)(buf + off);
            if (h->name[0] == '\0') break;
            unsigned int fsize = octal_to_int(h->size, 12);
            off += BLOCK_SIZE;
            if (fsize > 0 && off + fsize <= out_size) {
                libc_fs_write_file(h->name, buf + off, fsize);
            }
            off += ((fsize + BLOCK_SIZE - 1) / BLOCK_SIZE) * BLOCK_SIZE;
        }
        libc_free(buf);
    } else {
        kprintf("ar: unknown operation '%c'\n", op);
        return 1;
    }
    return 0;
}
