/* cpio.c — Copy-in/copy-out archiver (SVR4 cpio format) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/stat.h"

/* SVR4 cpio header (new format) — 110 bytes */
struct cpio_newc_header {
    char c_magic[6];      /* "070701" */
    char c_ino[8];
    char c_mode[8];
    char c_uid[8];
    char c_gid[8];
    char c_nlink[8];
    char c_mtime[8];
    char c_filesize[8];
    char c_devmajor[8];
    char c_devminor[8];
    char c_rdevmajor[8];
    char c_rdevminor[8];
    char c_namesize[8];
    char c_check[8];      /* "00000000" */
} __attribute__((packed));

#define CPIO_MAGIC "070701"
#define CPIO_TRAILER "TRAILER!!!"
#define CPIO_ALIGN 4

/* Helper: write a padded string to fd, return bytes written */
static int cpio_write_padded(int fd, const void *data, unsigned long len, unsigned long align) {
    int total = 0;
    int w = write(fd, data, len);
    if (w < 0) return -1;
    total += w;
    /* Write padding zeros */
    unsigned long pad = (align - (len % align)) % align;
    if (pad > 0) {
        char zeros[4];
        memset(zeros, 0, pad);
        w = write(fd, zeros, pad);
        if (w < 0) return -1;
        total += w;
    }
    return total;
}

