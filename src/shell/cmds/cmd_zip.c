/* cmd_zip.c — ZIP archiver (create basic ZIP files) */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

/* ZIP local file header signature */
#define ZIP_LOCAL_HDR_SIG  0x04034b50
#define ZIP_CENTRAL_HDR_SIG 0x02014b50
#define ZIP_END_CENTRAL_SIG 0x06054b50

struct zip_local_hdr {
    uint32_t sig;
    uint16_t version;
    uint16_t flags;
    uint16_t compression;
    uint16_t mod_time;
    uint16_t mod_date;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_len;
    uint16_t extra_len;
} __attribute__((packed));

void cmd_zip(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: zip <archive.zip> <files...>\n");
        return;
    }

    char argbuf[128];
    strncpy(argbuf, args, 127); argbuf[127] = '\0';

    char *archive_name = strtok(argbuf, " ");
    char *filenames[16];
    int num_files = 0;

    char *f = strtok((char *)0, " ");
    while (f && num_files < 16) {
        filenames[num_files++] = f;
        f = strtok((char *)0, " ");
    }

    if (!archive_name || num_files == 0) {
        kprintf("Usage: zip <archive.zip> <files...>\n");
        return;
    }

    char archive_path[64];
    if (archive_name[0] != '/') { archive_path[0] = '/'; strncpy(archive_path + 1, archive_name, 62); archive_path[63] = '\0'; }
    else { strncpy(archive_path, archive_name, 63); archive_path[63] = '\0'; }

    /* Add .zip extension if missing */
    int alen = strlen(archive_path);
    if (alen < 4 || archive_path[alen-4] != '.' || archive_path[alen-3] != 'z' ||
        archive_path[alen-2] != 'i' || archive_path[alen-1] != 'p') {
        if (alen < 60) { archive_path[alen] = '.'; archive_path[alen+1] = 'z'; archive_path[alen+2] = 'i'; archive_path[alen+3] = 'p'; archive_path[alen+4] = '\0'; }
    }

    static unsigned char zipbuf[16384];
    uint32_t zip_pos = 0;
    static unsigned char filebuf[4096];

    /* Store central directory offsets for later */
    uint32_t cd_offsets[16];
    uint16_t cd_fname_lens[16];

    /* Write local file headers and data */
    for (int i = 0; i < num_files; i++) {
        char filepath[64];
        if (filenames[i][0] != '/') { filepath[0] = '/'; strncpy(filepath + 1, filenames[i], 62); filepath[63] = '\0'; }
        else { strncpy(filepath, filenames[i], 63); filepath[63] = '\0'; }

        uint32_t file_size = 0;
        if (libc_vfs_read(filepath, filebuf, sizeof(filebuf), &file_size) != 0) {
            kprintf("zip: %s: not found\n", filenames[i]);
            continue;
        }

        uint16_t fname_len = (uint16_t)strlen(filenames[i]);
        cd_offsets[i] = zip_pos;
        cd_fname_lens[i] = fname_len;

        /* Local file header */
        struct zip_local_hdr hdr;
        hdr.sig = ZIP_LOCAL_HDR_SIG;
        hdr.version = 20;
        hdr.flags = 0;
        hdr.compression = 0; /* stored */
        hdr.mod_time = 0;
        hdr.mod_date = 0;
        hdr.crc32 = 0;
        hdr.compressed_size = file_size;
        hdr.uncompressed_size = file_size;
        hdr.filename_len = fname_len;
        hdr.extra_len = 0;

        if (zip_pos + sizeof(hdr) + fname_len + file_size > sizeof(zipbuf)) {
            kprintf("zip: archive too large\n");
            return;
        }

        /* Write header */
        // Use memcpy for packed struct
        unsigned char *p = zipbuf + zip_pos;
        *((uint32_t*)(p)) = hdr.sig; p += 4;
        *((uint16_t*)(p)) = hdr.version; p += 2;
        *((uint16_t*)(p)) = hdr.flags; p += 2;
        *((uint16_t*)(p)) = hdr.compression; p += 2;
        *((uint16_t*)(p)) = hdr.mod_time; p += 2;
        *((uint16_t*)(p)) = hdr.mod_date; p += 2;
        *((uint32_t*)(p)) = hdr.crc32; p += 4;
        *((uint32_t*)(p)) = hdr.compressed_size; p += 4;
        *((uint32_t*)(p)) = hdr.uncompressed_size; p += 4;
        *((uint16_t*)(p)) = hdr.filename_len; p += 2;
        *((uint16_t*)(p)) = hdr.extra_len; p += 2;
        zip_pos = (uint32_t)(p - zipbuf);

        /* Write filename */
        memcpy(zipbuf + zip_pos, filenames[i], fname_len);
        zip_pos += fname_len;

        /* Write file data */
        memcpy(zipbuf + zip_pos, filebuf, file_size);
        zip_pos += file_size;

        kprintf("  adding: %s (%u bytes)\n", filenames[i], (uint64_t)file_size);
    }

    /* Write central directory */
    uint32_t cd_start = zip_pos;
    for (int i = 0; i < num_files; i++) {
        if (cd_fname_lens[i] == 0) continue; /* skipped */

        uint16_t fname_len = cd_fname_lens[i];
        struct zip_local_hdr chdr;
        chdr.sig = ZIP_CENTRAL_HDR_SIG;
        chdr.version = 20;
        chdr.flags = 0;
        chdr.compression = 0;
        chdr.mod_time = 0;
        chdr.mod_date = 0;
        chdr.crc32 = 0;
        chdr.compressed_size = 0;
        chdr.uncompressed_size = 0;
        chdr.filename_len = fname_len;
        chdr.extra_len = 0;

        if (zip_pos + 46 + fname_len > sizeof(zipbuf)) break;

        unsigned char *p = zipbuf + zip_pos;
        *((uint32_t*)(p)) = chdr.sig; p += 4;
        *((uint16_t*)(p)) = chdr.version; p += 2;
        /* version needed */
        *((uint16_t*)(p)) = 20; p += 2;
        *((uint16_t*)(p)) = chdr.flags; p += 2;
        *((uint16_t*)(p)) = chdr.compression; p += 2;
        *((uint16_t*)(p)) = chdr.mod_time; p += 2;
        *((uint16_t*)(p)) = chdr.mod_date; p += 2;
        *((uint32_t*)(p)) = chdr.crc32; p += 4;
        *((uint32_t*)(p)) = chdr.compressed_size; p += 4;
        *((uint32_t*)(p)) = chdr.uncompressed_size; p += 4;
        *((uint16_t*)(p)) = fname_len; p += 2;
        *((uint16_t*)(p)) = chdr.extra_len; p += 2;
        /* file comment length */
        *((uint16_t*)(p)) = 0; p += 2;
        /* disk number start */
        *((uint16_t*)(p)) = 0; p += 2;
        /* internal file attributes */
        *((uint16_t*)(p)) = 0; p += 2;
        /* external file attributes */
        *((uint32_t*)(p)) = 0; p += 4;
        /* relative offset of local header */
        *((uint32_t*)(p)) = cd_offsets[i]; p += 4;
        zip_pos = (uint32_t)(p - zipbuf);

        memcpy(zipbuf + zip_pos, filenames[i], fname_len);
        zip_pos += fname_len;
    }

    /* Write end of central directory */
    uint32_t cd_end = zip_pos;
    uint32_t cd_size = cd_end - cd_start;
    uint16_t num_entries = 0;
    for (int i = 0; i < num_files; i++) {
        if (cd_fname_lens[i] > 0) num_entries++;
    }

    if (zip_pos + 22 <= sizeof(zipbuf)) {
        unsigned char *p = zipbuf + zip_pos;
        *((uint32_t*)(p)) = ZIP_END_CENTRAL_SIG; p += 4;
        *((uint16_t*)(p)) = 0; p += 2; /* disk number */
        *((uint16_t*)(p)) = 0; p += 2; /* disk with central dir */
        *((uint16_t*)(p)) = num_entries; p += 2; /* entries on this disk */
        *((uint16_t*)(p)) = num_entries; p += 2; /* total entries */
        *((uint32_t*)(p)) = cd_size; p += 4;
        *((uint32_t*)(p)) = cd_start; p += 4;
        *((uint16_t*)(p)) = 0; p += 2; /* comment length */
        zip_pos = (uint32_t)(p - zipbuf);
    }

    libc_vfs_write(archive_path, zipbuf, zip_pos);
    kprintf("Archive: %s (%u bytes, %u files)\n", archive_path, (uint64_t)zip_pos, (uint64_t)num_entries);
}
