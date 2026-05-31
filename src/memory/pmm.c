#include "pmm.h"
#include "string.h"
#include "printf.h"

/* Multiboot1 info structure (relevant fields) */
struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
};

struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type; /* 1 = available */
} __attribute__((packed));

#define MAX_FRAMES (256 * 1024) /* up to 1GB with 4KB pages */
static uint8_t  frame_bitmap[MAX_FRAMES / 8];
static uint16_t frame_refcount[MAX_FRAMES]; /* COW reference counts */
static uint64_t total_frames = 0;
static uint64_t used_frames = 0;
static uint64_t pmm_hint = 0; /* last-known free frame; speeds up allocation */

static void bitmap_set(uint64_t frame) {
    if (frame >= MAX_FRAMES) return;
    frame_bitmap[frame / 8] |= (1 << (frame % 8));
}

static void bitmap_clear(uint64_t frame) {
    if (frame >= MAX_FRAMES) return;
    frame_bitmap[frame / 8] &= ~(1 << (frame % 8));
}

static int bitmap_test(uint64_t frame) {
    if (frame >= MAX_FRAMES) return 1; /* out-of-range frames appear used */
    return frame_bitmap[frame / 8] & (1 << (frame % 8));
}

extern char _kernel_end[];

void pmm_init(uint64_t multiboot_info_phys) {
    /* Mark all frames as used initially */
    memset(frame_bitmap, 0xFF, sizeof(frame_bitmap));
    used_frames = MAX_FRAMES;

    struct multiboot_info *mbi = (struct multiboot_info *)PHYS_TO_VIRT(multiboot_info_phys);

    /* Check if memory map is available (bit 6 of flags) */
    if (mbi->flags & (1 << 6)) {
        uint64_t mmap_addr = (uint64_t)PHYS_TO_VIRT(mbi->mmap_addr);
        uint64_t mmap_end = mmap_addr + mbi->mmap_length;

        while (mmap_addr < mmap_end) {
            struct multiboot_mmap_entry *entry = (struct multiboot_mmap_entry *)mmap_addr;

            if (entry->type == 1 && entry->addr + entry->len > 0x100000) {
                uint64_t start = entry->addr;
                uint64_t end = entry->addr + entry->len;

                if (start < 0x100000) start = 0x100000;

                uint64_t start_frame = (start + PAGE_SIZE - 1) / PAGE_SIZE;
                uint64_t end_frame = end / PAGE_SIZE;

                if (end_frame > MAX_FRAMES) end_frame = MAX_FRAMES;
                if (end_frame > total_frames) total_frames = end_frame;

                for (uint64_t f = start_frame; f < end_frame; f++) {
                    bitmap_clear(f);
                    used_frames--;
                }
            }
            mmap_addr += entry->size + 4; /* size field doesn't include itself */
        }
    } else {
        /* Fallback: use mem_upper (KB above 1MB) */
        uint64_t mem_bytes = (uint64_t)(mbi->mem_upper) * 1024 + 0x100000;
        uint64_t end_frame = mem_bytes / PAGE_SIZE;
        if (end_frame > MAX_FRAMES) end_frame = MAX_FRAMES;
        total_frames = end_frame;

        for (uint64_t f = 0x100000 / PAGE_SIZE; f < end_frame; f++) {
            bitmap_clear(f);
            used_frames--;
        }
    }

    /* Reserve kernel frames — use physical address since _kernel_end has a high VMA */
    uint64_t kernel_end_phys = VIRT_TO_PHYS((uint64_t)(uintptr_t)_kernel_end);
    uint64_t kernel_end_frame = (kernel_end_phys + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t f = 0; f < kernel_end_frame && f < MAX_FRAMES; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
        }
    }

    /* Recount used frames based on actual total */
    used_frames = 0;
    for (uint64_t f = 0; f < total_frames; f++) {
        if (bitmap_test(f)) used_frames++;
    }
}

