/* tar.c — Tape archiver (ustar format) */
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

/* Create (tar -cf archive files...) */
static int tar_create(const char *archive, int argc, char *argv[], int first_file) {
    int fd = open(archive, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("tar: cannot create '%s'\n", archive);
        return 1;
    }

    for (int i = first_file; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            printf("tar: cannot stat '%s'\n", argv[i]);
            close(fd);
            return 1;
        }

        struct ustar_header hdr;
        memset(&hdr, 0, sizeof(hdr));

        /* Name */
        const char *bname = argv[i];
        const char *slash = strrchr(argv[i], '/');
        if (slash) bname = slash + 1;
        unsigned long nlen = strlen(bname);
        if (nlen > 99) nlen = 99;
        memcpy(hdr.name, bname, nlen);

        octal_format(st.st_mode & 07777, hdr.mode, 8);
        octal_format(st.st_uid, hdr.uid, 8);
        octal_format(st.st_gid, hdr.gid, 8);
        octal_format(st.st_size, hdr.size, 12);
        octal_format(st.st_mtime, hdr.mtime, 12);

        if (S_ISDIR(st.st_mode))
            hdr.typeflag = DIRTYPE;
        else
            hdr.typeflag = REGTYPE;

        memcpy(hdr.magic, TAR_MAGIC, 5);
        hdr.magic[5] = '\0';
        hdr.version[0] = '0';
        hdr.version[1] = '0';

        /* Compute checksum */
        memset(hdr.chksum, ' ', 8);
        int cksum = 0;
        unsigned char *p = (unsigned char *)&hdr;
        for (int j = 0; j < TAR_BLOCK_SIZE; j++)
            cksum += p[j];
        octal_format(cksum, hdr.chksum, 7);
        hdr.chksum[7] = ' ';

        if (write(fd, &hdr, TAR_BLOCK_SIZE) != TAR_BLOCK_SIZE) {
            printf("tar: write error\n");
            close(fd);
            return 1;
        }

        if (S_ISREG(st.st_mode) && st.st_size > 0) {
            int infd = open(argv[i], O_RDONLY, 0);
            if (infd < 0) {
                printf("tar: cannot open '%s'\n", argv[i]);
                close(fd);
                return 1;
            }
            char buf[8192];
            unsigned long long total = 0;
            int n;
            while ((n = read(infd, buf, sizeof(buf))) > 0) {
                if (write(fd, buf, n) != n) {
                    close(infd);
                    close(fd);
                    printf("tar: write error\n");
                    return 1;
                }
                total += n;
            }
            close(infd);

            /* Pad to block boundary */
            unsigned long long pad = (TAR_BLOCK_SIZE - (total % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;
            if (pad > 0) {
                char zeros[512];
                memset(zeros, 0, pad);
                write(fd, zeros, pad);
            }
        }
    }

    /* End of archive: two zero blocks */
    char zeros[1024];
    memset(zeros, 0, 1024);
    write(fd, zeros, 1024);

    close(fd);
    return 0;
}

/* Extract (tar -xf archive) */
static int tar_extract(const char *archive) {
    int fd = open(archive, O_RDONLY, 0);
    if (fd < 0) {
        printf("tar: cannot open '%s'\n", archive);
        return 1;
    }

    char block[TAR_BLOCK_SIZE];
    int end_blocks = 0;

    while (1) {
        memset(block, 0, TAR_BLOCK_SIZE);
        int n = read(fd, block, TAR_BLOCK_SIZE);
        if (n <= 0) break;

        int is_zero = 1;
        for (int i = 0; i < TAR_BLOCK_SIZE; i++) {
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
            printf("tar: invalid tar header (bad magic)\n");
            close(fd);
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
            printf("tar: filename too long\n");
            close(fd);
            return 1;
        }

        unsigned long long file_size = octal_parse(hdr->size, 12);
        unsigned int mode = (unsigned int)octal_parse(hdr->mode, 8);

        if (hdr->typeflag == DIRTYPE || hdr->typeflag == '5') {
            mkdir(name, mode);
            continue;
        }

        int outfd = open(name, O_WRONLY | O_CREAT | O_TRUNC, (int)mode & 0777);
        if (outfd < 0) {
            printf("tar: cannot create '%s'\n", name);
        }

        unsigned long long remaining = file_size;
        char databuf[8192];
        while (remaining > 0) {
            unsigned long long to_read = remaining;
            if (to_read > sizeof(databuf)) to_read = sizeof(databuf);
            int r = read(fd, databuf, to_read);
            if (r <= 0) break;
            if (outfd >= 0)
                write(outfd, databuf, r);
            remaining -= r;
        }
        if (outfd >= 0)
            close(outfd);

        /* Skip padding */
        unsigned long long padded = ((file_size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE) * TAR_BLOCK_SIZE;
        unsigned long long skip = padded - file_size;
        while (skip > 0) {
            int r = read(fd, databuf, skip < sizeof(databuf) ? skip : sizeof(databuf));
            if (r <= 0) break;
            skip -= r;
        }
    }

    close(fd);
    return 0;
}

/* List (tar -tf archive) */
static int tar_list(const char *archive) {
    int fd = open(archive, O_RDONLY, 0);
    if (fd < 0) {
        printf("tar: cannot open '%s'\n", archive);
        return 1;
    }

    char block[TAR_BLOCK_SIZE];
    int end_blocks = 0;

    while (1) {
        memset(block, 0, TAR_BLOCK_SIZE);
        int n = read(fd, block, TAR_BLOCK_SIZE);
        if (n <= 0) break;

        int is_zero = 1;
        for (int i = 0; i < TAR_BLOCK_SIZE; i++) {
            if (block[i] != 0) { is_zero = 0; break; }
        }

        if (is_zero) {
            end_blocks++;
            if (end_blocks >= 2) break;
            continue;
        }
        end_blocks = 0;

        struct ustar_header *hdr = (struct ustar_header *)block;

        if (memcmp(hdr->magic, TAR_MAGIC, 5) != 0) {
            printf("tar: invalid tar header (bad magic)\n");
            close(fd);
            return 1;
        }

        /* Print name */
        if (strlen(hdr->prefix) > 0) {
            printf("%s/", hdr->prefix);
        }
        printf("%s\n", hdr->name);

        /* Skip file data */
        unsigned long long file_size = octal_parse(hdr->size, 12);
        unsigned long long padded = ((file_size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE) * TAR_BLOCK_SIZE;
        unsigned long long skip = padded;
        char skipbuf[8192];
        while (skip > 0) {
            int r = read(fd, skipbuf, skip < sizeof(skipbuf) ? skip : sizeof(skipbuf));
            if (r <= 0) break;
            skip -= r;
        }
    }

    close(fd);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("usage: tar -cf <archive> <files>   (create)\n");
        printf("       tar -xf <archive>            (extract)\n");
        printf("       tar -tf <archive>            (list)\n");
        return 1;
    }

    if (strcmp(argv[1], "-cf") == 0) {
        if (argc < 4) {
            printf("tar: -cf requires archive and file arguments\n");
            return 1;
        }
        return tar_create(argv[2], argc, argv, 3);
    }

    if (strcmp(argv[1], "-xf") == 0) {
        return tar_extract(argv[2]);
    }

    if (strcmp(argv[1], "-tf") == 0) {
        return tar_list(argv[2]);
    }

    printf("tar: unknown option '%s'\n", argv[1]);
    return 1;
}
