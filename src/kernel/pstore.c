#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#include "pstore.h"
#include "pmm.h"
#include "vmm.h"
#include "timer.h"
#include "notifier.h"
#include "errno.h"

/* ── Virtual-memory access to the persistent region ────────────────────
 * The boot page tables identity-map the first 1 GB of physical memory
 * at both low addresses (0x0 – 0x3FFFFFFF) and at KERNEL_VMA_OFFSET.
 * So PHYS_TO_VIRT(PSTORE_REGION_PADDR) is always accessible without
 * additional page-table manipulation. */

static volatile struct pstore_region_header *pstore_hdr = NULL;
static volatile uint8_t *pstore_region_base = NULL;  /* virtual base */
static int pstore_initialized = 0;

/* Forward declarations */
static void pstore_init_notifier(void);

/* Spinlock for concurrent write access (SMP-safe) */
static spinlock_t pstore_lock = SPINLOCK_INIT;

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Return the virtual address of the n-th record slot */
static inline volatile struct pstore_record *pstore_slot(unsigned int idx)
{
    uint64_t base = (uint64_t)(uintptr_t)pstore_region_base;
    uint64_t rec_off = sizeof(struct pstore_region_header)
                       + (uint64_t)idx * PSTORE_RECORD_SIZE;
    return (volatile struct pstore_record *)(base + rec_off);
}

/* Check whether a slot appears to contain a valid record */
static inline int slot_valid(const volatile struct pstore_record *rec)
{
    return (rec->magic == PSTORE_RECORD_MAGIC && rec->length > 0
            && rec->length <= PSTORE_MAX_DATA_LEN);
}

/* ── Initialisation ──────────────────────────────────────────────────── */

