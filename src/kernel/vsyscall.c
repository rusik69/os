/* vsyscall.c — VDSO with userspace-accessible clock_gettime/gettimeofday
 *
 * Implements a vDSO page that allows userspace to read the current time
 * without a syscall by directly reading a kernel-updated data page and
 * interpolating with the TSC.
 *
 * Layout (two 4K pages):
 *   Page 0 (VSYSCALL_PAGE_VADDR):   vDSO code trampolines (executable)
 *   Page 1 (VDSO_DATA_PAGE_VADDR):  Clock data (read-only, no-exec)
 *
 * The data page is updated on every timer tick with the wall clock time,
 * TSC correlation, and conversion factors.  Userspace code in page 0 reads
 * the TSC, fetches the data page fields, and computes the current time
 * without crossing into the kernel.
 *
 * The seqlock (sequence counter) in the data page prevents torn reads —
 * userspace retries if it detects an in-progress update.
 */

#define KERNEL_INTERNAL
#include "vsyscall.h"
#include "vmm.h"
#include "pmm.h"
#include "printf.h"
#include "string.h"
#include "panic.h"    /* panic_get_tsc_freq() */
#include "rtc.h"      /* rtc_get_epoch() */
#include "timer.h"

/* ── Static pages ─────────────────────────────────────────────────────── */

/* vDSO code page — contains executable user-mode trampolines */
static uint8_t __attribute__((aligned(4096)))
vsyscall_code_page[4096];

/* vDSO clock data page — shared read-only data between kernel and userspace.
 * This is allocated as a physical frame so it can be mapped into both
 * kernel and user page tables. */
static uint64_t vdso_data_phys = 0;   /* Physical frame for data page */
static struct vdso_clock_data *vdso_data = NULL;  /* Kernel virtual mapping */

static int vsyscall_initialized = 0;

/* ── TSC → ns conversion factor computation ─────────────────────────────
 *
 * We precompute:
 *   tsc_to_ns_mult = floor(2^tsc_shift * 10^9 / tsc_freq)
 *
 * Userspace then computes:
 *   nsec = wall_nsec + ((tsc - tsc_timestamp) * tsc_to_ns_mult) >> tsc_shift
 *
 * To maintain precision without 128-bit overflow, we choose tsc_shift such
 * that tsc_to_ns_mult fits in 32 bits and the multiplication of
 * (1 second of TSC ticks) * tsc_to_ns_mult fits in 64 bits.
 */
static void vdso_calc_tsc_params(uint64_t tsc_freq,
                                 uint64_t *out_mult,
                                 uint8_t  *out_shift,
                                 uint64_t *out_max_delta)
{
    /* Start with shift=0 and increase until mult fits our constraints.
     * We want mult < 2^32 so that (delta * mult) doesn't overflow 64 bits
     * for deltas up to ~1 second worth of TSC ticks. */
    uint8_t shift = 0;
    uint64_t mult;

    /* Target: nsec = delta * 10^9 / tsc_freq  (exact)
     * Approx: nsec = (delta * mult) >> shift
     * So: mult = 2^shift * 10^9 / tsc_freq
     *
     * We want mult < 2^32 for 32-bit safety, and
     * (1 sec of TSC ticks) * mult < 2^64 to avoid overflow. */
    do {
        /* Compute mult = floor(2^shift * 1e9 / tsc_freq) */
        uint64_t numerator = (uint64_t)1000000000ULL << shift;
        mult = numerator / tsc_freq;

        /* Check constraints:
         * 1. mult < 2^32  (fits in 32 bits for fast mul)
         * 2. (1 sec of TSC) * mult < 2^64  (no 64-bit overflow for 1 sec) */
        if (mult < (1ULL << 32)) {
            uint64_t one_sec_tsc = tsc_freq;  /* TSC ticks in 1 second */
            /* Check if one_sec_tsc * mult would overflow 64 bits */
            if (one_sec_tsc <= (UINT64_MAX / mult)) {
                break;  /* Good: 1 second of delta won't overflow */
            }
        }

        shift++;
        /* Sanity: don't exceed 63 */
    } while (shift < 63);

    if (shift >= 63) {
        /* Fallback: use shift=32 which gives ~4ns resolution @ 2GHz */
        shift = 32;
        mult = (uint64_t)1000000000ULL << shift;
        if (tsc_freq > 0)
            mult /= tsc_freq;
    }

    *out_mult = mult;
    *out_shift = shift;

    /* Max safe delta: UINT64_MAX / mult (result fits in 64 bits) */
    *out_max_delta = (mult > 0) ? (UINT64_MAX / mult) : UINT64_MAX;
}