void pmm_reserve_frames(uint64_t phys_start, uint64_t byte_size) {
    uint64_t start_frame = phys_start / PAGE_SIZE;
    uint64_t end_frame   = (phys_start + byte_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (end_frame > MAX_FRAMES) end_frame = MAX_FRAMES;
    for (uint64_t f = start_frame; f < end_frame; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            used_frames++;
        }
    }
}

void pmm_advance_hint(uint64_t phys_addr) {
    uint64_t frame = phys_addr / PAGE_SIZE;
    if (frame + 1 > pmm_hint)
        pmm_hint = frame + 1;
}

void pmm_oom_kill(void); /* defined in syscall.c — kills a process to free memory */

uint64_t pmm_alloc_frame(void) {
    /* Start from hint to avoid scanning already-allocated frames */
    uint64_t i = pmm_hint;
    do {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_frames++;
            frame_refcount[i] = 1;
            pmm_hint = i + 1;
            if (pmm_hint >= total_frames) pmm_hint = 0;
            return i * PAGE_SIZE;
        }
        i++;
        if (i >= total_frames) i = 0;
    } while (i != pmm_hint);
    /* Out of memory — invoke OOM killer and retry once */
    pmm_oom_kill();
    /* Second attempt */
    i = pmm_hint;
    do {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_frames++;
            frame_refcount[i] = 1;
            pmm_hint = i + 1;
            if (pmm_hint >= total_frames) pmm_hint = 0;
            return i * PAGE_SIZE;
        }
        i++;
        if (i >= total_frames) i = 0;
    } while (i != pmm_hint);
    return 0; /* truly out of memory */
}

/* Allocate count contiguous frames. Returns first frame physical addr, or 0 on failure. */
uint64_t *pmm_alloc_frames(size_t count) {
    if (count == 0) return NULL;
    if (count == 1) return (uint64_t *)pmm_alloc_frame();

    /* Scan for 'count' contiguous free frames */
    uint64_t start = pmm_hint;
    uint64_t found = 0;

    uint64_t i = pmm_hint;
    do {
        if (!bitmap_test(i)) {
            if (found == 0) start = i;
            found++;
            if (found == count) {
                /* Allocate all frames */
                for (uint64_t j = start; j < start + count; j++) {
                    bitmap_set(j);
                    used_frames++;
                    frame_refcount[j] = 1;
                }
                pmm_hint = start + count;
                if (pmm_hint >= total_frames) pmm_hint = 0;
                return (uint64_t *)(start * PAGE_SIZE);
            }
        } else {
            found = 0;
        }
        i++;
        if (i >= total_frames) i = 0;
    } while (i != pmm_hint);

    return NULL; /* out of memory */
}

void pmm_free_frame(uint64_t addr) {
    if (addr & (PAGE_SIZE - 1)) return;
    uint64_t frame = addr / PAGE_SIZE;
    if (frame >= MAX_FRAMES) return;
    if (!bitmap_test(frame)) return;
    /* Safety: refuse to free a frame with outstanding COW references */
    if (frame_refcount[frame] > 1) return;
    bitmap_clear(frame);
    frame_refcount[frame] = 0;
    used_frames--;
}

void pmm_ref_frame(uint64_t phys) {
    uint64_t frame = phys / PAGE_SIZE;
    if (frame < MAX_FRAMES && frame_refcount[frame] < 65535)
        frame_refcount[frame]++;
}

int pmm_unref_frame(uint64_t phys) {
    uint64_t frame = phys / PAGE_SIZE;
    if (frame >= MAX_FRAMES) return 0;
    if (frame_refcount[frame] == 0) return 0;
    frame_refcount[frame]--;
    if (frame_refcount[frame] == 0) {
        bitmap_clear(frame);
        used_frames--;
    }
    return (int)frame_refcount[frame];
}

int pmm_refcount(uint64_t phys) {
    uint64_t frame = phys / PAGE_SIZE;
    if (frame >= MAX_FRAMES) return 0;
    return (int)frame_refcount[frame];
}

uint64_t pmm_get_total_frames(void) { return total_frames; }
uint64_t pmm_get_used_frames(void)  { return used_frames; }
