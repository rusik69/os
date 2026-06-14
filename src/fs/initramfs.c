/* src/fs/initramfs.c — Extract embedded cpio archive into VFS at boot.
 *
 * Parses a CPIO "newc" format archive (as produced by gen_init_cpio)
 * and extracts files into the filesystem using vfs_create/vfs_write.
 *
 * The cpio archive is expected to be linked into the kernel as a binary
 * blob, accessible via the linker symbols:
 *   extern uint8_t _binary_build_initramfs_cpio_start[];
 *   extern uint8_t _binary_build_initramfs_cpio_end[];
 *
 * If no initramfs is linked, initramfs_extract() returns 0 silently.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "string.h"
#include "printf.h"
#include "fs.h"
#include "vfs.h"

/* ── CPIO newc format constants ─────────────────────────────────── */

/* CPIO "newc" magic: "070701" */
#define CPIO_MAGIC "\070\070\070\070\070\061"
#define CPIO_HEADER_SIZE 110
#define CPIO_TRAILER "TRAILER!!!"
#define CPIO_ALIGN  4

/* ── Hex parsing helpers ────────────────────────────────────────── */

/* Parse 8 hex characters into uint32_t */
static uint32_t hex8(const char *s) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        v <<= 4;
        char c = s[i];
        if      (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
    }
    return v;
}

/* Align v up to 4-byte boundary */
static uint32_t align4(uint32_t v) {
    return (v + 3) & ~3u;
}

/* ── S_IFMT / S_ISDIR / S_ISREG macros ──────────────────────────── */
#ifndef S_IFMT
#define S_IFMT   0170000
#endif
#ifndef S_IFDIR
#define S_IFDIR  0040000
#endif
#ifndef S_IFREG
#define S_IFREG  0100000
#endif
#ifndef S_ISDIR
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#endif

/* ── Embedded initramfs linker symbols (weak, may be absent) ── */
extern uint8_t _binary_build_initramfs_cpio_start[] __attribute__((weak));
extern uint8_t _binary_build_initramfs_cpio_end[] __attribute__((weak));

/* ── initramfs_extract — walk a CPIO archive and create files ──── */
int initramfs_extract(void) {
    uint8_t *start = _binary_build_initramfs_cpio_start;
    uint8_t *end   = _binary_build_initramfs_cpio_end;
    uint32_t size  = (uint32_t)(end - start);

    if (size == 0 || size < CPIO_HEADER_SIZE) {
        kprintf("[initramfs] No embedded cpio archive found\n");
        return 0;
    }

    kprintf("[initramfs] Extracting %u bytes of cpio data...\n", size);

    uint32_t offset = 0;
    int count = 0;

    while (offset + CPIO_HEADER_SIZE <= size) {
        const char *hdr = (const char *)(start + offset);

        /* Check magic */
        if (hdr[0] != '0' || hdr[1] != '7' || hdr[2] != '0' ||
            hdr[3] != '7' || hdr[4] != '0' || hdr[5] != '1') {
            kprintf("[initramfs] Bad magic at offset %u, stopping\n", offset);
            break;
        }

        uint32_t namesize = hex8(hdr + 94);  /* c_namesize at offset 94 */
        uint32_t filesize = hex8(hdr + 54);  /* c_filesize at offset 54 */
        uint32_t mode     = hex8(hdr + 38);  /* c_mode at offset 38 */

        /* Compute header+name field size (aligned to 4) */
        uint32_t hdr_name_size = CPIO_HEADER_SIZE + namesize;
        hdr_name_size = align4(hdr_name_size);
        uint32_t data_offset = offset + hdr_name_size;

        const char *filename = (const char *)(start + offset + CPIO_HEADER_SIZE);

        /* Check for trailer */
        if (namesize >= 10 && strncmp(filename, CPIO_TRAILER, 10) == 0) {
            break;
        }

        /* Skip "." entries */
        if (filename[0] != '\0' &&
            !(namesize == 2 && filename[0] == '.' && filename[1] == '\0')) {

            /* Null-terminate the filename for safe use */
            char namebuf[128];
            int nlen = (int)(namesize - 1); /* exclude null terminator */
            if (nlen > 127) nlen = 127;
            memcpy(namebuf, filename, nlen);
            namebuf[nlen] = '\0';

            if (S_ISDIR(mode)) {
                /* Create directory via VFS */
                struct vfs_stat st;
                if (vfs_stat(namebuf, &st) < 0) {
                    if (vfs_create(namebuf, VFS_TYPE_DIR) < 0) {
                        kprintf("[initramfs] Failed to create dir: %s\n", namebuf);
                    } else {
                        kprintf("[initramfs]   dir: %s\n", namebuf);
                    }
                }
            } else if (S_ISREG(mode) || (mode & 0xF000) == 0) {
                /* Regular file */
                struct vfs_stat st;
                if (vfs_stat(namebuf, &st) < 0) {
                    if (filesize > 0) {
                        const void *data = (const void *)(start + data_offset);
                        if (vfs_create(namebuf, VFS_TYPE_FILE) == 0) {
                            if (vfs_write(namebuf, data, filesize) < 0) {
                                kprintf("[initramfs] Failed to write: %s\n", namebuf);
                            } else {
                                kprintf("[initramfs]   file: %s (%u bytes)\n",
                                        namebuf, filesize);
                            }
                        } else {
                            kprintf("[initramfs] Failed to create: %s\n", namebuf);
                        }
                    } else {
                        /* Empty file */
                        if (vfs_create(namebuf, VFS_TYPE_FILE) < 0) {
                            kprintf("[initramfs] Failed to create empty: %s\n", namebuf);
                        }
                    }
                }
            }
            /* Skip symlinks for now — we can add S_ISLNK later if needed */

            count++;
        }

        /* Advance past the data (aligned to 4) */
        uint32_t data_size = align4(filesize);
        offset = data_offset + data_size;

        if (offset >= size) break;
    }

    kprintf("[initramfs] Extracted %d files\n", count);
    return count;
}