void pstore_init(void)
{
    uint64_t phys = PSTORE_REGION_PADDR;
    uint64_t virt = (uint64_t)PHYS_TO_VIRT(phys);

    /* Ensure the physical region is reserved in PMM so nothing else
     * allocates over it.  (It's below 1 MB so PMM ignores it by default,
     * but this is an explicit safety net.) */
    pmm_reserve_frames(phys, PSTORE_REGION_SIZE);

    /* Map each page of the region into the virtual address space.
     * This is redundant for the identity-mapped first 1 MB but ensures
     * the mapping exists even on systems that later tear down the
     * low identity map. */
    for (uint64_t off = 0; off < PSTORE_REGION_SIZE; off += PAGE_SIZE) {
        int ret = vmm_map_page(virt + off, phys + off,
                               VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
        if (ret != 0) {
            kprintf("[!!] pstore: failed to map page at phys 0x%llx\n",
                    (unsigned long long)(phys + off));
            return;
        }
    }

    pstore_region_base = (volatile uint8_t *)virt;
    pstore_hdr = (volatile struct pstore_region_header *)virt;

    /* ── Check for existing data from a previous boot ── */
    if (pstore_hdr->region_magic == PSTORE_REGION_MAGIC &&
        pstore_hdr->version == 1 &&
        pstore_hdr->record_count > 0) {

        kprintf("[..] pstore: recovering %u records from previous boot\n",
                (unsigned int)pstore_hdr->record_count);

        /* Print recovered records */
        pstore_recover();

        /* Mark that we recovered */
        pstore_hdr->recovered = 1;
    } else {
        kprintf("[..] pstore: fresh region (no previous data)\n");
    }

    /* ── Format a fresh region header for this boot cycle ── */
    /* We keep the old records physically in memory (for post-mortem
     * analysis tools) but start writing new records at slot 0. */
    memset((void *)(uintptr_t)pstore_hdr, 0, sizeof(*pstore_hdr));
    pstore_hdr->region_magic  = PSTORE_REGION_MAGIC;
    pstore_hdr->version       = 1;
    pstore_hdr->write_slot    = 0;
    pstore_hdr->record_count  = 0;
    pstore_hdr->boot_timestamp = timer_get_ticks();
    pstore_hdr->recovered     = 0;

    pstore_initialized = 1;

    kprintf("[OK] pstore: 64 KB persistent region at phys 0x%llx virt 0x%llx\n",
            (unsigned long long)phys, (unsigned long long)virt);

    /* Ensure the notifier subsystem is initialised before we register.
     * (The existing boot sequence never calls notifier_init() explicitly,
     * so we do it here to guarantee the panic chain works.) */
    notifier_init();

    /* Register the panic notifier so pstore captures a final marker
     * on kernel panic / oops. */
    pstore_init_notifier();
}

/* ── Write a record ──────────────────────────────────────────────────── */

int pstore_write(uint8_t type, const uint8_t *data, int len)
{
    if (!pstore_initialized || !pstore_hdr)
        return -ENODEV;
    if (len < 0 || len > PSTORE_MAX_DATA_LEN - 1)  /* -1 for type byte */
        return -EINVAL;
    if (!data && len > 0)
        return -EINVAL;

    spinlock_acquire(&pstore_lock);

    unsigned int slot = pstore_hdr->write_slot;
    volatile struct pstore_record *rec = pstore_slot(slot);

    /* Fill the record */
    rec->magic     = PSTORE_RECORD_MAGIC;
    rec->length    = (uint32_t)(len + 1);   /* type byte + payload */
    rec->timestamp = timer_get_ticks();
    rec->data[0]   = type;                   /* type as first byte */
    if (len > 0 && data)
        memcpy((void *)(uintptr_t)rec->data + 1, data, (size_t)len);

    /* Advance the write slot (ring buffer) */
    pstore_hdr->write_slot = (slot + 1) % PSTORE_MAX_RECORDS;
    if (pstore_hdr->record_count < PSTORE_MAX_RECORDS)
        pstore_hdr->record_count++;

    spinlock_release(&pstore_lock);
    return 0;
}

/* ── Read a record ───────────────────────────────────────────────────── */

int pstore_read(int index, uint8_t *buf, int len)
{
    if (!pstore_initialized || !pstore_hdr)
        return -ENODEV;
    if (index < 0 || buf == NULL || len <= 0)
        return -EINVAL;

    spinlock_acquire(&pstore_lock);

    unsigned int count = pstore_hdr->record_count;
    if ((unsigned int)index >= count) {
        spinlock_release(&pstore_lock);
        return -ENOENT;
    }

    /* Records are stored newest-first in the ring buffer.
     * The newest record is at slot (write_slot - 1) mod N.
     * Index 0 = newest, index count-1 = oldest. */
    unsigned int slot;
    if (pstore_hdr->write_slot == 0)
        slot = PSTORE_MAX_RECORDS - 1;
    else
        slot = pstore_hdr->write_slot - 1;

    /* Walk back `index` steps through the ring */
    slot = (slot - (unsigned int)index + PSTORE_MAX_RECORDS) % PSTORE_MAX_RECORDS;

    volatile struct pstore_record *rec = pstore_slot(slot);
    if (!slot_valid(rec)) {
        spinlock_release(&pstore_lock);
        return -ENOENT;
    }

    uint32_t data_len = rec->length;
    if (data_len > (uint32_t)len)
        data_len = (uint32_t)len;

    memcpy(buf, (void *)(uintptr_t)rec->data, data_len);
    spinlock_release(&pstore_lock);
    return (int)data_len;
}

/* ── Get record count ────────────────────────────────────────────────── */

int pstore_get_count(void)
{
    if (!pstore_initialized || !pstore_hdr)
        return 0;
    return (int)pstore_hdr->record_count;
}

/* ── Boot-time recovery ──────────────────────────────────────────────── */

void pstore_recover(void)
{
    if (!pstore_hdr)
        return;

    unsigned int count = pstore_hdr->record_count;
    if (count == 0)
        return;

    kprintf("\n========== PSTORE RECOVERY (%u records from previous boot) ==========\n",
            count);

    for (unsigned int i = 0; i < count; i++) {
        /* Walk backwards from write_slot-1, same as pstore_read */
        unsigned int slot;
        if (pstore_hdr->write_slot == 0)
            slot = PSTORE_MAX_RECORDS - 1;
        else
            slot = pstore_hdr->write_slot - 1;

        slot = (slot - i + PSTORE_MAX_RECORDS) % PSTORE_MAX_RECORDS;

        volatile struct pstore_record *rec = pstore_slot(slot);
        if (!slot_valid(rec))
            continue;

        uint8_t type = rec->data[0];
        uint32_t dlen = rec->length - 1;  /* skip type byte */
        if (dlen > 256)
            dlen = 256;  /* cap display length */

        /* Print record header */
        kprintf("  [%02u] type=%u len=%u ts=%llu\n",
                i, type, (unsigned int)dlen,
                (unsigned long long)rec->timestamp);

        /* Print the payload as text (assuming ASCII / printable) */
        if (dlen > 0) {
            /* Null-terminate for safe printing */
            char print_buf[512];
            uint32_t copy = dlen < sizeof(print_buf) - 1 ? dlen : sizeof(print_buf) - 1;
            memcpy(print_buf, (void *)(uintptr_t)rec->data + 1, copy);
            print_buf[copy] = '\0';
            kprintf("       %s\n", print_buf);
        }
    }

    kprintf("================================================================\n");
}

/* ── Panic / oops notifier ───────────────────────────────────────────── */

static struct notifier_block pstore_nb = {
    .notifier_call = NULL,  /* set in pstore_init_notifier */
    .next          = NULL,
};

/* Called on kernel panic (via NOTIFIER_PANIC chain).
 * Writes a final "panic occurred" marker and dmesg snapshot. */
int pstore_panic_notifier(struct notifier_block *nb,
                          unsigned long action, void *data)
{
    (void)nb;
    (void)data;

    if (action == 0) {
        /* Write a panic marker record */
        const char *msg = "KERNEL PANIC — system halted";
        pstore_write(PSTORE_TYPE_PANIC,
                     (const uint8_t *)msg, (int)strlen(msg));
    }

    return 0;
}

/* Register the panic notifier.  Called from kernel boot after
 * notifier_init() and pstore_init() have completed. */
void pstore_init_notifier(void)
{
    pstore_nb.notifier_call = pstore_panic_notifier;
    int ret = notifier_chain_register(NOTIFIER_PANIC, &pstore_nb);
    if (ret == 0) {
        kprintf("[OK] pstore: panic notifier registered\n");
    } else {
        kprintf("[!!] pstore: failed to register panic notifier (%d)\n", ret);
    }
}

/* ── Stub: pstore_erase ────────────────────────────────────────────── */
static int pstore_erase(int index)
{
    (void)index;
    kprintf("[PSTORE] pstore_erase: not yet implemented\n");
    return 0;
}

/* ── Stub: pstore_open ─────────────────────────────────────────────── */
static int pstore_open(void)
{
    kprintf("[PSTORE] pstore_open: not yet implemented\n");
    return 0;
}

/* ── Stub: pstore_close ────────────────────────────────────────────── */
static void pstore_close(void)
{
    kprintf("[PSTORE] pstore_close: not yet implemented\n");
}
