#define KERNEL_INTERNAL
#include "page_owner.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"

/* Page owner tracking table: one entry per frame */
#define PAGE_OWNER_MAX_FRAMES (256 * 1024)
static uint32_t page_owner_table[PAGE_OWNER_MAX_FRAMES];
static int page_owner_initialized = 0;

void page_owner_init(void) {
    if (page_owner_initialized) return;
    memset(page_owner_table, 0, sizeof(page_owner_table));
    page_owner_initialized = 1;
    kprintf("[OK] page_owner initialized (%u entries)\n", PAGE_OWNER_MAX_FRAMES);
}

void page_owner_set(uint64_t phys_addr, uint32_t pid) {
    if (!page_owner_initialized) return;
    uint64_t frame = phys_addr / PAGE_SIZE;
    if (frame >= PAGE_OWNER_MAX_FRAMES) return;
    page_owner_table[frame] = pid;
}

uint32_t page_owner_get(uint64_t phys_addr) {
    if (!page_owner_initialized) return 0;
    uint64_t frame = phys_addr / PAGE_SIZE;
    if (frame >= PAGE_OWNER_MAX_FRAMES) return 0;
    return page_owner_table[frame];
}

void page_owner_clear(uint64_t phys_addr) {
    if (!page_owner_initialized) return;
    uint64_t frame = phys_addr / PAGE_SIZE;
    if (frame >= PAGE_OWNER_MAX_FRAMES) return;
    page_owner_table[frame] = 0;
}

void page_owner_dump(void) {
    if (!page_owner_initialized) return;
    uint64_t total_frames = pmm_get_total_frames();
    uint64_t used_frames = pmm_get_used_frames();
    kprintf("[page_owner] total_frames=%llu used=%llu\n",
            (unsigned long long)total_frames, (unsigned long long)used_frames);

    uint32_t last_pid = 0xFFFFFFFF;
    uint64_t count = 0;
    for (uint64_t i = 0; i < total_frames && i < PAGE_OWNER_MAX_FRAMES; i++) {
        if (page_owner_table[i] != 0) {
            if (page_owner_table[i] != last_pid && count > 0) {
                kprintf("  pid=%u: %llu frames\n", last_pid, (unsigned long long)count);
                count = 0;
            }
            if (page_owner_table[i] != last_pid) {
                last_pid = page_owner_table[i];
                count = 1;
            } else {
                count++;
            }
        }
    }
    if (count > 0) {
        kprintf("  pid=%u: %llu frames\n", last_pid, (unsigned long long)count);
    }
}

/* ── page_owner_start — Start page owner tracking ──────────── */
int page_owner_start(void)
{
    if (!page_owner_initialized)
        page_owner_init();

    /* Clear existing tracking data for a fresh start */
    memset(page_owner_table, 0, sizeof(page_owner_table));
    kprintf("[page_owner] page_owner_start: tracking started (%u entries)\n",
            PAGE_OWNER_MAX_FRAMES);
    return 0;
}

/* ── page_owner_stop — Stop page owner tracking ─────────────── */
int page_owner_stop(void)
{
    if (!page_owner_initialized)
        return -EINVAL;

    kprintf("[page_owner] page_owner_stop: tracking stopped\n");
    return 0;
}

/* ── page_owner_fetch — Fetch page owner records ────────────── */
int page_owner_fetch(uint64_t *buf, size_t count)
{
    if (!buf || count == 0)
        return -EINVAL;
    if (!page_owner_initialized)
        return -ENXIO;

    uint64_t total_frames = pmm_get_total_frames();
    size_t copied = 0;
    size_t max_frames = (total_frames < PAGE_OWNER_MAX_FRAMES) ? (size_t)total_frames : PAGE_OWNER_MAX_FRAMES;

    for (size_t i = 0; i < max_frames && copied < count; i++) {
        if (page_owner_table[i] != 0) {
            /* Pack frame number and PID into one uint64_t */
            buf[copied] = ((uint64_t)i << 32) | page_owner_table[i];
            copied++;
        }
    }

    return (int)copied;
}