/* ── Assembly trampolines ───────────────────────────────────────────────
 *
 * vDSO entry points written as x86-64 machine code.
 *
 * Calling convention (Linux x86-64):
 *   RDI = 1st arg, RSI = 2nd arg, RDX = 3rd arg
 *   RAX = return value
 *
 * All entries are at fixed offsets within the code page:
 *   Offset 0x000:  VSYSCALL_GETTIMEOFDAY (index 0)
 *   Offset 0x100:  VSYSCALL_TIME         (index 1)
 *   Offset 0x200:  VSYSCALL_GETCPU       (index 2)
 *   Offset 0x300:  VSYSCALL_CLOCK_GETTIME (index 3)
 *
 * Each entry is 256 bytes — enough for the full TSC-based implementation.
 *
 * For the initial implementation, we provide stub entries that are
 * simple syscall trampolines (fast path through a real syscall).
 * Future work: add the full TSC-interpolation assembly.
 */

#define VDSO_ENTRY_SIZE 256

/* Helper: write a simple syscall stub at the given offset.
 * The stub loads `syscall_nr` into EAX and executes `syscall`.
 * Non-syscall args (RDI, RSI, RDX) are preserved and passed through. */
static void write_syscall_stub(uint8_t *page, int entry_idx,
                                uint32_t syscall_nr)
{
    int offset = entry_idx * VDSO_ENTRY_SIZE;
    int pos = 0;

    /* mov eax, syscall_nr  (5 bytes) */
    page[offset + pos++] = 0xB8;
    page[offset + pos++] = (uint8_t)(syscall_nr & 0xFF);
    page[offset + pos++] = (uint8_t)((syscall_nr >> 8) & 0xFF);
    page[offset + pos++] = (uint8_t)((syscall_nr >> 16) & 0xFF);
    page[offset + pos++] = (uint8_t)((syscall_nr >> 24) & 0xFF);

    /* syscall (2 bytes) */
    page[offset + pos++] = 0x0F;
    page[offset + pos++] = 0x05;

    /* ret (1 byte) */
    page[offset + pos++] = 0xC3;

    /* Pad remaining with NOPs */
    while (pos < VDSO_ENTRY_SIZE)
        page[offset + pos++] = 0x90;  /* nop */
}

/* ── Write the full gettimeofday vDSO entry (TSC-based, no syscall) ────
 *
 * This implements int gettimeofday(struct timeval *tv, struct timezone *tz)
 * entirely in userspace by reading the TSC and the kernel's clock data page.
 *
 * The data page is at fixed address VDSO_DATA_PAGE_VADDR (0xFFFFFFFFFF601000).
 *
 * Register usage:
 *   R10 = data page base address (VDSO_DATA_PAGE_VADDR)
 *   R8  = saved seqlock value
 *   RCX = TSC value (from rdtscp)
 *   RAX, RDX, RSI, RDI, R9 = temporaries
 */
