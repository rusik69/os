/* pax.c — POSIX interchange archive utility (tar ustar format) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/stat.h"

/* Ustar tar header — 512 bytes */
struct ustar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];      /* "ustar\0" */
    char version[2];    /* "00" */
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} __attribute__((packed));

#define TAR_BLOCK_SIZE 512
#define TAR_MAGIC "ustar"

/* Type flags */
#define REGTYPE  '0'
#define AREGTYPE '\0'
#define DIRTYPE  '5'

static void octal_format(unsigned long long val, char *buf, int len) {
    buf[len - 1] = '\0';
    int i = len - 2;
    while (i >= 0) {
        buf[i] = '0' + (val & 7);
        val >>= 3;
        i--;
        if (val == 0 && i < 0) break;
    }
    while (i >= 0) {
        buf[i] = '0';
        i--;
    }
}

static unsigned long long octal_parse(const char *buf, int len) {
    unsigned long long val = 0;
    for (int i = 0; i < len && buf[i]; i++) {
        if (buf[i] >= '0' && buf[i] <= '7')
            val = (val << 3) | (buf[i] - '0');
        else
            break;
    }
    return val;
}

static int write_tar_header(const char *path, struct stat *st) {
    struct ustar_header hdr;
    memset(&hdr, 0, sizeof(hdr));

    /* Name — basename only (no path) */
    const char *bname = path;
    const char *slash = strrchr(path, '/');
    if (slash) bname = slash + 1;
    unsigned long nlen = strlen(bname);
    if (nlen > 99) nlen = 99;
    memcpy(hdr.name, bname, nlen);

    /* Mode */
    octal_format(st->st_mode & 07777, hdr.mode, 8);

    /* UID / GID */
    octal_format(st->st_uid, hdr.uid, 8);
    octal_format(st->st_gid, hdr.gid, 8);

    /* Size */
    octal_format(st->st_size, hdr.size, 12);

    /* Mtime */
    octal_format(st->st_mtime, hdr.mtime, 12);

    /* Type flag */
    if (S_ISDIR(st->st_mode))
        hdr.typeflag = DIRTYPE;
    else
        hdr.typeflag = REGTYPE;

    /* Magic */
    memcpy(hdr.magic, TAR_MAGIC, 5);
    hdr.magic[5] = '\0';
    hdr.version[0] = '0';
    hdr.version[1] = '0';

    /* Compute checksum: fill field with spaces, sum all bytes, then store */
    memset(hdr.chksum, ' ', 8);
    int cksum = 0;
    unsigned char *p = (unsigned char *)&hdr;
    for (int i = 0; i < TAR_BLOCK_SIZE; i++)
        cksum += p[i];
    octal_format(cksum, hdr.chksum, 7);
    /* End with space (traditional ustar format) */
    hdr.chksum[7] = ' ';

    return write(1, &hdr, TAR_BLOCK_SIZE);
}

