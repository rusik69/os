/* ar.c — Archive creation and extraction (BSD ar format) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/stat.h"

#define AR_MAGIC "!<arch>\n"
#define AR_FMAGIC "`\n"
#define AR_HDRSIZE 60

/* BSD ar header — 60 bytes */
struct ar_hdr {
    char ar_name[16];
    char ar_date[12];
    char ar_uid[6];
    char ar_gid[6];
    char ar_mode[8];
    char ar_size[10];
    char ar_fmag[2];
} __attribute__((packed));

/* Format decimal value into field: right-justified, space-padded, no null */
static void fmt_decimal(char *buf, int buflen, unsigned long long val) {
    /* Write digits from right */
    char tmp[32];
    int pos = 0;
    if (val == 0) {
        tmp[pos++] = '0';
    } else {
        while (val > 0 && pos < 30) {
            tmp[pos++] = '0' + (val % 10);
            val /= 10;
        }
    }
    /* Copy into buf, space-padded on left */
    int i;
    for (i = 0; i < buflen; i++) {
        int src = pos - 1 - (buflen - 1 - i);
        if (src >= 0 && src < pos)
            buf[i] = tmp[src];
        else
            buf[i] = ' ';
    }
}

/* Format octal value into field: right-justified, space-padded */
static void fmt_octal(char *buf, int buflen, unsigned long long val) {
    char tmp[32];
    int pos = 0;
    if (val == 0) {
        tmp[pos++] = '0';
    } else {
        while (val > 0 && pos < 30) {
            tmp[pos++] = '0' + (val & 7);
            val >>= 3;
        }
    }
    for (int i = 0; i < buflen; i++) {
        int src = pos - 1 - (buflen - 1 - i);
        if (src >= 0 && src < pos)
            buf[i] = tmp[src];
        else
            buf[i] = ' ';
    }
}

/* Parse decimal from space-padded field */
static unsigned long long parse_decimal(const char *buf, int len) {
    unsigned long long val = 0;
    for (int i = 0; i < len; i++) {
        if (buf[i] >= '0' && buf[i] <= '9')
            val = val * 10 + (buf[i] - '0');
        else if (val > 0)
            break; /* stop at first space after number */
    }
    return val;
}

static int ar_read_entries(int fd, int extract, int list);
static int ar_write_archive(const char *archive, int argc, char *argv[], int first_file);

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("usage: ar -r <archive> <files>   (replace/insert)\n");
        printf("       ar -t <archive>            (list contents)\n");
        printf("       ar -x <archive>            (extract)\n");
        return 1;
    }

    if (strcmp(argv[1], "-r") == 0) {
        if (argc < 4) {
            printf("ar: -r requires archive and file arguments\n");
            return 1;
        }
        return ar_write_archive(argv[2], argc, argv, 3);
    }

    if (strcmp(argv[1], "-t") == 0) {
        int fd = open(argv[2], O_RDONLY, 0);
        if (fd < 0) {
            printf("ar: cannot open '%s'\n", argv[2]);
            return 1;
        }
        int ret = ar_read_entries(fd, 0, 1);
        close(fd);
        return ret;
    }

    if (strcmp(argv[1], "-x") == 0) {
        int fd = open(argv[2], O_RDONLY, 0);
        if (fd < 0) {
            printf("ar: cannot open '%s'\n", argv[2]);
            return 1;
        }
        int ret = ar_read_entries(fd, 1, 0);
        close(fd);
        return ret;
    }

    printf("ar: unknown option '%s'\n", argv[1]);
    return 1;
}