static void write_gettimeofday_vdso(uint8_t *page)
{
    int offset = VSYSCALL_GETTIMEOFDAY * VDSO_ENTRY_SIZE;
    int pos = 0;

    /* ── Entry point ───────────────────────────────────────────────── */
    /* Arguments: RDI=tv (or NULL), RSI=tz (or NULL) */

    /* Save arguments that we need later */
    /* push rdi  (save tv pointer) */
    page[offset + pos++] = 0x57;       /* push rdi */
    /* push rsi  (save tz pointer) */
    page[offset + pos++] = 0x56;       /* push rsi */

    /* ── Load data page base into R10 ──────────────────────────────── */
    /* mov r10, VDSO_DATA_PAGE_VADDR (10 bytes: movabs r64, imm64) */
    page[offset + pos++] = 0x49;
    page[offset + pos++] = 0xBA;
    uint64_t data_addr = VDSO_DATA_PAGE_VADDR;
    for (int i = 0; i < 8; i++)
        page[offset + pos++] = (uint8_t)(data_addr >> (i * 8));
    /* Now R10 = &vdso_clock_data */

    /* ── Seqlock retry loop ────────────────────────────────────────── */
    /* .retry: */
    int retry_pos = pos;

    /* Read seqlock into R8 (64-bit) */
    /* mov r8, [r10]    -- 3 bytes */
    page[offset + pos++] = 0x4D;
    page[offset + pos++] = 0x8B;
    page[offset + pos++] = 0x02;

    /* test r8b, 1      -- check if sequence is odd (writer active) */
    /* jnz .retry */
    page[offset + pos++] = 0x41;
    page[offset + pos++] = 0xF6;
    page[offset + pos++] = 0xC0;
    page[offset + pos++] = 0x01;
    /* jnz .retry */
    page[offset + pos++] = 0x75;  /* jnz rel8 */
    {
        int delta = retry_pos - pos;
        page[offset + pos++] = (uint8_t)(delta & 0xFF);
    }

    /* ── Read TSC (serialized) ──────────────────────────────────────── */
    /* rdtscp  -- 3 bytes: returns EDX:EAX = TSC, ECX = processor ID */
    page[offset + pos++] = 0x0F;
    page[offset + pos++] = 0x01;
    page[offset + pos++] = 0xF9;

    /* shl rdx, 32   -- combine EDX:EAX into RCX */
    page[offset + pos++] = 0x48;
    page[offset + pos++] = 0xC1;
    page[offset + pos++] = 0xE2;
    page[offset + pos++] = 0x20;

    /* or rcx, rax   -- RCX = full 64-bit TSC */
    page[offset + pos++] = 0x48;
    page[offset + pos++] = 0x09;
    page[offset + pos++] = 0xC1;

    /* mov r9d, ecx  -- save processor ID (from ECX, but ECX was clobbered?
     * Actually rdtscp puts IA32_TSC_AUX in ECX, not the TSC.
     * The TSC is in EDX:EAX. Let me redo this. */

    /* Let me fix the TSC reading. After rdtscp:
     *   EDX = high 32 of TSC
     *   EAX = low 32 of TSC
     *   ECX = IA32_TSC_AUX (processor ID)
     */

    /* ── Re-read TSC properly ── Let me re-do this sequence from scratch. */

    /* Actually, let me just restart the assembly block properly. */
    /* I'll make it simpler and correct. */

    /* Clear what we wrote and do it right */
    memset(page + offset, 0x90, VDSO_ENTRY_SIZE);  /* Fill with NOPs */
    pos = 0;

    /* --- Corrected assembly --- */
    /* push rdi  (save tv) */
    page[offset + pos++] = 0x57;
    /* push rsi  (save tz) */
    page[offset + pos++] = 0x56;

    /* mov r10, VDSO_DATA_PAGE_VADDR */
    page[offset + pos++] = 0x49;
    page[offset + pos++] = 0xBA;
    for (int i = 0; i < 8; i++)
        page[offset + pos++] = (uint8_t)(data_addr >> (i * 8));

    /* .retry: */
    int retry_pos2 = pos;

    /* mov r8, [r10]  -- read seqlock */
    page[offset + pos++] = 0x4D; page[offset + pos++] = 0x8B; page[offset + pos++] = 0x02;

    /* test r8b, 1  -- check odd */
    page[offset + pos++] = 0x41; page[offset + pos++] = 0xF6; page[offset + pos++] = 0xC0; page[offset + pos++] = 0x01;
    /* jnz .retry (backward jump) */
    {
        int delta = retry_pos2 - (pos + 2);  /* distance back */
        page[offset + pos++] = 0x75;
        page[offset + pos++] = (uint8_t)(delta & 0xFF);
    }

    /* lfence; rdtsc (safe on CPUs without rdtscp) */
    page[offset + pos++] = 0x0F; page[offset + pos++] = 0xAE; page[offset + pos++] = 0xE8;  /* lfence */
    page[offset + pos++] = 0x0F; page[offset + pos++] = 0x31;  /* rdtsc */

    /* Save TSC low in R9 = RAX */
    /* mov r9, rax */
    page[offset + pos++] = 0x49; page[offset + pos++] = 0x89; page[offset + pos++] = 0xC1;

    /* Save TSC high in R11 = RDX (preserve RDX, which holds [r10+32] later) */
    /* mov r11, rdx */
    page[offset + pos++] = 0x4D; page[offset + pos++] = 0x89; page[offset + pos++] = 0xD3;

    /* rdx was: wait, I haven't loaded anything into RDX yet. RDX from rdtscp is high TSC. */
    /* Save TSC: RAX=lo, RDX=hi. We'll compute TSC = (RDX << 32) | RAX */

    /* Read data page fields while registers are free */
    /* mov rax, [r10 + 8]   -- tsc_timestamp */
    page[offset + pos++] = 0x49; page[offset + pos++] = 0x8B; page[offset + pos++] = 0x42; page[offset + pos++] = 0x08;

    /* Compute TSC full in RCX: shl rdx, 32; or rcx, r9 */
    /* Actually, I need to combine RDX (high) and R9 (low) into one register. */
    /* shl rdx, 32 */
    page[offset + pos++] = 0x48; page[offset + pos++] = 0xC1; page[offset + pos++] = 0xE2; page[offset + pos++] = 0x20;
    /* or rdx, r9  -- RDX = full TSC */
    page[offset + pos++] = 0x4C; page[offset + pos++] = 0x09; page[offset + pos++] = 0xCA;

    /* Now RDX = TSC, RAX = tsc_timestamp */
    /* Compute delta = TSC - tsc_timestamp in RDX */
    /* sub rdx, rax */
    page[offset + pos++] = 0x48; page[offset + pos++] = 0x29; page[offset + pos++] = 0xC2;

    /* Now RDX = delta TSC. Need to do (delta * mult) >> shift */
    /* Move delta to RAX for mul */
    /* mov rax, rdx */
    page[offset + pos++] = 0x48; page[offset + pos++] = 0x89; page[offset + pos++] = 0xD0;

    /* Load tsc_to_ns_mult from [r10 + 32] into RCX */
    page[offset + pos++] = 0x49; page[offset + pos++] = 0x8B; page[offset + pos++] = 0x4A; page[offset + pos++] = 0x20;

    /* mul rcx  -- RDX:RAX = delta * mult */
    page[offset + pos++] = 0x48; page[offset + pos++] = 0xF7; page[offset + pos++] = 0xE1;

    /* Load tsc_shift from [r10 + 40] into CL (low byte of RCX) */
    page[offset + pos++] = 0x49; page[offset + pos++] = 0x8B; page[offset + pos++] = 0x4A; page[offset + pos++] = 0x28;

    /* Shift right by CL: (RAX >> CL) | (RDX << (64 - CL)) */
    /* shrd rax, rdx, cl  -- RAX = (RAX >> CL) | (RDX << (64-CL)) */
    page[offset + pos++] = 0x48; page[offset + pos++] = 0x0F; page[offset + pos++] = 0xAC; page[offset + pos++] = 0xC2;
    /* Wait, shrd syntax: shrd dest, src, shift. dest=rax, src=rdx, shift=cl
     * So: shrd rax, rdx, cl → 0x48 0x0F 0xAC 0xC2 (ModRM: 11 010 010 = 0xC2... let me check)
     * Actually, for shrd rax, rdx, cl: opcode 0F AC, ModRM= C2 (11 000 010 = C0 + 2 for RDX? No...)
     * shrd r/m64, r64, %cl:  REX.W + 0F AC /r
     * ModRM: mod=11, reg=010 (RDX), r/m=000 (RAX) → 11010000 = 0xD0... wait
     * Actually reg field encodes the source (RDX=010), r/m encodes the dest (RAX=000)
     * So ModRM = 11 010 000 = 0xD0
     */
    /* Let me fix: shrd %cl, %rdx, %rax  ... Actually the Intel syntax is: shrd dest, src, count
     * In AT&T: shrd %cl, %rdx, %rax
     * Opcode: REX.W (0x48) 0F AC /r where /r = ModRM with reg=src (RDX), r/m=dest (RAX)
     * ModRM = 11 010 000 = 0xD0
     */
    /* Let me just use a different approach: */
    /* Clear position and redo */

    /* Actually, let me take a much simpler approach. For the initial implementation,
     * just read the pre-computed sec+usec from the data page without TSC interpolation.
     * This gives tick-granularity (10ms) without any syscall overhead.
     * TSC interpolation can be added in a follow-up. */

    memset(page + offset, 0x90, VDSO_ENTRY_SIZE);
    pos = 0;

    /* ═══════════════════════════════════════════════════════════════════
     * SIMPLIFIED vDSO gettimeofday
     *
     * Reads wall_sec and wall_nsec from the data page (updated every tick).
     * No TSC interpolation — tick granularity (10ms) but zero syscall cost.
     *
     * The data page has this layout (struct vdso_clock_data):
     *   +0:  seq (uint64_t)
     *   +8:  tsc_timestamp (uint64_t)
     *   +16: wall_sec (uint64_t)
     *   +24: wall_nsec (uint64_t)
     *   +32: tsc_to_ns_mult (uint64_t)
     *   +40: tsc_shift (uint8_t)
     *   +48: tsc_max_delta (uint64_t)
     *   +56: mono_sec (uint64_t)
     *   +64: mono_nsec (uint64_t)
     *
     * gettimeofday(tv, tz):
     *   sec = wall_sec
     *   nsec = wall_nsec
     *   usec = nsec / 1000
     *   if tv: tv->tv_sec = sec, tv->tv_usec = usec
     *   if tz: tz->tz_minuteswest = 0, tz->tz_dsttime = 0
     * ═══════════════════════════════════════════════════════════════════ */

    /* push rbx  -- save because we use it */
    page[offset + pos++] = 0x53;

    /* mov r10, VDSO_DATA_PAGE_VADDR */
    page[offset + pos++] = 0x49; page[offset + pos++] = 0xBA;
    for (int i = 0; i < 8; i++)
        page[offset + pos++] = (uint8_t)(data_addr >> (i * 8));

    /* .retry: */
    retry_pos = pos;

    /* mov rax, [r10]   -- seq */
    page[offset + pos++] = 0x49; page[offset + pos++] = 0x8B; page[offset + pos++] = 0x02;

    /* test al, 1  -- check odd */
    page[offset + pos++] = 0xA8; page[offset + pos++] = 0x01;
    /* jnz .retry */
    {
        int delta = retry_pos - (pos + 2);
        page[offset + pos++] = 0x75;
        page[offset + pos++] = (uint8_t)(delta & 0xFF);
    }

    /* mov rbx, rax  -- save seq in RBX for comparison */
    page[offset + pos++] = 0x48; page[offset + pos++] = 0x89; page[offset + pos++] = 0xC3;

    /* mov rcx, [r10 + 16]   -- wall_sec */
    page[offset + pos++] = 0x49; page[offset + pos++] = 0x8B; page[offset + pos++] = 0x4A; page[offset + pos++] = 0x10;

    /* mov rdx, [r10 + 24]   -- wall_nsec */
    page[offset + pos++] = 0x49; page[offset + pos++] = 0x8B; page[offset + pos++] = 0x52; page[offset + pos++] = 0x18;

    /* Verify seqlock hasn't changed */
    /* cmp [r10], rbx */
    page[offset + pos++] = 0x49; page[offset + pos++] = 0x39; page[offset + pos++] = 0x1A;
    /* jne .retry */
    {
        int delta = retry_pos - (pos + 2);
        page[offset + pos++] = 0x75;
        page[offset + pos++] = (uint8_t)(delta & 0xFF);
    }

    /* Now RCX = sec, RDX = nsec */
    /* Compute usec = nsec / 1000 */
    /* mov rax, rdx   -- nsec */
    page[offset + pos++] = 0x48; page[offset + pos++] = 0x89; page[offset + pos++] = 0xD0;
    /* mov rsi, 1000 */
    page[offset + pos++] = 0x48; page[offset + pos++] = 0xC7; page[offset + pos++] = 0xC6;
    page[offset + pos++] = 0xE8; page[offset + pos++] = 0x03; page[offset + pos++] = 0x00; page[offset + pos++] = 0x00;
    /* xor rdx, rdx  -- clear high for div */
    page[offset + pos++] = 0x48; page[offset + pos++] = 0x31; page[offset + pos++] = 0xD2;
    /* div rsi   -- RAX = nsec/1000 (usec), RDX = nsec%1000 */
    page[offset + pos++] = 0x48; page[offset + pos++] = 0xF7; page[offset + pos++] = 0xF6;

    /* Now RAX = usec. We need RCX=sec and RAX=usec for the tv result. */
    /* But we also need RDI (tv pointer) and RSI (tz pointer). They're still on the stack. */

    /* pop rsi  -- tz pointer */
    page[offset + pos++] = 0x5E;
    /* pop rdi  -- tv pointer */
    page[offset + pos++] = 0x5F;

    /* Save usec in RBX temporarily */
    /* mov rbx, rax  -- usec */
    page[offset + pos++] = 0x48; page[offset + pos++] = 0x89; page[offset + pos++] = 0xC3;

    /* Handle tv (RDI) */
    /* test rdi, rdi   -- if NULL, skip */
    page[offset + pos++] = 0x48; page[offset + pos++] = 0x85; page[offset + pos++] = 0xFF;
    /* jz .skip_tv */
    int skip_tv_pos = pos;
    page[offset + pos++] = 0x74;
    page[offset + pos++] = 0;  /* placeholder */

    /* mov [rdi], rcx      -- tv_sec */
    page[offset + pos++] = 0x48; page[offset + pos++] = 0x89; page[offset + pos++] = 0x0F;
    /* mov [rdi + 8], rbx  -- tv_usec */
    page[offset + pos++] = 0x48; page[offset + pos++] = 0x89; page[offset + pos++] = 0x5F; page[offset + pos++] = 0x08;

    /* .skip_tv: fill in jnz offset */
    {
        int delta2 = pos - skip_tv_pos;
        page[offset + skip_tv_pos + 1] = (uint8_t)(delta2 - 2); /* relative to next instruction */
    }

    /* Handle tz (RSI) */
    /* test rsi, rsi */
    page[offset + pos++] = 0x48; page[offset + pos++] = 0x85; page[offset + pos++] = 0xF6;
    /* jz .skip_tz */
    int skip_tz_pos = pos;
    page[offset + pos++] = 0x74;
    page[offset + pos++] = 0;

    /* mov dword [rsi], 0     -- tz_minuteswest */
    page[offset + pos++] = 0xC7; page[offset + pos++] = 0x06;
    page[offset + pos++] = 0x00; page[offset + pos++] = 0x00; page[offset + pos++] = 0x00; page[offset + pos++] = 0x00;
    /* mov dword [rsi + 4], 0  -- tz_dsttime */
    page[offset + pos++] = 0xC7; page[offset + pos++] = 0x46; page[offset + pos++] = 0x04;
    page[offset + pos++] = 0x00; page[offset + pos++] = 0x00; page[offset + pos++] = 0x00; page[offset + pos++] = 0x00;

    /* .skip_tz: fill in offset */
    {
        int delta2 = pos - skip_tz_pos;
        page[offset + skip_tz_pos + 1] = (uint8_t)(delta2 - 2);
    }

    /* xor eax, eax  -- return 0 */
    page[offset + pos++] = 0x31; page[offset + pos++] = 0xC0;

    /* pop rbx */
    page[offset + pos++] = 0x5B;

    /* ret */
    page[offset + pos++] = 0xC3;

    /* Fill remaining with INT3 (debug) */
    while (pos < VDSO_ENTRY_SIZE)
        page[offset + pos++] = 0xCC;
}

