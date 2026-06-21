/*
 * src/fs/cpio.c — Initramfs CPIO archive extraction.
 *
 * Parses a CPIO "newc" format archive (as produced by gen_init_cpio)
 * and extracts files into the SMTF filesystem.
 */

#define KERNEL_INTERNAL
#include "cpio.h"
#include "fs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"

/* Parse the typical S_ISDIR/DIR/LNK macros if not already defined */
#ifndef S_ISDIR
#define S_ISDIR(m)  (((m) & 0170000) == 0040000)
#endif
#ifndef S_ISREG
#define S_ISREG(m)  (((m) & 0170000) == 0100000)
#endif
#ifndef S_ISLNK
#define S_ISLNK(m)  (((m) & 0170000) == 0120000)
#endif

/* Convert 8 hex characters to uint32_t */
static uint32_t hex8(const char *s) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        v <<= 4;
        char c = s[i];
        if (c >= '0' && c <= '9')      v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
    }
    return v;
}

/* Align up to 4 bytes */
static uint32_t align4(uint32_t v) {
    return (v + 3) & ~3u;
}

int cpio_extract_initramfs(uint32_t addr, uint32_t size) {
    (void)size;
    uint8_t *buf = (uint8_t *)(uint64_t)addr;
    uint32_t offset = 0;
    int count = 0;

    kprintf("[cpio] Extracting initramfs at 0x%x...\n", addr);

    while (1) {
        struct cpio_newc_header *hdr = (struct cpio_newc_header *)(buf + offset);

        if (offset + 110 > size) break;

        /* Check magic */
        if (hdr->c_magic[0] != '0' || hdr->c_magic[1] != '7' ||
            hdr->c_magic[2] != '0' || hdr->c_magic[3] != '7' ||
            hdr->c_magic[4] != '0' || hdr->c_magic[5] != '1') {
            break;
        }

        uint32_t namesize = hex8(hdr->c_namesize);
        uint32_t filesize = hex8(hdr->c_filesize);
        uint32_t mode     = hex8(hdr->c_mode);

        /* Header + filename (aligned to 4) */
        uint32_t hdr_size = 110 + namesize;
        hdr_size = align4(hdr_size);

        const char *filename = (const char *)(buf + offset + 110);
        uint32_t data_offset = offset + hdr_size;

        /* Check for trailer */
        if (namesize >= 10 && strncmp(filename, CPIO_TRAILER, 10) == 0) {
            break;
        }

        /* Skip "." entries */
        if (filename[0] != '\0' && !(namesize == 2 && filename[0] == '.' && filename[1] == '\0')) {
            /* Ensure filename is null-terminated within bounds */
            char namebuf[128];
            int nlen = (int)(namesize - 1); /* exclude null terminator */
            if (nlen > 127) nlen = 127;
            memcpy(namebuf, filename, nlen);
            namebuf[nlen] = '\0';

            /* Create directories and files */
            if (S_ISDIR(mode)) {
                fs_create(namebuf, FS_TYPE_DIR);
            } else if (S_ISREG(mode) || (mode & 0xF000) == 0) {
                /* Regular file */
                if (filesize > 0) {
                    void *data = (void *)(buf + data_offset);
                    fs_write_file(namebuf, data, filesize);
                } else {
                    fs_create(namebuf, FS_TYPE_FILE);
                }
            } else if (S_ISLNK(mode)) {
                /* Symlink */
                if (filesize > 0) {
                    char *target = (char *)(buf + data_offset);
                    char tmp[256];
                    int tlen = (int)filesize;
                    if (tlen > 255) tlen = 255;
                    memcpy(tmp, target, tlen);
                    tmp[tlen] = '\0';
                    fs_symlink(namebuf, tmp);
                }
            }

            count++;
        }

        /* Advance past the data (aligned to 4) */
        uint32_t data_size = align4(filesize);
        offset = data_offset + data_size;

        if (offset >= size) break;
    }

    kprintf("[cpio] Extracted %d files from initramfs\n", count);
    return count;
}

int cpio_init(void) {
    kprintf("[cpio] CPIO/initramfs module initialized\n");
    return 0;
}

/* ── Stub: cpio_mount ─────────────────────────────── */
int cpio_mount(const char *source, const char *target, unsigned long flags)
{
    (void)source;
    (void)target;
    (void)flags;
    kprintf("[cpio] cpio_mount: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: cpio_umount ─────────────────────────────── */
int cpio_umount(const char *target)
{
    (void)target;
    kprintf("[cpio] cpio_umount: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: cpio_readdir ─────────────────────────────── */
int cpio_readdir(void *dir, void *filldir)
{
    (void)dir;
    (void)filldir;
    kprintf("[cpio] cpio_readdir: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: cpio_lookup ─────────────────────────────── */
int cpio_lookup(const char *name, void *parent)
{
    (void)name;
    (void)parent;
    kprintf("[cpio] cpio_lookup: not yet implemented\n");
    return -ENOSYS;
}