/* Helper: parse hex field to unsigned long long */
static unsigned long long cpio_hex_parse(const char *buf, int len) {
    unsigned long long val = 0;
    for (int i = 0; i < len && buf[i]; i++) {
        char c = buf[i];
        if (c >= '0' && c <= '9')
            val = (val << 4) | (c - '0');
        else if (c >= 'a' && c <= 'f')
            val = (val << 4) | (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            val = (val << 4) | (c - 'A' + 10);
        else
            break;
    }
    return val;
}

/* Helper: format hex field (8 hex digits) */
static void cpio_hex_format(unsigned long long val, char *buf, int len) {
    buf[len - 1] = 0;
    for (int i = len - 2; i >= 0; i--) {
        int digit = val & 0xf;
        buf[i] = digit < 10 ? '0' + digit : 'a' + digit - 10;
        val >>= 4;
    }
}

/* Copy-out mode: read file list from stdin, write archive to stdout */
static int cpio_copy_out(void) {
    char line[4096];

    while (1) {
        /* Read one filename from stdin */
        int pos = 0;
        int c;
        while (pos < (int)sizeof(line) - 1) {
            if (read(0, &c, 1) <= 0) {
                if (pos == 0) goto done;
                break;
            }
            if (c == '\n') break;
            line[pos++] = c;
        }
        line[pos] = 0;
        if (pos == 0) break;

        struct stat st;
        if (stat(line, &st) < 0) {
            /* Silently skip unstatable files */
            continue;
        }

        /* Build header */
        struct cpio_newc_header hdr;
        memset(&hdr, 0, sizeof(hdr));
        memcpy(hdr.c_magic, CPIO_MAGIC, 6);
        cpio_hex_format(st.st_ino, hdr.c_ino, 8);
        cpio_hex_format(st.st_mode, hdr.c_mode, 8);
        cpio_hex_format(st.st_uid, hdr.c_uid, 8);
        cpio_hex_format(st.st_gid, hdr.c_gid, 8);
        cpio_hex_format(st.st_nlink, hdr.c_nlink, 8);
        cpio_hex_format(st.st_mtime, hdr.c_mtime, 8);
        cpio_hex_format(st.st_size, hdr.c_filesize, 8);
        cpio_hex_format(0, hdr.c_devmajor, 8);
        cpio_hex_format(0, hdr.c_devminor, 8);
        cpio_hex_format(0, hdr.c_rdevmajor, 8);
        cpio_hex_format(0, hdr.c_rdevminor, 8);
        unsigned long name_len = strlen(line) + 1;
        cpio_hex_format(name_len, hdr.c_namesize, 8);
        memcpy(hdr.c_check, "00000000", 8);

        /* Write header */
        if (write(1, &hdr, sizeof(hdr)) != sizeof(hdr)) {
            printf("cpio: write error\n");
            return 1;
        }

        /* Write filename with padding */
        if (cpio_write_padded(1, line, name_len, CPIO_ALIGN) < 0) {
            printf("cpio: write error\n");
            return 1;
        }

        /* Write file data with padding */
        if (S_ISREG(st.st_mode) && st.st_size > 0) {
            int infd = open(line, O_RDONLY, 0);
            if (infd < 0) continue;
            char buf[8192];
            int n;
            while ((n = read(infd, buf, sizeof(buf))) > 0) {
                if (write(1, buf, n) != n) {
                    close(infd);
                    printf("cpio: write error\n");
                    return 1;
                }
            }
            close(infd);
            /* Write padding */
            unsigned long pad = (CPIO_ALIGN - ((unsigned long)st.st_size % CPIO_ALIGN)) % CPIO_ALIGN;
            if (pad > 0) {
                char zeros[4];
                memset(zeros, 0, pad);
                write(1, zeros, pad);
            }
        }
    }

done:
    /* Write trailer */
    {
        struct cpio_newc_header hdr;
        memset(&hdr, 0, sizeof(hdr));
        memcpy(hdr.c_magic, CPIO_MAGIC, 6);
        cpio_hex_format(0, hdr.c_mode, 8);
        cpio_hex_format(0, hdr.c_uid, 8);
        cpio_hex_format(0, hdr.c_gid, 8);
        cpio_hex_format(1, hdr.c_nlink, 8);
        unsigned long tnamelen = strlen(CPIO_TRAILER) + 1;
        cpio_hex_format(tnamelen, hdr.c_namesize, 8);
        memcpy(hdr.c_check, "00000000", 8);
        if (write(1, &hdr, sizeof(hdr)) != sizeof(hdr)) return 1;
        if (cpio_write_padded(1, CPIO_TRAILER, tnamelen, CPIO_ALIGN) < 0) return 1;
    }

    return 0;
}

/* Copy-in mode: read archive from stdin, extract files */
static int cpio_copy_in(void) {
    char buf[512];

    for (;;) {
        /* Read header */
        struct cpio_newc_header hdr;
        int n = read(0, &hdr, sizeof(hdr));
        if (n <= 0) break;
        if (n < (int)sizeof(hdr)) {
            printf("cpio: truncated header\n");
            return 1;
        }

        /* Check magic */
        if (memcmp(hdr.c_magic, CPIO_MAGIC, 6) != 0) {
            printf("cpio: bad magic\n");
            return 1;
        }

        unsigned long long namesize = cpio_hex_parse(hdr.c_namesize, 8);
        unsigned long long filesize = cpio_hex_parse(hdr.c_filesize, 8);
        unsigned long long mode = cpio_hex_parse(hdr.c_mode, 8);

        /* Read filename */
        char name[4096];
        if (namesize >= sizeof(name)) {
            printf("cpio: filename too long\n");
            return 1;
        }
        n = read(0, name, namesize);
        if (n < (int)namesize) break;
        name[namesize] = 0;

        /* Skip padding after name */
        unsigned long name_pad = (CPIO_ALIGN - ((unsigned long)namesize % CPIO_ALIGN)) % CPIO_ALIGN;
        while (name_pad > 0) {
            int r = read(0, buf, name_pad < sizeof(buf) ? name_pad : sizeof(buf));
            if (r <= 0) break;
            name_pad -= r;
        }

        /* Check for trailer */
        if (strcmp(name, CPIO_TRAILER) == 0)
            break;

        if (S_ISDIR(mode)) {
            mkdir(name, (int)(mode & 07777));
        } else if (S_ISREG(mode)) {
            int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, (int)(mode & 07777));
            if (fd < 0) {
                printf("cpio: cannot create '%s'\n", name);
            }
            char databuf[8192];
            unsigned long long remaining = filesize;
            while (remaining > 0) {
                unsigned long long chunk = remaining > sizeof(databuf) ? sizeof(databuf) : remaining;
                int r = read(0, databuf, chunk);
                if (r <= 0) break;
                if (fd >= 0)
                    write(fd, databuf, r);
                remaining -= r;
            }
            if (fd >= 0)
                close(fd);

            /* Skip padding */
            unsigned long data_pad = (CPIO_ALIGN - ((unsigned long)filesize % CPIO_ALIGN)) % CPIO_ALIGN;
            while (data_pad > 0) {
                int r = read(0, buf, data_pad < sizeof(buf) ? data_pad : sizeof(buf));
                if (r <= 0) break;
                data_pad -= r;
            }
        } else {
            /* Skip file data */
            unsigned long long remaining = filesize;
            char skipbuf[8192];
            while (remaining > 0) {
                unsigned long long chunk = remaining > sizeof(skipbuf) ? sizeof(skipbuf) : remaining;
                int r = read(0, skipbuf, chunk);
                if (r <= 0) break;
                remaining -= r;
            }
            unsigned long data_pad = (CPIO_ALIGN - ((unsigned long)filesize % CPIO_ALIGN)) % CPIO_ALIGN;
            while (data_pad > 0) {
                int r = read(0, buf, data_pad < sizeof(buf) ? data_pad : sizeof(buf));
                if (r <= 0) break;
                data_pad -= r;
            }
        }
    }

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: cpio -o < filelist    (create archive on stdout)\n");
        printf("       cpio -i                (extract archive from stdin)\n");
        return 1;
    }

    if (strcmp(argv[1], "-o") == 0) {
        return cpio_copy_out();
    }

    if (strcmp(argv[1], "-i") == 0) {
        return cpio_copy_in();
    }

    printf("cpio: unknown option '%s'\n", argv[1]);
    return 1;
}
