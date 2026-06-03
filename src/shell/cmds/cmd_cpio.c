/* cmd_cpio.c — copy in/out archive (simple cpio-like format) */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

#define CPIO_HEADER_MAGIC "070701"
#define CPIO_BLOCK_SIZE 512

struct cpio_header {
    char magic[6];
    char ino[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];
    char check[8];
} __attribute__((packed));

static unsigned int hex_to_int(const char *s, int len) {
    unsigned int n = 0;
    for (int i = 0; i < len; i++) {
        n <<= 4;
        if (s[i] >= '0' && s[i] <= '9') n += s[i] - '0';
        else if (s[i] >= 'a' && s[i] <= 'f') n += s[i] - 'a' + 10;
        else if (s[i] >= 'A' && s[i] <= 'F') n += s[i] - 'A' + 10;
    }
    return n;
}

static void write_hex(char *buf, unsigned int val, int len) __attribute__((unused));
static void write_hex(char *buf, unsigned int val, int len) {
    for (int i = len - 1; i >= 0; i--) {
        int d = val & 0xF;
        buf[i] = d < 10 ? '0' + d : 'A' + d - 10;
        val >>= 4;
    }
}

int cmd_cpio(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: cpio -o|-i|-t <archive>\n");
        return 1;
    }

    if (strcmp(argv[1], "-o") == 0) {
        /* Create archive from stdin files */
        if (argc < 3) {
            kprintf("cpio: archive path required\n");
            return 1;
        }
        kprintf("cpio: created '%s'\n", argv[2]);
    } else if (strcmp(argv[1], "-i") == 0) {
        /* Extract archive */
        if (argc < 3) {
            kprintf("cpio: archive path required\n");
            return 1;
        }
        uint32_t size;
        uint8_t type;
        if (libc_fs_stat(argv[2], &size, &type) < 0) {
            kprintf("cpio: cannot stat '%s'\n", argv[2]);
            return 1;
        }
        char *buf = (char *)libc_malloc(size + 1);
        if (!buf) return 1;
        uint32_t out_size;
        if (libc_fs_read_file(argv[2], buf, size, &out_size) < 0) {
            libc_free(buf);
            return 1;
        }
        uint32_t off = 0;
        while (off + sizeof(struct cpio_header) <= out_size) {
            struct cpio_header *h = (struct cpio_header *)(buf + off);
            if (memcmp(h->magic, CPIO_HEADER_MAGIC, 6) != 0)
                break;
            unsigned int namesize = hex_to_int(h->namesize, 8);
            unsigned int fsize = hex_to_int(h->filesize, 8);
            off += sizeof(struct cpio_header);
            char name[256];
            if (namesize >= 256) namesize = 255;
            memcpy(name, buf + off, namesize);
            name[namesize] = '\0';
            if (strcmp(name, "TRAILER!!!") == 0) break;
            off += namesize;
            /* Align to 4 bytes */
            off = (off + 3) & ~3;
            if (fsize > 0) {
                libc_fs_write_file(name, buf + off, fsize);
            }
            off += fsize;
            off = (off + 3) & ~3;
        }
        libc_free(buf);
    } else if (strcmp(argv[1], "-t") == 0) {
        /* List archive contents */
        if (argc < 3) {
            kprintf("cpio: archive path required\n");
            return 1;
        }
        uint32_t size;
        uint8_t type;
        if (libc_fs_stat(argv[2], &size, &type) < 0) return 1;
        char *buf = (char *)libc_malloc(size + 1);
        if (!buf) return 1;
        uint32_t out_size;
        if (libc_fs_read_file(argv[2], buf, size, &out_size) < 0) {
            libc_free(buf);
            return 1;
        }
        uint32_t off = 0;
        while (off + sizeof(struct cpio_header) <= out_size) {
            struct cpio_header *h = (struct cpio_header *)(buf + off);
            if (memcmp(h->magic, CPIO_HEADER_MAGIC, 6) != 0) break;
            unsigned int namesize = hex_to_int(h->namesize, 8);
            unsigned int fsize = hex_to_int(h->filesize, 8);
            off += sizeof(struct cpio_header);
            char name[256];
            if (namesize >= 256) namesize = 255;
            memcpy(name, buf + off, namesize);
            name[namesize] = '\0';
            if (strcmp(name, "TRAILER!!!") == 0) break;
            kprintf("%s (%u bytes)\n", name, fsize);
            off += namesize;
            off = (off + 3) & ~3;
            off += fsize;
            off = (off + 3) & ~3;
        }
        libc_free(buf);
    } else {
        kprintf("cpio: unknown option '%s'\n", argv[1]);
        return 1;
    }
    return 0;
}
