#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#include "pstore.h"
#include "pmm.h"
#include "vmm.h"
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
    /* Try to reserve the fixed physical address range */
    pmm_reserve_frames(PSTORE_RESERVE_PHYS, PSTORE_REGION_SIZE);

    /* Map the region into kernel virtual address space */
    uint64_t virt = (uint64_t)PHYS_TO_VIRT(PSTORE_RESERVE_PHYS);

    /* Page-map the 64KB region (16 pages at 4K each) */
    for (uint64_t offset = 0; offset < PSTORE_REGION_SIZE; offset += PAGE_SIZE) {
        uint64_t phys = PSTORE_RESERVE_PHYS + offset;
        uint64_t vaddr = virt + offset;
        int ret = vmm_map_page(vaddr, phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
        if (ret != 0) {
            kprintf("[!!] pstore: failed to map page at 0x%llx\n", (unsigned long long)vaddr);
            return;
        }
    }

    pstore_region = (volatile struct pstore_record *)virt;
    memset((void *)pstore_region, 0, PSTORE_REGION_SIZE);

    /* Scan for existing records by checking sequence numbers */
    int count = 0;
    int max_seq = -1;
    int max_idx = 0;
    for (int i = 0; i < PSTORE_MAX_RECORDS; i++) {
        if (pstore_region[i].type != 0 && pstore_region[i].sequence != 0) {
            count++;
            if ((int)pstore_region[i].sequence > max_seq) {
                max_seq = pstore_region[i].sequence;
                max_idx = i;
            }
        }
    }
    pstore_record_count = count;
    pstore_write_idx = (max_idx + 1) % PSTORE_MAX_RECORDS;

    pstore_initialized = 1;
    kprintf("[OK] pstore: 64KB persistent storage at phys 0x%llx (%d existing records)\n",
            (unsigned long long)PSTORE_RESERVE_PHYS, count);
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
