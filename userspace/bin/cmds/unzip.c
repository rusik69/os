/* unzip.c — ZIP extraction (PK format) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/stat.h"

/* ZIP signatures */
#define PK_LOCAL_FILE_HDR  0x04034b50
#define PK_CENTRAL_DIR_HDR 0x02014b50
#define PK_END_CENTRAL_DIR 0x06054b50

/* Local file header (30 bytes + filename + extra) */
struct zip_local_hdr {
    unsigned int   signature;        /* 0x04034b50 */
    unsigned short version_needed;
    unsigned short flags;
    unsigned short method;           /* 0=stored, 8=deflated */
    unsigned short mod_time;
    unsigned short mod_date;
    unsigned int   crc32;
    unsigned int   comp_size;
    unsigned int   uncomp_size;
    unsigned short filename_len;
    unsigned short extra_len;
} __attribute__((packed));

/* Central directory header (46 bytes + filename + extra + comment) */
struct zip_central_hdr {
    unsigned int   signature;        /* 0x02014b50 */
    unsigned short version_made;
    unsigned short version_needed;
    unsigned short flags;
    unsigned short method;
    unsigned short mod_time;
    unsigned short mod_date;
    unsigned int   crc32;
    unsigned int   comp_size;
    unsigned int   uncomp_size;
    unsigned short filename_len;
    unsigned short extra_len;
    unsigned short comment_len;
    unsigned short disk_start;
    unsigned short internal_attr;
    unsigned int   external_attr;
    unsigned int   local_offset;
} __attribute__((packed));

/* End of central directory (22 bytes + comment) */
struct zip_end_central {
    unsigned int   signature;        /* 0x06054b50 */
    unsigned short disk_num;
    unsigned short central_disk;
    unsigned short num_entries_disk;
    unsigned short num_entries_total;
    unsigned int   central_size;
    unsigned int   central_offset;
    unsigned short comment_len;
} __attribute__((packed));

static int unzip_list(const char *zipfile);
static int unzip_extract(const char *zipfile);

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: unzip -l <zip>   (list contents)\n");
        printf("       unzip <zip>      (extract)\n");
        return 1;
    }

    if (strcmp(argv[1], "-l") == 0) {
        if (argc < 3) {
            printf("unzip: -l requires a zip file\n");
            return 1;
        }
        return unzip_list(argv[2]);
    }

    return unzip_extract(argv[1]);
}

static int read_uint16(const unsigned char *p) {
    return p[0] | (p[1] << 8);
}

static int read_uint32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

/* Read and parse end of central directory to find central dir offset */
static int find_central_dir(int fd, struct zip_end_central *eocd) {
    /* Read last 65557 bytes of file to find EOCD signature */
    unsigned long long file_size = 0;
    {
        char buf[256];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            file_size += n;
    }

    unsigned long long search_start = 0;
    if (file_size > 65557)
        search_start = file_size - 65557;

    /* Read data starting from search_start */
    lseek(fd, search_start, SEEK_SET);
    char data[65557];
    int n = read(fd, data, sizeof(data));
    if (n <= 0) return -1;

    /* Search backwards for EOCD signature */
    for (int i = n - 22; i >= 0; i--) {
        unsigned char *p = (unsigned char *)&data[i];
        if (p[0] == 0x50 && p[1] == 0x4b && p[2] == 0x05 && p[3] == 0x06) {
            eocd->signature = 0x06054b50;
            eocd->disk_num = read_uint16(&p[4]);
            eocd->central_disk = read_uint16(&p[6]);
            eocd->num_entries_disk = read_uint16(&p[8]);
            eocd->num_entries_total = read_uint16(&p[10]);
            eocd->central_size = read_uint32(&p[12]);
            eocd->central_offset = read_uint32(&p[16]);
            eocd->comment_len = read_uint16(&p[20]);
            return 0;
        }
    }

    return -1;
}

static int unzip_list(const char *zipfile) {
    int fd = open(zipfile, O_RDONLY, 0);
    if (fd < 0) {
        printf("unzip: cannot open '%s'\n", zipfile);
        return 1;
    }

    struct zip_end_central eocd;
    if (find_central_dir(fd, &eocd) < 0) {
        printf("unzip: not a valid zip file (no EOCD)\n");
        close(fd);
        return 1;
    }

    /* Read through central directory entries */
    lseek(fd, eocd.central_offset, SEEK_SET);

    unsigned char hdr_buf[46];
    for (int i = 0; i < eocd.num_entries_total; i++) {
        int n = read(fd, hdr_buf, 46);
        if (n < 46) break;

        unsigned int sig = read_uint32(&hdr_buf[0]);
        if (sig != 0x02014b50) break;

        unsigned short method = read_uint16(&hdr_buf[10]);
        unsigned int comp_size = read_uint32(&hdr_buf[20]);
        unsigned int uncomp_size = read_uint32(&hdr_buf[24]);
        unsigned short filename_len = read_uint16(&hdr_buf[28]);
        unsigned short extra_len = read_uint16(&hdr_buf[30]);
        unsigned short comment_len = read_uint16(&hdr_buf[32]);

        /* Read filename */
        char namebuf[256];
        if (filename_len >= sizeof(namebuf)) filename_len = sizeof(namebuf) - 1;
        int r = read(fd, namebuf, filename_len);
        namebuf[filename_len] = 0;

        /* Skip extra field and comment */
        unsigned long skip = extra_len + comment_len;
        char skipbuf[256];
        while (skip > 0) {
            unsigned long chunk = skip > sizeof(skipbuf) ? sizeof(skipbuf) : skip;
            r = read(fd, skipbuf, chunk);
            if (r <= 0) break;
            skip -= r;
        }

        /* Print info */
        printf("  %s", namebuf);
        if (method == 0)
            printf(" (stored, %u bytes)", uncomp_size);
        else if (method == 8)
            printf(" (deflated, %u -> %u)", uncomp_size, comp_size);
        else
            printf(" (method %u)", method);
        printf("\n");
    }

    close(fd);
    return 0;
}

