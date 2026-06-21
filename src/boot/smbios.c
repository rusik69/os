/*
 * smbios.c — SMBIOS entry table walker
 *
 * Scans the BIOS area for the SMBIOS entry point structure, then
 * walks the SMBIOS structure table to find entries by type.
 *
 * This implementation runs at boot time, using direct physical
 * memory access via PHYS_TO_VIRT.
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "errno.h"

/* ── SMBIOS entry point structure (v2.1+) ──────────────────────────────── */

struct smbios_entry_point {
    char     anchor[4];              /* "_SM_" */
    uint8_t  checksum;
    uint8_t  length;
    uint8_t  major_version;
    uint8_t  minor_version;
    uint16_t max_structure_size;
    uint8_t  revision;
    uint8_t  formatted_area[5];
    char     intermediate_anchor[5]; /* "_DMI_" */
    uint8_t  intermediate_checksum;
    uint16_t structure_table_length;
    uint32_t structure_table_address;
    uint16_t number_of_structures;
    uint8_t  smbios_bcd_revision;
} __attribute__((packed));

/* ── SMBIOS structure header (every structure starts with this) ──────── */

struct smbios_header {
    uint8_t  type;
    uint8_t  length;
    uint16_t handle;
    /* type-specific data and strings follow */
} __attribute__((packed));

/* SMBIOS v3 entry point (64-bit) */
struct smbios_entry_point_v3 {
    char     anchor[5];              /* "_SM3_" */
    uint8_t  checksum;
    uint8_t  length;
    uint8_t  major_version;
    uint8_t  minor_version;
    uint8_t  docrev;
    uint8_t  revision;
    uint8_t  reserved;
    uint32_t max_structure_size;
    uint64_t structure_table_address;
} __attribute__((packed));

/* ── Scan range ─────────────────────────────────────────────────────── */

/* SMBIOS entry point is typically found in the BIOS area between
 * 0xF0000 and 0xFFFFF, aligned to 16 bytes. */
#define SMBIOS_SCAN_START    0x000F0000ULL
#define SMBIOS_SCAN_END      0x00100000ULL
#define SMBIOS_SCAN_ALIGN    16

/* ── Saved SMBIOS state ─────────────────────────────────────────────── */

static struct smbios_header *g_smbios_table = NULL;
static uint32_t g_smbios_table_length = 0;
static int g_smbios_found = 0;

/* ── Checksum helper ────────────────────────────────────────────────── */

static int smbios_checksum(const void *data, uint32_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++)
        sum += bytes[i];
    return (sum == 0) ? 0 : -1;
}

/* ── Find the SMBIOS entry point ────────────────────────────────────── */

int smbios_init(void)
{
    /* Already initialised */
    if (g_smbios_found)
        return 0;

    /* Scan the BIOS area for the entry point signature */
    for (uint64_t addr = SMBIOS_SCAN_START; addr < SMBIOS_SCAN_END;
         addr += SMBIOS_SCAN_ALIGN) {
        char sig[4];
        memcpy(sig, (void *)(uintptr_t)addr, 4);

        if (memcmp(sig, "_SM_", 4) == 0) {
            struct smbios_entry_point *ep =
                (struct smbios_entry_point *)(uintptr_t)addr;

            /* Validate checksums */
            if (smbios_checksum(ep, ep->length) < 0)
                continue;

            if (memcmp(ep->intermediate_anchor, "_DMI_", 5) != 0)
                continue;

            if (smbios_checksum(ep, 0x10) < 0)
                continue;

            /* Found a valid SMBIOS entry point */
            g_smbios_table =
                (struct smbios_header *)(uintptr_t)ep->structure_table_address;
            g_smbios_table_length = ep->structure_table_length;
            g_smbios_found = 1;

            kprintf("[SMBIOS] Found v%u.%u entry point at 0x%llx\n",
                    ep->major_version, ep->minor_version,
                    (unsigned long long)addr);
            kprintf("[SMBIOS] Table at 0x%x, %u bytes, %u structures\n",
                    ep->structure_table_address,
                    ep->structure_table_length,
                    ep->number_of_structures);
            return 0;
        }
    }

    /* Try SMBIOS v3 entry point */
    for (uint64_t addr = SMBIOS_SCAN_START; addr < SMBIOS_SCAN_END;
         addr += SMBIOS_SCAN_ALIGN) {
        char sig[5];
        memcpy(sig, (void *)(uintptr_t)addr, 5);

        if (memcmp(sig, "_SM3_", 5) == 0) {
            struct smbios_entry_point_v3 *ep =
                (struct smbios_entry_point_v3 *)(uintptr_t)addr;

            if (smbios_checksum(ep, ep->length) < 0)
                continue;

            g_smbios_table =
                (struct smbios_header *)(uintptr_t)ep->structure_table_address;
            g_smbios_table_length = ep->max_structure_size;
            g_smbios_found = 1;

            kprintf("[SMBIOS] Found v3 entry point at 0x%llx\n",
                    (unsigned long long)addr);
            kprintf("[SMBIOS] Table at 0x%llx, %u bytes\n",
                    (unsigned long long)ep->structure_table_address,
                    ep->max_structure_size);
            return 0;
        }
    }

    kprintf("[SMBIOS] No SMBIOS entry point found\n");
    return -ENOENT;
}

/* ── Walk SMBIOS table and return first entry matching type ─────────── */

void *smbios_find_entry(uint8_t type)
{
    if (!g_smbios_found || !g_smbios_table)
        return NULL;

    uint32_t offset = 0;

    while (offset < g_smbios_table_length) {
        struct smbios_header *hdr =
            (struct smbios_header *)((uint8_t *)g_smbios_table + offset);

        /* End-of-table marker: type=127, length=4 */
        if (hdr->type == 127)
            break;

        if (hdr->type == type) {
            kprintf("[SMBIOS] Found entry type %u at offset %u\n",
                    type, offset);
            return (void *)hdr;
        }

        /* Skip to next structure: advance past the formatted area
         * (length bytes) and then past the null-terminated strings
         * (double null = end of strings). */
        uint32_t next_offset = offset + hdr->length;
        while (next_offset + 1 < g_smbios_table_length) {
            if (((uint8_t *)g_smbios_table)[next_offset] == 0 &&
                ((uint8_t *)g_smbios_table)[next_offset + 1] == 0) {
                next_offset += 2;
                break;
            }
            next_offset++;
        }

        if (next_offset <= offset)
            break; /* Prevent infinite loop on malformed data */

        offset = next_offset;
    }

    return NULL;
}

/* ── Walk SMBIOS table calling callback for each entry ──────────────── */

int smbios_walk(int (*callback)(struct smbios_header *entry, void *ctx),
                void *ctx)
{
    if (!g_smbios_found || !g_smbios_table)
        return -ENOENT;

    uint32_t offset = 0;

    while (offset < g_smbios_table_length) {
        struct smbios_header *hdr =
            (struct smbios_header *)((uint8_t *)g_smbios_table + offset);

        if (hdr->type == 127)
            break;

        if (callback) {
            int ret = callback(hdr, ctx);
            if (ret != 0)
                return ret;
        }

        uint32_t next_offset = offset + hdr->length;
        while (next_offset + 1 < g_smbios_table_length) {
            if (((uint8_t *)g_smbios_table)[next_offset] == 0 &&
                ((uint8_t *)g_smbios_table)[next_offset + 1] == 0) {
                next_offset += 2;
                break;
            }
            next_offset++;
        }

        if (next_offset <= offset)
            break;

        offset = next_offset;
    }

    return 0;
}
