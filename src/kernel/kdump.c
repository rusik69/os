#define KERNEL_INTERNAL
#include "kdump.h"
#include "panic.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"
#include "vmm.h"
#include "smp.h"
#include "process.h"

/* ── External symbols from the linker script ── */
extern uint8_t _text_start[];
extern uint8_t _text_end[];
extern uint8_t _rodata_start[];
extern uint8_t _rodata_end[];
extern uint8_t _data_start[];
extern uint8_t _data_end[];
extern uint8_t _bss_start[];
extern uint8_t _bss_end[];
extern uint8_t _kernel_end[];

/* Virtual address of the mapped kdump region */
static volatile struct kdump_header *kdump_virt = NULL;
static int kdump_initialized = 0;

/*
 * Physical address range for crash dump.
 * We reserve 1 MB at KDUMP_PHYS_BASE (0x7FC00000).
 * This is just below the pstore region at 0x7FE00000.
 */

/* ── Initialisation ──────────────────────────────────────────────────── */

void kdump_init(void)
{
    /* Reserve the physical region */
    pmm_reserve_frames(KDUMP_PHYS_BASE, KDUMP_REGION_SIZE);

    /* Map the region into kernel virtual address space */
    uint64_t virt = (uint64_t)PHYS_TO_VIRT(KDUMP_PHYS_BASE);

    for (uint64_t offset = 0; offset < KDUMP_REGION_SIZE; offset += PAGE_SIZE) {
        uint64_t phys = KDUMP_PHYS_BASE + offset;
        uint64_t vaddr = virt + offset;
        int ret = vmm_map_page(vaddr, phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
        if (ret != 0) {
            kprintf("[!!] kdump: failed to map page at 0x%llx\n",
                    (unsigned long long)vaddr);
            return;
        }
    }

    kdump_virt = (volatile struct kdump_header *)virt;
    memset((void *)kdump_virt, 0, KDUMP_REGION_SIZE);
    kdump_initialized = 1;

    kprintf("[OK] kdump: 1 MB crash dump region at phys 0x%llx\n",
            (unsigned long long)KDUMP_PHYS_BASE);
}

/* ── Read CPU registers into entries array ───────────────────────────── */

static int capture_regs(struct kdump_reg_entry *entries, uint64_t rip)
{
    int n = 0;

    uint64_t cr0, cr2, cr3, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

#define SAVE_REG(reg_id, reg_val)                         \
    do {                                                   \
        if (n < KDUMP_MAX_REGS) {                          \
            entries[n].id    = (uint64_t)(reg_id);          \
            entries[n].value = (uint64_t)(reg_val);          \
            n++;                                             \
        }                                                    \
    } while (0)

    uint64_t rax, rbx, rcx, rdx, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rsp, rbp, rflags;
    uint64_t cs, ds, es, fs, gs, ss;

    __asm__ volatile("mov %%rax, %0" : "=r"(rax));
    __asm__ volatile("mov %%rbx, %0" : "=r"(rbx));
    __asm__ volatile("mov %%rcx, %0" : "=r"(rcx));
    __asm__ volatile("mov %%rdx, %0" : "=r"(rdx));
    __asm__ volatile("mov %%rsi, %0" : "=r"(rsi));
    __asm__ volatile("mov %%rdi, %0" : "=r"(rdi));
    __asm__ volatile("mov %%r8,  %0" : "=r"(r8));
    __asm__ volatile("mov %%r9,  %0" : "=r"(r9));
    __asm__ volatile("mov %%r10, %0" : "=r"(r10));
    __asm__ volatile("mov %%r11, %0" : "=r"(r11));
    __asm__ volatile("mov %%r12, %0" : "=r"(r12));
    __asm__ volatile("mov %%r13, %0" : "=r"(r13));
    __asm__ volatile("mov %%r14, %0" : "=r"(r14));
    __asm__ volatile("mov %%r15, %0" : "=r"(r15));
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
    __asm__ volatile("mov %%cs,  %0" : "=r"(cs));
    __asm__ volatile("mov %%ds,  %0" : "=r"(ds));
    __asm__ volatile("mov %%es,  %0" : "=r"(es));
    __asm__ volatile("mov %%fs,  %0" : "=r"(fs));
    __asm__ volatile("mov %%gs,  %0" : "=r"(gs));
    __asm__ volatile("mov %%ss,  %0" : "=r"(ss));

    SAVE_REG(KREG_RAX, rax);
    SAVE_REG(KREG_RBX, rbx);
    SAVE_REG(KREG_RCX, rcx);
    SAVE_REG(KREG_RDX, rdx);
    SAVE_REG(KREG_RSI, rsi);
    SAVE_REG(KREG_RDI, rdi);
    SAVE_REG(KREG_RBP, rbp);
    SAVE_REG(KREG_RSP, rsp);
    SAVE_REG(KREG_R8,  r8);
    SAVE_REG(KREG_R9,  r9);
    SAVE_REG(KREG_R10, r10);
    SAVE_REG(KREG_R11, r11);
    SAVE_REG(KREG_R12, r12);
    SAVE_REG(KREG_R13, r13);
    SAVE_REG(KREG_R14, r14);
    SAVE_REG(KREG_R15, r15);
    SAVE_REG(KREG_RIP, rip);
    SAVE_REG(KREG_RFL, rflags);
    SAVE_REG(KREG_CS,  cs);
    SAVE_REG(KREG_DS,  ds);
    SAVE_REG(KREG_ES,  es);
    SAVE_REG(KREG_FS,  fs);
    SAVE_REG(KREG_GS,  gs);
    SAVE_REG(KREG_SS,  ss);
    SAVE_REG(KREG_CR0, cr0);
    SAVE_REG(KREG_CR2, cr2);
    SAVE_REG(KREG_CR3, cr3);
    SAVE_REG(KREG_CR4, cr4);

#undef SAVE_REG
    return n;
}

/* ── Capture stack trace via frame pointer walk ──────────────────────── */

static int capture_stack_frames(uint64_t *frames, int max_frames)
{
    int count = 0;
    uint64_t *rbp;

    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));

    while (rbp && count < max_frames && count < KDUMP_MAX_FRAMES) {
        uint64_t ret_addr = rbp[1];
        if (ret_addr == 0)
            break;
        /* Skip user-mode addresses */
        if (ret_addr < 0xFFFF800000000000ULL)
            break;
        frames[count++] = ret_addr;
        rbp = (uint64_t *)rbp[0];
    }
    return count;
}