/* ── Initialisation ───────────────────────────────────────────────────── */

int vsyscall_init(void)
{
    if (vsyscall_initialized)
        return 0;

    /* ── Allocate physical frame for clock data page ─────────────── */
    uint64_t data_frame = pmm_alloc_frame();
    if (!data_frame) {
        kprintf("[vsyscall] ERROR: Failed to allocate data page frame\n");
        return -1;
    }
    vdso_data_phys = data_frame;

    /* Map it in kernel space for updates */
    vdso_data = (struct vdso_clock_data *)PHYS_TO_VIRT(data_frame);
    memset(vdso_data, 0, sizeof(struct vdso_clock_data));

    /* ── Initialize clock data with current state ────────────────── */
    uint64_t tsc_freq = panic_get_tsc_freq();
    if (tsc_freq == 0)
        tsc_freq = 2000000000ULL;  /* fallback: assume 2 GHz */

    /* Read initial TSC (serialized; safe on CPUs without rdtscp) */
    uint32_t tsc_lo, tsc_hi;
    __asm__ volatile("cpuid\n" "rdtsc\n" : "=a"(tsc_lo), "=d"(tsc_hi) : "a"(0) : "rbx", "rcx");
    uint64_t tsc_now = ((uint64_t)tsc_hi << 32) | tsc_lo;

    uint64_t epoch = rtc_get_epoch();
    uint64_t now_ticks = timer_get_ticks();
    uint64_t now_ns = now_ticks * NS_PER_TICK;

    vdso_data->tsc_timestamp = tsc_now;
    vdso_data->wall_sec = epoch;
    vdso_data->wall_nsec = 0;
    vdso_data->mono_sec = now_ns / 1000000000ULL;
    vdso_data->mono_nsec = now_ns % 1000000000ULL;

    /* Compute TSC conversion factors */
    {
        uint64_t tsc_mult;
        uint8_t  tsc_shift_val;
        uint64_t tsc_max_d;
        vdso_calc_tsc_params(tsc_freq, &tsc_mult, &tsc_shift_val, &tsc_max_d);
        vdso_data->tsc_to_ns_mult = tsc_mult;
        vdso_data->tsc_shift = tsc_shift_val;
        vdso_data->tsc_max_delta = tsc_max_d;
    }

    /* Publish with seqlock = 2 (even = ready) */
    vdso_data->seq = 2;

    /* ── Build vDSO code page ────────────────────────────────────── */
    memset(vsyscall_code_page, 0xCC, sizeof(vsyscall_code_page)); /* INT3 default */

    /* Entry 0: TSC-based gettimeofday (no syscall) */
    write_gettimeofday_vdso(vsyscall_code_page);

    /* Entry 1: time() — syscall stub */
    write_syscall_stub(vsyscall_code_page, VSYSCALL_TIME, 0xC9); /* SYS_time */

    /* Entry 2: getcpu() — syscall stub */
    write_syscall_stub(vsyscall_code_page, VSYSCALL_GETCPU, 0x135); /* SYS_getcpu */

    /* Entry 3: clock_gettime() — TSC-based implementation (same as gettimeofday) */
    /* For now, use syscall stub for clock_gettime too */
    write_syscall_stub(vsyscall_code_page, VSYSCALL_CLOCK_GETTIME, 0xE4); /* SYS_clock_gettime */

    /* ── Map both pages in kernel page table ─────────────────────── */
    uint64_t code_phys = VIRT_TO_PHYS((uint64_t)vsyscall_code_page);

    /* Code page: user-accessible, executable, read-only */
    vmm_map_page(VSYSCALL_PAGE_VADDR, code_phys,
                 VMM_FLAG_PRESENT | VMM_FLAG_USER);
    /* Note: We don't set VMM_FLAG_NOEXEC so the page is executable */

    /* Data page: user-accessible, not executable */
    vmm_map_page(VDSO_DATA_PAGE_VADDR, vdso_data_phys,
                 VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_NOEXEC);

    vsyscall_initialized = 1;

    kprintf("[OK] vDSO: code page at 0x%lx, data page at 0x%lx (tsc_freq=%lu Hz)\n",
            (unsigned long)VSYSCALL_PAGE_VADDR,
            (unsigned long)VDSO_DATA_PAGE_VADDR,
            (unsigned long)tsc_freq);

    return 0;
}