static int ar_write_archive(const char *archive, int argc, char *argv[], int first_file) {
    int fd = open(archive, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("ar: cannot create '%s'\n", archive);
        return 1;
    }

    /* Write magic */
    if (write(fd, AR_MAGIC, 8) != 8) {
        printf("ar: write error\n");
        close(fd);
        return 1;
    }

    for (int i = first_file; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            printf("ar: cannot stat '%s'\n", argv[i]);
            close(fd);
            return 1;
        }

        /* Use basename */
        const char *bname = argv[i];
        const char *slash = strrchr(argv[i], '/');
        if (slash) bname = slash + 1;
        unsigned long nlen = strlen(bname);

        struct ar_hdr hdr;
        memset(&hdr, 0, sizeof(hdr));

        /* Name field: if fits in 15 chars + '/', use directly; else BSD long format */
        if (nlen <= 15) {
            memcpy(hdr.ar_name, bname, nlen);
            hdr.ar_name[nlen] = '/';
        } else {
            /* BSD long name: "#1/<len>" — name precedes data */
            char nbuf[16];
            int ni = 0;
            nbuf[ni++] = '#';
            nbuf[ni++] = '1';
            nbuf[ni++] = '/';
            /* Convert length to decimal */
            unsigned long ln = nlen;
            char ltmp[16];
            int lp = 0;
            if (ln == 0) ltmp[lp++] = '0';
            while (ln > 0 && lp < 14) {
                ltmp[lp++] = '0' + (ln % 10);
                ln /= 10;
            }
            for (int j = lp - 1; j >= 0 && ni < 15; j--)
                nbuf[ni++] = ltmp[j];
            nbuf[ni] = 0;
            memcpy(hdr.ar_name, nbuf, ni);
        }

        /* Date field */
        fmt_decimal(hdr.ar_date, 12, st.st_mtime);

        /* UID, GID */
        fmt_decimal(hdr.ar_uid, 6, st.st_uid);
        fmt_decimal(hdr.ar_gid, 6, st.st_gid);

        /* Mode: octal */
        fmt_octal(hdr.ar_mode, 8, st.st_mode & 07777);

        /* Size: includes name if using BSD long format */
        unsigned long long total = st.st_size;
        if (nlen > 15)
            total += nlen;
        fmt_decimal(hdr.ar_size, 10, total);

        /* Magic trailer */
        hdr.ar_fmag[0] = AR_FMAGIC[0];
        hdr.ar_fmag[1] = AR_FMAGIC[1];

        if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
            printf("ar: write error\n");
            close(fd);
            return 1;
        }

        /* Write BSD long name before data if needed */
        if (nlen > 15) {
            if (write(fd, bname, nlen) != (int)nlen) {
                printf("ar: write error\n");
                close(fd);
                return 1;
            }
        }

        /* Write file data */
        if (S_ISREG(st.st_mode) && st.st_size > 0) {
            int infd = open(argv[i], O_RDONLY, 0);
            if (infd < 0) {
                printf("ar: cannot open '%s'\n", argv[i]);
                close(fd);
                return 1;
            }
            char buf[8192];
            int n;
            while ((n = read(infd, buf, sizeof(buf))) > 0) {
                if (write(fd, buf, n) != n) {
                    close(infd);
                    close(fd);
                    printf("ar: write error\n");
                    return 1;
                }
            }
            close(infd);
        }
    }

    close(fd);
    return 0;
}

static int ar_read_entries(int fd, int extract, int list) {
    /* Read and verify magic */
    char magic[8];
    int n = read(fd, magic, 8);
    if (n < 8 || memcmp(magic, AR_MAGIC, 8) != 0) {
        printf("ar: bad magic\n");
        return 1;
    }

    for (;;) {
        struct ar_hdr hdr;
        memset(&hdr, 0, sizeof(hdr));
        n = read(fd, &hdr, sizeof(hdr));
        if (n <= 0)
            break;
        if (n < (int)sizeof(hdr)) {
            printf("ar: truncated header\n");
            return 1;
        }

        /* Verify magic trailer */
        if (hdr.ar_fmag[0] != AR_FMAGIC[0] || hdr.ar_fmag[1] != AR_FMAGIC[1]) {
            printf("ar: bad header magic\n");
            return 1;
        }

        /* Extract name */
        char name[256];
        int long_name = 0;
        unsigned long long name_len = 0;

        if (memcmp(hdr.ar_name, "#1/", 3) == 0) {
            /* BSD long name format */
            long_name = 1;
            name_len = 0;
            const char *np = hdr.ar_name + 3;
            for (int i = 0; i < 13 && np[i] >= '0' && np[i] <= '9'; i++) {
                name_len = name_len * 10 + (np[i] - '0');
            }
        } else {
            /* Regular name — find '/' or space to terminate */
            int ni = 0;
            while (ni < 16 && hdr.ar_name[ni] && hdr.ar_name[ni] != '/' && hdr.ar_name[ni] != ' ') {
                name[ni] = hdr.ar_name[ni];
                ni++;
            }
            name[ni] = 0;
        }

        /* Parse size */
        unsigned long long file_size = parse_decimal(hdr.ar_size, 10);

        /* Handle BSD long name: read name from data stream */
        if (long_name) {
            if (name_len > 0 && name_len < 256) {
                n = read(fd, name, name_len);
                if (n < (int)name_len) {
                    printf("ar: truncated name\n");
                    return 1;
                }
                name[name_len] = 0;
                file_size -= name_len;
            } else {
                char skipbuf[256];
                unsigned long long to_skip = name_len;
                while (to_skip > 0) {
                    unsigned long long chunk = to_skip > 256 ? 256 : to_skip;
                    int r = read(fd, skipbuf, chunk);
                    if (r <= 0) break;
                    to_skip -= r;
                }
                file_size -= name_len;
                name[0] = 0;
            }
        }

        if (list) {
            printf("%s\n", name);
        }

        if (extract && name[0]) {
            int outfd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (outfd < 0) {
                printf("ar: cannot create '%s'\n", name);
            }
            char buf[8192];
            unsigned long long remaining = file_size;
            while (remaining > 0) {
                unsigned long long chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
                int r = read(fd, buf, chunk);
                if (r <= 0) break;
                if (outfd >= 0)
                    write(outfd, buf, r);
                remaining -= r;
            }
            if (outfd >= 0)
                close(outfd);
        } else {
            char skipbuf[8192];
            unsigned long long remaining = file_size;
            while (remaining > 0) {
                unsigned long long chunk = remaining > sizeof(skipbuf) ? sizeof(skipbuf) : remaining;
                int r = read(fd, skipbuf, chunk);
                if (r <= 0) break;
                remaining -= r;
            }
        }

        /* Check for end marker: empty name */
        if (name[0] == 0 && !long_name)
            break;
    }

    return 0;
}
