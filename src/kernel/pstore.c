#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#include "pstore.h"
#include "heap.h"
#include "timer.h"
#include "errno.h"
#include "compress.h"

/* Pointer to the persistent storage region */
static volatile struct pstore_record *pstore_region = NULL;
static int pstore_initialized = 0;
static int pstore_write_idx = 0;     /* next write index (wraps around) */
static int pstore_record_count = 0;  /* max(PSTORE_MAX_RECORDS, actual writes) */

static spinlock_t pstore_lock = SPINLOCK_INIT;

void pstore_init(void)
{
    /* Allocate pstore buffer from heap */
    pstore_region = (volatile struct pstore_record *)kcalloc(1, PSTORE_REGION_SIZE);
    if (!pstore_region) {
        kprintf("[!!] pstore: failed to allocate %d bytes\n", PSTORE_REGION_SIZE);
        return;
    }

    pstore_initialized = 1;
    kprintf("[OK] pstore: 64KB storage allocated at 0x%llx\n",
            (unsigned long long)(uintptr_t)pstore_region);
}

/*
 * Map an uncompressed type to its compressed counterpart.
 * Returns 0 if no compressed variant exists for the type.
 */
static inline uint8_t pstore_compressed_type(uint8_t type)
{
    switch (type) {
    case PSTORE_TYPE_DMESG: return PSTORE_TYPE_DMESG_COMPRESSED;
    case PSTORE_TYPE_PANIC: return PSTORE_TYPE_PANIC_COMPRESSED;
    case PSTORE_TYPE_OOPS:  return PSTORE_TYPE_OOPS_COMPRESSED;
    default:                return 0;
    }
}

/*
 * Map a compressed type back to the original uncompressed type.
 */
static inline uint8_t pstore_uncompressed_type(uint8_t type)
{
    switch (type) {
    case PSTORE_TYPE_DMESG_COMPRESSED: return PSTORE_TYPE_DMESG;
    case PSTORE_TYPE_PANIC_COMPRESSED: return PSTORE_TYPE_PANIC;
    case PSTORE_TYPE_OOPS_COMPRESSED:  return PSTORE_TYPE_OOPS;
    default:                           return type;
    }
}

int pstore_write(uint8_t type, const uint8_t *data, int len)
{
    if (!pstore_initialized || !pstore_region)
        return -ENODEV;
    if (len < 0 || len > PSTORE_MAX_DATA_LEN)
        return -EINVAL;

    /* Attempt compression for records above the threshold that have a
     * compressed variant.  If compression succeeds and saves space, store
     * the compressed version instead. */
    if (len > PSTORE_COMPRESS_THRESH) {
        uint8_t comp_type = pstore_compressed_type(type);
        if (comp_type != 0) {
            uint8_t comp_buf[LZSS_WORST_CASE(PSTORE_MAX_DATA_LEN)];
            int comp_len = lzss_compress(data, len, comp_buf, sizeof(comp_buf));
            if (comp_len > 0 && comp_len < len) {
                /* Compression saved space — store the compressed record */
                spinlock_acquire(&pstore_lock);

                volatile struct pstore_record *rec = &pstore_region[pstore_write_idx];
                rec->type = comp_type;
                rec->len = (uint8_t)(comp_len < 255 ? comp_len : 255);
                rec->sequence = (uint16_t)(pstore_write_idx + 1);
                rec->timestamp = timer_get_ticks();
                memcpy((void *)rec->data, comp_buf, comp_len);

                pstore_write_idx = (pstore_write_idx + 1) % PSTORE_MAX_RECORDS;
                if (pstore_record_count < PSTORE_MAX_RECORDS)
                    pstore_record_count++;

                spinlock_release(&pstore_lock);
                return 0;
            }
        }
    }

    /* Fall back to uncompressed write */
    spinlock_acquire(&pstore_lock);

    volatile struct pstore_record *rec = &pstore_region[pstore_write_idx];
    rec->type = type;
    rec->len = (uint8_t)(len < 255 ? len : 255);
    rec->sequence = (uint16_t)(pstore_write_idx + 1);
    rec->timestamp = timer_get_ticks();
    memcpy((void *)rec->data, data, len);

    pstore_write_idx = (pstore_write_idx + 1) % PSTORE_MAX_RECORDS;
    if (pstore_record_count < PSTORE_MAX_RECORDS)
        pstore_record_count++;

    spinlock_release(&pstore_lock);
    return 0;
}

int pstore_read(int index, uint8_t *buf, int len)
{
    if (!pstore_initialized || !pstore_region)
        return -ENODEV;
    if (index < 0 || index >= PSTORE_MAX_RECORDS)
        return -EINVAL;

    spinlock_acquire(&pstore_lock);

    volatile struct pstore_record *rec = &pstore_region[index];
    if (rec->type == 0 || rec->len == 0) {
        spinlock_release(&pstore_lock);
        return -ENOENT;
    }

    uint8_t data_type = rec->type;
    uint8_t data_len  = rec->len;

    spinlock_release(&pstore_lock);

    /* If the record is compressed, decompress it first */
    if (data_type == PSTORE_TYPE_DMESG_COMPRESSED ||
        data_type == PSTORE_TYPE_PANIC_COMPRESSED ||
        data_type == PSTORE_TYPE_OOPS_COMPRESSED) {
        uint8_t decomp_buf[PSTORE_MAX_DATA_LEN];
        int decomp_len = lzss_decompress(
            (const uint8_t *)rec->data, data_len,
            decomp_buf, sizeof(decomp_buf));
        if (decomp_len < 0)
            return decomp_len;
        uint8_t copy_len = (len < decomp_len) ? len : decomp_len;
        memcpy(buf, decomp_buf, copy_len);
        return (int)copy_len;
    }

    /* Uncompressed — direct copy */
    uint8_t copy_len = (len < data_len) ? len : data_len;
    memcpy(buf, (void *)rec->data, copy_len);
    return (int)copy_len;
}

int pstore_get_count(void)
{
    return pstore_record_count;
}