static int unzip_extract(const char *zipfile) {
    int fd = open(zipfile, O_RDONLY, 0);
    if (fd < 0) {
        printf("unzip: cannot open '%s'\n", zipfile);
        return 1;
    }

    /* Find central directory */
    struct zip_end_central eocd;
    if (find_central_dir(fd, &eocd) < 0) {
        printf("unzip: not a valid zip file (no EOCD)\n");
        close(fd);
        return 1;
    }

    /* Read central directory to get local header offsets */
    unsigned char *central_raw = malloc(eocd.central_size);
    if (!central_raw) {
        printf("unzip: out of memory\n");
        close(fd);
        return 1;
    }
    lseek(fd, eocd.central_offset, SEEK_SET);
    int n = read(fd, central_raw, eocd.central_size);
    if (n < (int)eocd.central_size) {
        free(central_raw);
        printf("unzip: truncated central directory\n");
        close(fd);
        return 1;
    }

    /* Parse each central directory entry */
    unsigned long offset = 0;
    for (int i = 0; i < eocd.num_entries_total; i++) {
        if (offset + 46 > eocd.central_size) break;
        unsigned char *entry = central_raw + offset;

        unsigned int sig = read_uint32(&entry[0]);
        if (sig != 0x02014b50) break;

        unsigned short method = read_uint16(&entry[10]);
        unsigned int comp_size = read_uint32(&entry[20]);
        unsigned short filename_len = read_uint16(&entry[28]);
        unsigned short extra_len = read_uint16(&entry[30]);
        unsigned short comment_len = read_uint16(&entry[32]);
        unsigned int local_offset = read_uint32(&entry[42]);

        /* Get filename */
        char namebuf[256];
        if (filename_len >= sizeof(namebuf)) filename_len = sizeof(namebuf) - 1;
        memcpy(namebuf, entry + 46, filename_len);
        namebuf[filename_len] = 0;

        offset += 46 + filename_len + extra_len + comment_len;

        /* Check for directory entry */
        unsigned long nmlen = strlen(namebuf);
        if (nmlen > 0 && namebuf[nmlen - 1] == '/') {
            /* Directory — strip trailing slash and create */
            namebuf[nmlen - 1] = 0;
            mkdir(namebuf, 0755);
            continue;
        }

        /* Seek to local file header */
        lseek(fd, local_offset, SEEK_SET);
        struct zip_local_hdr local;
        n = read(fd, &local, sizeof(local));
        if (n < (int)sizeof(local)) continue;

        /* Skip filename and extra in local header */
        unsigned long local_skip = local.filename_len + local.extra_len;
        char skipbuf[256];
        while (local_skip > 0) {
            unsigned long chunk = local_skip > sizeof(skipbuf) ? sizeof(skipbuf) : local_skip;
            int r = read(fd, skipbuf, chunk);
            if (r <= 0) break;
            local_skip -= r;
        }

        if (method == 0) {
            /* Stored: copy directly */
            int outfd = open(namebuf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (outfd < 0) {
                printf("unzip: cannot create '%s'\n", namebuf);
            }
            char databuf[8192];
            unsigned int remaining = comp_size;
            while (remaining > 0) {
                unsigned int chunk = remaining > sizeof(databuf) ? sizeof(databuf) : remaining;
                int r = read(fd, databuf, chunk);
                if (r <= 0) break;
                if (outfd >= 0)
                    write(outfd, databuf, r);
                remaining -= r;
            }
            if (outfd >= 0)
                close(outfd);
            printf(" extracting: %s\n", namebuf);
        } else if (method == 8) {
            /* Deflated — skip since we don't have inflate */
            printf(" extracting: %s (deflated, skipping)\n", namebuf);
            unsigned int remaining = comp_size;
            while (remaining > 0) {
                unsigned int chunk = remaining > sizeof(skipbuf) ? sizeof(skipbuf) : remaining;
                int r = read(fd, skipbuf, chunk);
                if (r <= 0) break;
                remaining -= r;
            }
        } else {
            printf(" extracting: %s (method %u, skipping)\n", namebuf, method);
            unsigned int remaining = comp_size;
            while (remaining > 0) {
                unsigned int chunk = remaining > sizeof(skipbuf) ? sizeof(skipbuf) : remaining;
                int r = read(fd, skipbuf, chunk);
                if (r <= 0) break;
                remaining -= r;
            }
        }
    }

    free(central_raw);
    close(fd);
    return 0;
}