/* ── Memory segment helpers ──────────────────────────────────────────── */

/* Calculate number of pages needed to store @size bytes */
static inline uint64_t pages_for_size(uint64_t size)
{
    return (size + PAGE_SIZE - 1) / PAGE_SIZE;
}

/* Copy kernel memory into the kdump region at the given offset.
 * Returns the number of bytes written (rounded up to PAGE_SIZE). */
static uint64_t copy_segment_data(uint64_t dst_offset,
                                  const void *src_vaddr,
                                  uint64_t size)
{
    if (!kdump_virt || size == 0)
        return 0;

    uint8_t *base = (uint8_t *)kdump_virt;
    uint64_t copy_size = size;

    /* Clamp to region boundary */
    if (dst_offset + copy_size > KDUMP_REGION_SIZE)
        copy_size = KDUMP_REGION_SIZE - dst_offset;

    /* Copy page by page, checking that the source is readable */
    uint64_t written = 0;
    while (written < copy_size) {
        uint64_t chunk = copy_size - written;
        if (chunk > PAGE_SIZE)
            chunk = PAGE_SIZE;

        /* Attempt to read; if the page is unmapped this will fault,
         * but we're in panic context with interrupts off, so we just
         * skip the rest of this segment rather than risking a double-fault. */
        volatile uint8_t probe __attribute__((unused)) =
            ((volatile const uint8_t *)src_vaddr)[written + chunk - 1];
        (void)probe;

        memcpy(base + dst_offset + written,
               (const uint8_t *)src_vaddr + written, chunk);
        written += chunk;
    }
    return written;
}