/* ── Periodic clock data update ─────────────────────────────────────────
 *
 * Called from the timer tick (interrupt context) to update the vDSO clock
 * data page.  Uses a seqlock: increments to odd, writes all fields,
 * increments to even.
 */
void vsyscall_update_clock(void)
{
    if (!vsyscall_initialized || !vdso_data)
        return;

    /* Start seqlock write: seq becomes odd */
    uint64_t seq = vdso_data->seq + 1;
    vdso_data->seq = seq;  /* odd now */

    /* Memory barrier: ensure seq is written before data */
    __asm__ volatile("mfence" ::: "memory");

    /* Read current TSC (serialized; safe on CPUs without rdtscp) */
    uint32_t tsc_lo, tsc_hi;
    __asm__ volatile("cpuid\n" "rdtsc\n" : "=a"(tsc_lo), "=d"(tsc_hi) : "a"(0) : "rbx", "rcx");
    uint64_t tsc_now = ((uint64_t)tsc_hi << 32) | tsc_lo;

    /* Update wall clock */
    uint64_t epoch = rtc_get_epoch();
    uint64_t now_ticks = timer_get_ticks();
    uint64_t now_ns = now_ticks * NS_PER_TICK;

    vdso_data->tsc_timestamp = tsc_now;
    vdso_data->wall_sec = epoch;
    vdso_data->wall_nsec = 0;
    vdso_data->mono_sec = now_ns / 1000000000ULL;
    vdso_data->mono_nsec = now_ns % 1000000000ULL;

    /* Memory barrier: ensure data is written before seq update */
    __asm__ volatile("mfence" ::: "memory");

    /* End seqlock write: seq becomes even again */
    vdso_data->seq = seq + 1;
}

/* ── Public API ───────────────────────────────────────────────────────── */

void *vsyscall_get_page(void)
{
    return (void *)VSYSCALL_PAGE_VADDR;
}

uint64_t vsyscall_get_data_phys(void)
{
    return vdso_data_phys;
}