static int pad_to_block(unsigned long long written) {
    unsigned long long pad = (TAR_BLOCK_SIZE - (written % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;
    char zeros[512];
    memset(zeros, 0, pad);
    if (pad > 0)
        return write(1, zeros, pad);
    return 0;
}

static int write_tar_end(void) {
    char zeros[1024];
    memset(zeros, 0, 1024);
    return write(1, zeros, 1024);
}

static int pax_write(int argc, char *argv[], int first_file) {
    for (int i = first_file; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            printf("pax: cannot stat '%s'\n", argv[i]);
            return 1;
        }

        /* Write header */
        if (write_tar_header(argv[i], &st) != TAR_BLOCK_SIZE) {
            printf("pax: write error\n");
            return 1;
        }

        /* Write file data if regular file */
        if (S_ISREG(st.st_mode) && st.st_size > 0) {
            int fd = open(argv[i], O_RDONLY, 0);
            if (fd < 0) {
                printf("pax: cannot open '%s'\n", argv[i]);
                return 1;
            }
            char buf[8192];
            unsigned long long total = 0;
            int n;
            while ((n = read(fd, buf, sizeof(buf))) > 0) {
                if (write(1, buf, n) != n) {
                    close(fd);
                    printf("pax: write error\n");
                    return 1;
                }
                total += n;
            }
            close(fd);

            /* Pad to block boundary */
            if (pad_to_block(total) < 0)
                return 1;
        }
    }

    /* End of archive: two zero blocks */
    if (write_tar_end() < 0)
        return 1;

    return 0;
}

static int pax_read(void) {
    char block[TAR_BLOCK_SIZE];
    int end_blocks = 0;

    while (1) {
        memset(block, 0, TAR_BLOCK_SIZE);
        int n = read(0, block, TAR_BLOCK_SIZE);
        if (n <= 0) break;

        /* Only check the bytes we actually read for zero */
        int check_len = n < TAR_BLOCK_SIZE ? n : TAR_BLOCK_SIZE;
        int is_zero = 1;
        for (int i = 0; i < check_len; i++) {
            if (block[i] != 0) { is_zero = 0; break; }
        }

        if (is_zero) {
            end_blocks++;
            if (end_blocks >= 2) break;
            continue;
        }
        end_blocks = 0;

        struct ustar_header *hdr = (struct ustar_header *)block;

        /* Validate magic */
        if (memcmp(hdr->magic, TAR_MAGIC, 5) != 0) {
            printf("pax: invalid tar header (bad magic)\n");
            return 1;
        }

        /* Extract file name */
        char name[256];
        unsigned long plen = strlen(hdr->prefix);
        unsigned long nlen = strlen(hdr->name);
        if (plen > 0 && plen + 1 + nlen < 256) {
            memcpy(name, hdr->prefix, plen);
            name[plen] = '/';
            memcpy(name + plen + 1, hdr->name, nlen + 1);
        } else if (nlen < 256) {
            memcpy(name, hdr->name, nlen + 1);
        } else {
            printf("pax: filename too long\n");
            return 1;
        }

        /* Parse size */
        unsigned long long file_size = octal_parse(hdr->size, 12);

        /* Mode */
        unsigned int mode = (unsigned int)octal_parse(hdr->mode, 8);

        /* Handle type */
        if (hdr->typeflag == DIRTYPE || hdr->typeflag == '5') {
            mkdir(name, mode);
            continue;
        }

        /* Regular file */
        int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, (int)mode & 0777);
        if (fd < 0) {
            printf("pax: cannot create '%s'\n", name);
            return 1;
        }

        /* Read file data */
        unsigned long long remaining = file_size;
        char databuf[8192];
        while (remaining > 0) {
            unsigned long long to_read = remaining;
            if (to_read > sizeof(databuf)) to_read = sizeof(databuf);
            int r = read(0, databuf, to_read);
            if (r <= 0) break;
            write(fd, databuf, r);
            remaining -= r;
        }
        close(fd);

        /* Skip padding to next block boundary */
        unsigned long long padded = ((file_size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE) * TAR_BLOCK_SIZE;
        unsigned long long skip = padded - file_size;
        while (skip > 0) {
            int r = read(0, databuf, skip < sizeof(databuf) ? skip : sizeof(databuf));
            if (r <= 0) break;
            skip -= r;
        }
    }

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: pax -w <files>    (write archive to stdout)\n");
        printf("       pax -r            (read/extract archive from stdin)\n");
        return 1;
    }

    if (strcmp(argv[1], "-w") == 0) {
        if (argc < 3) {
            printf("pax: -w requires file arguments\n");
            return 1;
        }
        return pax_write(argc, argv, 2);
    }

    if (strcmp(argv[1], "-r") == 0) {
        return pax_read();
    }

    printf("pax: unknown option '%s'\n", argv[1]);
    return 1;
}