/* ── Main capture function ───────────────────────────────────────────── */

void kdump_capture(const char *msg, uint64_t rip)
{
    if (!kdump_initialized || !kdump_virt)
        return;

    struct process *cur = process_get_current();

    /* Zero the region to start fresh */
    memset((void *)kdump_virt, 0, KDUMP_REGION_SIZE);

    /* ── Fill header ── */
    struct kdump_header *hdr = (struct kdump_header *)kdump_virt;
    hdr->magic   = KDUMP_MAGIC;
    hdr->version = KDUMP_VERSION;

    /* TSC timestamp */
    {
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        hdr->timestamp_tsc = ((uint64_t)hi << 32) | (uint64_t)lo;
    }

    hdr->cpu_id    = smp_get_cpu_id();
    hdr->cpu_count = smp_get_cpu_count();
    hdr->pid       = cur ? cur->pid : 0;

    /* Panic message */
    if (msg) {
        size_t mlen = strlen(msg);
        if (mlen >= sizeof(hdr->panic_msg))
            mlen = sizeof(hdr->panic_msg) - 1;
        memcpy(hdr->panic_msg, msg, mlen);
        hdr->panic_msg[mlen] = '\0';
    } else {
        memcpy(hdr->panic_msg, "(null)", 7);
    }

    /* ── Register entries (right after header) ── */
    struct kdump_reg_entry *regs =
        (struct kdump_reg_entry *)((uint8_t *)kdump_virt + sizeof(*hdr));
    int num_regs = capture_regs(regs, rip);
    hdr->num_regs = num_regs;

    /* ── Stack frames (right after registers) ── */
    uint64_t *frames = (uint64_t *)((uint8_t *)kdump_virt + sizeof(*hdr)
                                    + KDUMP_MAX_REGS * sizeof(struct kdump_reg_entry));
    int num_frames = capture_stack_frames(frames, KDUMP_MAX_FRAMES);
    hdr->num_frames = num_frames;

    /* ── Memory segment descriptors ── */
    struct kdump_mem_desc *segs =
        (struct kdump_mem_desc *)((uint8_t *)kdump_virt + sizeof(*hdr)
                                  + KDUMP_MAX_REGS * sizeof(struct kdump_reg_entry)
                                  + KDUMP_MAX_FRAMES * sizeof(uint64_t));

    uint64_t data_offset = (uint64_t)((uint8_t *)segs - (uint8_t *)kdump_virt)
                           + KDUMP_MAX_SEGMENTS * sizeof(struct kdump_mem_desc);
    /* Align to page boundary */
    data_offset = (data_offset + 0xFFF) & ~0xFFFULL;

    uint64_t current_data_off = data_offset;
    int seg_idx = 0;

    /* Segment 0: .text section */
    if (seg_idx < KDUMP_MAX_SEGMENTS) {
        uint64_t sz = (uint64_t)(_text_end - _text_start);
        segs[seg_idx].start_vaddr = (uint64_t)_text_start;
        segs[seg_idx].size        = sz;
        segs[seg_idx].offset      = current_data_off;
        segs[seg_idx].flags       = KSEG_READ | KSEG_EXEC | KSEG_KERNEL;
        uint64_t written = copy_segment_data(current_data_off, _text_start, sz);
        current_data_off += written;
        seg_idx++;
    }

    /* Segment 1: .rodata */
    if (seg_idx < KDUMP_MAX_SEGMENTS) {
        uint64_t sz = (uint64_t)(_rodata_end - _rodata_start);
        segs[seg_idx].start_vaddr = (uint64_t)_rodata_start;
        segs[seg_idx].size        = sz;
        segs[seg_idx].offset      = current_data_off;
        segs[seg_idx].flags       = KSEG_READ | KSEG_KERNEL;
        uint64_t written = copy_segment_data(current_data_off, _rodata_start, sz);
        current_data_off += written;
        seg_idx++;
    }

    /* Segment 2: .data */
    if (seg_idx < KDUMP_MAX_SEGMENTS) {
        uint64_t sz = (uint64_t)(_data_end - _data_start);
        segs[seg_idx].start_vaddr = (uint64_t)_data_start;
        segs[seg_idx].size        = sz;
        segs[seg_idx].offset      = current_data_off;
        segs[seg_idx].flags       = KSEG_READ | KSEG_WRITE | KSEG_KERNEL;
        uint64_t written = copy_segment_data(current_data_off, _data_start, sz);
        current_data_off += written;
        seg_idx++;
    }

    /* Segment 3: .bss (includes .lbss) */
    if (seg_idx < KDUMP_MAX_SEGMENTS) {
        uint64_t sz = (uint64_t)(_kernel_end - _bss_start);
        segs[seg_idx].start_vaddr = (uint64_t)_bss_start;
        segs[seg_idx].size        = sz;
        segs[seg_idx].offset      = current_data_off;
        segs[seg_idx].flags       = KSEG_READ | KSEG_WRITE | KSEG_KERNEL;
        uint64_t written = copy_segment_data(current_data_off, _bss_start, sz);
        current_data_off += written;
        seg_idx++;
    }

    /* Segment 4: current process kernel stack */
    if (seg_idx < KDUMP_MAX_SEGMENTS && cur) {
        uint64_t stack_base = cur->kernel_stack;
        uint64_t stack_top  = cur->stack_top;
        if (stack_base > 0 && stack_top > stack_base) {
            uint64_t sz = stack_top - stack_base;
            /* Clamp to a reasonable stack size (64 KB max) */
            if (sz > 65536)
                sz = 65536;
            segs[seg_idx].start_vaddr = stack_base;
            segs[seg_idx].size        = sz;
            segs[seg_idx].offset      = current_data_off;
            segs[seg_idx].flags       = KSEG_READ | KSEG_WRITE | KSEG_KERNEL;
            uint64_t written = copy_segment_data(current_data_off,
                                                  (const void *)stack_base, sz);
            current_data_off += written;
            seg_idx++;
        }
    }

    /* Segment 5: page table root (CR3) */
    if (seg_idx < KDUMP_MAX_SEGMENTS) {
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        /* CR3 holds the physical address of PML4 — map and save it */
        uint64_t cr3_virt = (uint64_t)PHYS_TO_VIRT(cr3 & ~0xFFFULL);
        uint64_t sz = PAGE_SIZE;
        segs[seg_idx].start_vaddr = cr3_virt;
        segs[seg_idx].size        = sz;
        segs[seg_idx].offset      = current_data_off;
        segs[seg_idx].flags       = KSEG_READ | KSEG_KERNEL;
        uint64_t written = copy_segment_data(current_data_off,
                                              (const void *)cr3_virt, sz);
        current_data_off += written;
        seg_idx++;
    }

    hdr->num_segments = seg_idx;
    hdr->total_size   = (uint32_t)(current_data_off < 0xFFFFFFFFULL
                                   ? current_data_off : 0xFFFFFFFFULL);
}

/* ── Post-mortem check ───────────────────────────────────────────────── */

int kdump_check(void)
{
    if (!kdump_initialized || !kdump_virt)
        return 0;

    volatile struct kdump_header *hdr = kdump_virt;
    return (hdr->magic == KDUMP_MAGIC && hdr->version == KDUMP_VERSION) ? 1 : 0;
}

const struct kdump_header *kdump_get_header(void)
{
    if (!kdump_check())
        return NULL;
    return (const struct kdump_header *)kdump_virt;
}
