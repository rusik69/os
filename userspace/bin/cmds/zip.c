/* zip.c — ZIP creation (PK format, stored method) */
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
    unsigned int   signature;
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
} __attribute__((packed));

/* Central directory header (46 bytes + filename + extra + comment) */
struct zip_central_hdr {
    unsigned int   signature;
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
    unsigned int   signature;
    unsigned short disk_num;
    unsigned short central_disk;
    unsigned short num_entries_disk;
    unsigned short num_entries_total;
    unsigned int   central_size;
    unsigned int   central_offset;
    unsigned short comment_len;
} __attribute__((packed));

static void write16(unsigned char *p, unsigned short val) {
    p[0] = val & 0xff;
    p[1] = (val >> 8) & 0xff;
}

static void write32(unsigned char *p, unsigned int val) {
    p[0] = val & 0xff;
    p[1] = (val >> 8) & 0xff;
    p[2] = (val >> 16) & 0xff;
    p[3] = (val >> 24) & 0xff;
}

static unsigned int crc32_calc(const unsigned char *data, unsigned long len) {
    unsigned int crc = 0xffffffff;
    static const unsigned int table[256] = {
        0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
        0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
        0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
        0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
        0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
        0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
        0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
        0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
        0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
        0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
        0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
        0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
        0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
        0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
        0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
        0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
        0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
        0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
        0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
        0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
        0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
        0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
        0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
        0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
        0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
        0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
        0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
        0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
        0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
        0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
        0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
        0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
        0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
        0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
        0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
        0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
        0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
        0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
        0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
        0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
        0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
        0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
        0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
    };
    for (unsigned long i = 0; i < len; i++) {
        crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xffffffff;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("usage: zip <zipfile> <files...>\n");
        return 1;
    }

    const char *zipname = argv[1];
    int fd = open(zipname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("zip: cannot create '%s'\n", zipname);
        return 1;
    }

    /* Store offsets for central directory entries */
    unsigned int num_entries = argc - 2;
    unsigned int *local_offsets = malloc(num_entries * sizeof(unsigned int));
    unsigned int *comp_sizes = malloc(num_entries * sizeof(unsigned int));
    unsigned int *uncomp_sizes = malloc(num_entries * sizeof(unsigned int));
    unsigned int *crcs = malloc(num_entries * sizeof(unsigned int));
    unsigned short *name_lens = malloc(num_entries * sizeof(unsigned short));
    char **names = malloc(num_entries * sizeof(char *));

    if (!local_offsets || !comp_sizes || !uncomp_sizes || !crcs || !name_lens || !names) {
        printf("zip: out of memory\n");
        close(fd);
        return 1;
    }

    /* Write local file headers and data */
    for (unsigned int i = 0; i < num_entries; i++) {
        const char *filepath = argv[i + 2];
        struct stat st;
        if (stat(filepath, &st) < 0) {
            printf("zip: cannot stat '%s'\n", filepath);
            close(fd);
            return 1;
        }

        /* Use basename */
        const char *bname = filepath;
        const char *slash = strrchr(filepath, '/');
        if (slash) bname = slash + 1;
        unsigned short nlen = strlen(bname);

        names[i] = malloc(nlen + 1);
        if (!names[i]) {
            printf("zip: out of memory\n");
            close(fd);
            return 1;
        }
        memcpy(names[i], bname, nlen + 1);
        name_lens[i] = nlen;

        local_offsets[i] = lseek(fd, 0, SEEK_CUR);

        /* Read file data and compute CRC */
        unsigned char *filedata = 0;
        unsigned int fsize = 0;
        if (S_ISREG(st.st_mode) && st.st_size > 0) {
            int infd = open(filepath, O_RDONLY, 0);
            if (infd < 0) {
                printf("zip: cannot open '%s'\n", filepath);
                close(fd);
                return 1;
            }
            fsize = st.st_size;
            filedata = malloc(fsize);
            if (!filedata) {
                close(infd);
                close(fd);
                printf("zip: out of memory\n");
                return 1;
            }
            int n = read(infd, filedata, fsize);
            close(infd);
            if (n < (int)fsize) {
                free(filedata);
                close(fd);
                printf("zip: read error\n");
                return 1;
            }
        }

        unsigned int crc = 0;
        if (filedata) {
            crc = crc32_calc(filedata, fsize);
        }
        crcs[i] = crc;
        comp_sizes[i] = fsize;
        uncomp_sizes[i] = fsize;

        /* Write local file header */
        unsigned char local_hdr[30];
        write32(&local_hdr[0], 0x04034b50);
        write16(&local_hdr[4], 20);    /* version needed 2.0 */
        write16(&local_hdr[6], 0);     /* flags */
        write16(&local_hdr[8], 0);     /* method: stored */
        write16(&local_hdr[10], 0);    /* mod_time */
        write16(&local_hdr[12], 0);    /* mod_date */
        write32(&local_hdr[14], crc);
        write32(&local_hdr[18], fsize);
        write32(&local_hdr[22], fsize);
        write16(&local_hdr[26], nlen);
        write16(&local_hdr[28], 0);    /* extra_len */

        if (write(fd, local_hdr, 30) != 30) {
            printf("zip: write error\n");
            close(fd);
            return 1;
        }

        /* Write filename */
        if (write(fd, bname, nlen) != nlen) {
            printf("zip: write error\n");
            close(fd);
            return 1;
        }

        /* Write file data */
        if (filedata && fsize > 0) {
            if (write(fd, filedata, fsize) != (int)fsize) {
                printf("zip: write error\n");
                free(filedata);
                close(fd);
                return 1;
            }
            free(filedata);
        }
    }

    /* Write central directory */
    unsigned int central_offset = lseek(fd, 0, SEEK_CUR);

    for (unsigned int i = 0; i < num_entries; i++) {
        unsigned char cd_hdr[46];
        write32(&cd_hdr[0], 0x02014b50);
        write16(&cd_hdr[4], 20);     /* version made by */
        write16(&cd_hdr[6], 20);     /* version needed */
        write16(&cd_hdr[8], 0);      /* flags */
        write16(&cd_hdr[10], 0);     /* method: stored */
        write16(&cd_hdr[12], 0);     /* mod_time */
        write16(&cd_hdr[14], 0);     /* mod_date */
        write32(&cd_hdr[16], crcs[i]);
        write32(&cd_hdr[20], comp_sizes[i]);
        write32(&cd_hdr[24], uncomp_sizes[i]);
        write16(&cd_hdr[28], name_lens[i]);
        write16(&cd_hdr[30], 0);     /* extra_len */
        write16(&cd_hdr[32], 0);     /* comment_len */
        write16(&cd_hdr[34], 0);     /* disk_start */
        write16(&cd_hdr[36], 0);     /* internal_attr */
        write32(&cd_hdr[38], 0);     /* external_attr */
        write32(&cd_hdr[42], local_offsets[i]);

        if (write(fd, cd_hdr, 46) != 46) {
            printf("zip: write error\n");
            close(fd);
            return 1;
        }

        /* Write filename */
        if (write(fd, names[i], name_lens[i]) != name_lens[i]) {
            printf("zip: write error\n");
            close(fd);
            return 1;
        }

        free(names[i]);
    }

    unsigned int central_size = lseek(fd, 0, SEEK_CUR) - central_offset;

    /* Write end of central directory */
    unsigned char eocd[22];
    write32(&eocd[0], 0x06054b50);
    write16(&eocd[4], 0);            /* disk_num */
    write16(&eocd[6], 0);            /* central_disk */
    write16(&eocd[8], num_entries);  /* num_entries_disk */
    write16(&eocd[10], num_entries); /* num_entries_total */
    write32(&eocd[12], central_size);
    write32(&eocd[16], central_offset);
    write16(&eocd[20], 0);           /* comment_len */

    if (write(fd, eocd, 22) != 22) {
        printf("zip: write error\n");
        close(fd);
        return 1;
    }

    free(local_offsets);
    free(comp_sizes);
    free(uncomp_sizes);
    free(crcs);
    free(name_lens);
    free(names);

    close(fd);
    printf("zip: created '%s' (%u entries, stored)\n", zipname, num_entries);
    return 0;
}
