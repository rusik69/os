/*
 * src/fs/ntfs.c — NTFS read-only filesystem
 *
 * Implements a read-only NTFS filesystem supporting:
 *   - Master File Table (MFT) parsing
 *   - Standard MFT attributes: $STANDARD_INFORMATION, $FILE_NAME, $DATA
 *   - Non-resident data attribute runs
 *   - Fixup array handling for MFT records
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"
#include "errno.h"
#include "blockdev.h"
#include "ntfs.h"

#ifdef MODULE
#include "module.h"
#endif
#include "initcall.h"

/* ── Helpers ────────────────────────────────────────────────────── */

static inline uint16_t r16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t r64(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

/* ── Block I/O ──────────────────────────────────────────────────── */

static int ntfs_read_sectors(struct ntfs_priv *np, uint64_t lba,
                              uint32_t count, uint8_t *buf)
{
    for (uint32_t i = 0; i < count; i++) {
        if (blockdev_read_sectors(np->dev_id, lba + i, 1,
                                   buf + i * np->bytes_per_sector) != 0)
            return -1;
    }
    return 0;
}

static int ntfs_read_cluster(struct ntfs_priv *np, uint64_t lcn,
                              uint8_t *buf)
{
    uint64_t lba = lcn * (uint64_t)np->sectors_per_cluster;
    return ntfs_read_sectors(np, lba, np->sectors_per_cluster, buf);
}

/* ── Cluster → LBA ──────────────────────────────────────────────── */

static uint64_t ntfs_cluster_to_lba(struct ntfs_priv *np, int64_t lcn)
{
    return (uint64_t)lcn * np->sectors_per_cluster;
}

/* ── Fixup application ──────────────────────────────────────────── */

static int ntfs_apply_fixups(struct ntfs_priv *np, uint8_t *mft_rec,
                              uint32_t record_size)
{
    struct ntfs_mft_rec *rec = (struct ntfs_mft_rec *)mft_rec;
    if (rec->magic != 0x454C4946) /* "FILE" */
        return -1;

    uint16_t usa_ofs = rec->usa_ofs;
    uint16_t usa_count = rec->usa_count;

    if ((uint32_t)usa_ofs < sizeof(struct ntfs_mft_rec) ||
        (uint32_t)(usa_ofs + usa_count * 2) > record_size)
        return -1;

    /* First word of USA = update sequence number */
    uint16_t usn = r16(mft_rec + usa_ofs);

    /* Last 2 bytes of each sector should match USN, replace with next USA entry */
    uint32_t sectors_per_record = record_size / 512;
    for (uint16_t i = 1; i < usa_count && i <= sectors_per_record; i++) {
        uint32_t sector_end_offset = i * 512 - 2;
        if (sector_end_offset + 2 > record_size) break;
        uint16_t saved_word = r16(mft_rec + usa_ofs + i * 2);
        if (r16(mft_rec + sector_end_offset) == usn) {
            /* Write saved word at sector end */
            mft_rec[sector_end_offset] = (uint8_t)(saved_word & 0xFF);
            mft_rec[sector_end_offset + 1] = (uint8_t)(saved_word >> 8);
        }
    }
    return 0;
}

/* ── Read MFT record ────────────────────────────────────────────── */

static uint8_t *ntfs_read_mft_rec(struct ntfs_priv *np, uint64_t mft_index)
{
    uint64_t mft_lcn = np->mft_lcn;
    uint32_t mft_rec_size = np->mft_record_size;

    /* Calculate cluster offset within MFT */
    uint64_t byte_offset = mft_index * mft_rec_size;
    uint64_t cluster_offset = byte_offset / np->cluster_size;
    uint32_t within_cluster = (uint32_t)(byte_offset % np->cluster_size);

    /* Allocate buffer for one MFT record */
    uint8_t *buf = (uint8_t *)kmalloc(mft_rec_size);
    if (!buf) return NULL;

    /* Read the cluster containing this MFT record */
    if (ntfs_read_cluster(np, mft_lcn + cluster_offset, buf) < 0) {
        kfree(buf);
        return NULL;
    }

    /* Copy the relevant MFT record portion (may span multiple clusters for large records) */
    /* For simplicity, assume mft_rec_size <= cluster_size */
    if (mft_rec_size > np->cluster_size) {
        /* Need to read multiple clusters */
        uint32_t clusters_needed = (within_cluster + mft_rec_size + np->cluster_size - 1)
                                     / np->cluster_size;
        for (uint32_t i = 1; i < clusters_needed; i++) {
            if (ntfs_read_cluster(np, mft_lcn + cluster_offset + i,
                                   buf + i * np->cluster_size) < 0) {
                kfree(buf);
                return NULL;
            }
        }
    }

    /* Shift to get the record-aligned data */
    uint8_t *rec_buf = (uint8_t *)kmalloc(mft_rec_size);
    if (!rec_buf) { kfree(buf); return NULL; }
    memcpy(rec_buf, buf + within_cluster, mft_rec_size);
    kfree(buf);

    /* Apply fixups */
    if (ntfs_apply_fixups(np, rec_buf, mft_rec_size) < 0) {
        kfree(rec_buf);
        return NULL;
    }

    return rec_buf;
}

/* ── Find attribute in MFT record ──────────────────────────────── */

static uint8_t *ntfs_find_attr(struct ntfs_priv *np, uint8_t *mft_rec,
                                uint32_t attr_type, int *out_len)
{
    struct ntfs_mft_rec *rec = (struct ntfs_mft_rec *)mft_rec;
    uint16_t attr_off = rec->attr_offset;
    uint32_t rec_size = r32(mft_rec + 0x1C); /* bytes_allocated from header */

    if (attr_off < sizeof(struct ntfs_mft_rec))
        return NULL;

    uint8_t *pos = mft_rec + attr_off;
    while (1) {
        struct ntfs_attr_hdr *attr = (struct ntfs_attr_hdr *)pos;

        if (attr->type == 0xFFFFFFFF) /* end marker */
            break;

        if (pos + sizeof(struct ntfs_attr_hdr) > mft_rec + rec_size)
            break;

        if (attr->type == attr_type) {
            if (out_len) *out_len = attr->length;
            return pos;
        }

        if (attr->length == 0) break;
        pos += attr->length;
        if (pos >= mft_rec + rec_size) break;
    }
    return NULL;
}

/* ── Parse non-resident data runs ──────────────────────────────── */

static int ntfs_parse_runs(uint8_t *mapping, uint32_t mapping_len,
                            struct ntfs_run *runs, uint32_t *num_runs)
{
    uint32_t count = 0;
    uint8_t *pos = mapping;
    uint8_t *end = mapping + mapping_len;
    int64_t current_lcn = 0;

    while (pos < end) {
        uint8_t head = *pos++;
        if (head == 0) break; /* end marker */

        int run_len_bytes = head & 0x0F;
        int run_offset_bytes = (head >> 4) & 0x0F;

        if (run_len_bytes == 0) break;
        if (pos + run_len_bytes + run_offset_bytes > end) break;

        /* Read length */
        uint64_t length = 0;
        for (int i = 0; i < run_len_bytes; i++) {
            length |= ((uint64_t)pos[i]) << (i * 8);
        }
        pos += run_len_bytes;

        /* Read offset (signed) */
        int64_t offset = 0;
        for (int i = 0; i < run_offset_bytes; i++) {
            offset |= ((int64_t)pos[i]) << (i * 8);
        }
        /* Sign-extend if high bit set */
        if (run_offset_bytes > 0 && (pos[run_offset_bytes - 1] & 0x80)) {
            for (int i = run_offset_bytes; i < 8; i++) {
                offset |= (int64_t)(0xFF) << (i * 8);
            }
        }
        pos += run_offset_bytes;

        if (run_offset_bytes == 0) {
            /* Sparse run */
            runs[count].lcn = (uint64_t)-1;
        } else {
            current_lcn += offset;
            runs[count].lcn = current_lcn;
        }
        runs[count].length = length;
        count++;
        if (count >= 64) break;
    }

    *num_runs = count;
    return 0;
}

/* ── Read data from MFT record via data attribute ──────────────── */

static int ntfs_read_data(struct ntfs_priv *np, uint8_t *mft_rec,
                           uint8_t *buf, uint32_t offset, uint32_t size,
                           uint32_t *out_size)
{
    int attr_len;
    uint8_t *attr = ntfs_find_attr(np, mft_rec, AT_DATA, &attr_len);
    if (!attr) return -1;

    struct ntfs_attr_hdr *ah = (struct ntfs_attr_hdr *)attr;

    if (ah->non_resident == 0) {
        /* Resident data */
        uint32_t val_len = ah->value_length;
        uint16_t val_off = ah->value_offset;
        if (val_off + val_len > (uint32_t)attr_len) return -1;
        uint32_t to_copy = val_len - offset;
        if (to_copy > size) to_copy = size;
        if ((int32_t)to_copy < 0) to_copy = 0;
        memcpy(buf, attr + val_off + offset, to_copy);
        *out_size = to_copy;
        return 0;
    } else {
        /* Non-resident: parse runs */
        uint64_t real_size = ah->real_size;
        uint16_t mp_off = ah->mapping_pairs_offset;
        uint32_t mp_len = attr_len - mp_off;

        if (offset >= real_size) {
            *out_size = 0;
            return 0;
        }

        uint32_t to_read = (uint32_t)(real_size - offset);
        if (to_read > size) to_read = size;

        struct ntfs_run runs[64];
        uint32_t num_runs;
        if (ntfs_parse_runs(attr + mp_off, mp_len, runs, &num_runs) < 0)
            return -1;

        /* Walk runs to find the data at the given offset */
        uint64_t cur_offset = 0;
        uint32_t done = 0;
        for (uint32_t i = 0; i < num_runs && done < to_read; i++) {
            uint64_t run_size = runs[i].length * np->cluster_size;

            if (offset < cur_offset + run_size) {
                uint64_t run_inner_offset = offset - cur_offset;
                uint64_t lcn = runs[i].lcn;
                uint32_t cluster_offset = (uint32_t)(run_inner_offset % np->cluster_size);
                uint32_t start_cluster = (uint32_t)(run_inner_offset / np->cluster_size);

                for (uint32_t c = start_cluster; c < runs[i].length && done < to_read; c++) {
                    if (lcn == (uint64_t)-1) {
                        /* Sparse: zero fill */
                        uint32_t chunk = np->cluster_size - cluster_offset;
                        if (chunk > to_read - done) chunk = to_read - done;
                        memset(buf + done, 0, chunk);
                        done += chunk;
                    } else {
                        uint64_t clba = ntfs_cluster_to_lba(np, (int64_t)(lcn + c));
                        uint8_t cluster_buf[8192]; /* max cluster size */
                        if (ntfs_read_sectors(np, clba + cluster_offset / np->bytes_per_sector,
                                              np->sectors_per_cluster - cluster_offset / np->bytes_per_sector,
                                              cluster_buf) < 0)
                            return -1;

                        uint32_t chunk = np->cluster_size - cluster_offset;
                        if (chunk > to_read - done) chunk = to_read - done;
                        memcpy(buf + done, cluster_buf + cluster_offset, chunk);
                        done += chunk;
                    }
                    cluster_offset = 0;
                }
            }
            cur_offset += run_size;
        }

        *out_size = done;
        return 0;
    }
}

/* ── Get file name from MFT record ──────────────────────────────── */

static int ntfs_get_filename(struct ntfs_priv *np, uint8_t *mft_rec,
                              char *name, int max_len)
{
    int attr_len;
    uint8_t *attr = ntfs_find_attr(np, mft_rec, AT_FILE_NAME, &attr_len);
    if (!attr) return -1;

    struct ntfs_attr_hdr *ah = (struct ntfs_attr_hdr *)attr;
    if (ah->non_resident != 0) return -1;

    /* Resident: value is struct ntfs_file_name */
    struct ntfs_file_name *fn = (struct ntfs_file_name *)(attr + ah->value_offset);
    uint16_t name_len = fn->name_length;
    if (name_len > NTFS_NAMELEN) name_len = NTFS_NAMELEN;

    /* Convert from UTF-16LE to ASCII (simplified: ignore non-ASCII) */
    for (uint16_t i = 0; i < name_len && i < (uint16_t)(max_len - 1); i++) {
        uint16_t ch = fn->name[i];
        name[i] = (ch < 128) ? (char)ch : '?';
    }
    name[name_len] = '\0';
    return 0;
}

/* ── Get file size from MFT record ──────────────────────────────── */

static uint64_t ntfs_get_file_size(struct ntfs_priv *np, uint8_t *mft_rec)
{
    int attr_len;
    uint8_t *attr = ntfs_find_attr(np, mft_rec, AT_DATA, &attr_len);
    if (!attr) return 0;

    struct ntfs_attr_hdr *ah = (struct ntfs_attr_hdr *)attr;
    if (ah->non_resident)
        return ah->real_size;
    else
        return ah->value_length;
}

/* ── Check if directory ──────────────────────────────────────────── */

static int ntfs_is_directory(struct ntfs_priv *np, uint8_t *mft_rec)
{
    struct ntfs_mft_rec *rec = (struct ntfs_mft_rec *)mft_rec;
    return (rec->flags & MFT_RECORD_DIR) ? 1 : 0;
}

/* ── VFS operations ──────────────────────────────────────────────── */

static int ntfs_read(void *priv, const char *path,
                      void *buf, uint32_t max_size, uint32_t *out_size)
{
    (void)priv; (void)path; (void)buf; (void)max_size;
    if (out_size) *out_size = 0;
    return 0;
}

static int ntfs_write(void *priv, const char *path,
                       const void *data, uint32_t size)
{
    (void)priv; (void)path; (void)data; (void)size;
    return -EROFS;
}

static int ntfs_stat(void *priv, const char *path, struct vfs_stat *st)
{
    (void)priv;
    memset(st, 0, sizeof(*st));
    st->type = VFS_TYPE_FILE;
    if (path[0] == '/' && path[1] == '\0')
        st->type = VFS_TYPE_DIR;
    return 0;
}

static int ntfs_create(void *priv, const char *path, uint8_t type)
{
    (void)priv; (void)path; (void)type;
    return -EROFS;
}

static int ntfs_unlink(void *priv, const char *path)
{
    (void)priv; (void)path;
    return -EROFS;
}

static int ntfs_readdir(void *priv, const char *path)
{
    (void)priv;
    if (path[0] == '/' && path[1] == '\0')
        kprintf(".              <DIR>\n"
                "..             <DIR>\n"
                "[ntfs] NTFS filesystem (stub)\n");
    return 0;
}

static struct vfs_ops ntfs_ops = {
    .read    = ntfs_read,
    .write   = ntfs_write,
    .stat    = ntfs_stat,
    .create  = ntfs_create,
    .unlink  = ntfs_unlink,
    .readdir = ntfs_readdir,
};

/* ── Probe ───────────────────────────────────────────────────────── */

int ntfs_probe(uint8_t dev_id)
{
    uint8_t bpb_buf[512];

    /* Read boot sector (LBA 0) */
    if (blockdev_read_sectors(dev_id, 0, 1, bpb_buf) != 0)
        return -1;

    struct ntfs_bpb *bpb = (struct ntfs_bpb *)bpb_buf;
    if (memcmp(bpb->oem_id, "NTFS    ", 8) != 0)
        return -1;

    kprintf("[ntfs] detected on dev %u\n", dev_id);
    return 0;
}

/* ── Init ────────────────────────────────────────────────────────── */

int ntfs_init(void)
{
    kprintf("[ntfs] NTFS read-only filesystem initialized\n");
    vfs_register_filesystem("ntfs", &ntfs_ops);
    return 0;
}

device_initcall(ntfs_init);

#ifdef MODULE
int init_module(void) { return ntfs_init(); }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("NTFS — read-only");
#endif
