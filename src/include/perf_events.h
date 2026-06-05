#ifndef PERF_EVENTS_H
#define PERF_EVENTS_H

#include "types.h"

/* MSR definitions for performance counters */
#define IA32_PERF_GLOBAL_CTRL     0x38F
#define IA32_PERF_GLOBAL_STATUS   0x38E
#define IA32_PERF_GLOBAL_OVF_CTRL 0x390
#define IA32_PERFEVTSEL0          0x186
#define IA32_PMC0                 0xC1
#define IA32_PMC1                 0xC2

/* Fixed-function counters (architectural) */
#define IA32_FIXED_CTR0           0x309
#define IA32_FIXED_CTR0_CTRL      0x38D

/* PEBS (Precise Event-Based Sampling) MSRs */
#define IA32_DS_AREA              0x600  /* Debug Store area base address */
#define IA32_PEBS_ENABLE          0x3F1  /* PEBS enable per counter */

/* PERF_GLOBAL_CTRL bits */
#define GLOBAL_CTRL_PMC0          (1ULL << 0)
#define GLOBAL_CTRL_PMC1          (1ULL << 1)
#define GLOBAL_CTRL_PMC2          (1ULL << 2)
#define GLOBAL_CTRL_PMC3          (1ULL << 3)
#define GLOBAL_CTRL_FIXED0        (1ULL << 32)
#define GLOBAL_CTRL_FIXED1        (1ULL << 33)
#define GLOBAL_CTRL_FIXED2        (1ULL << 34)

/* PERF_GLOBAL_STATUS bits */
#define GLOBAL_STATUS_PMC0_OVF    (1ULL << 0)
#define GLOBAL_STATUS_PMC1_OVF    (1ULL << 1)
#define GLOBAL_STATUS_PMC2_OVF    (1ULL << 2)
#define GLOBAL_STATUS_PMC3_OVF    (1ULL << 3)
#define GLOBAL_STATUS_DS_BUFFER   (1ULL << 62)  /* DS buffer overflow */

/* IA32_PERFEVTSEL bits */
#define PERFEVTSEL_EVENT_MASK     0xFFULL
#define PERFEVTSEL_UMASK_SHIFT    8
#define PERFEVTSEL_USR            (1ULL << 16)
#define PERFEVTSEL_OS             (1ULL << 17)
#define PERFEVTSEL_ENABLE         (1ULL << 18)
#define PERFEVTSEL_INT            (1ULL << 20)
#define PERFEVTSEL_ANYTHREAD      (1ULL << 21)
#define PERFEVTSEL_EN             (1ULL << 22)
#define PERFEVTSEL_INV            (1ULL << 23)
#define PERFEVTSEL_CMASK_SHIFT    24

/* Software event counters */
struct perf_sw_counters {
    uint64_t context_switches;
    uint64_t page_faults;
    uint64_t cpu_cycles;
    uint64_t instructions;
};

/* ── Debug Store (DS) Area ────────────────────────────────────────────
 *
 * Per-CPU structure that holds BTS (Branch Trace Store) and PEBS buffer
 * pointers.  Must be naturally aligned and in non-cacheable memory.
 * Reference: Intel SDM Vol 3B Chapter 23.6. */

struct debug_store {
    uint64_t bts_buffer_base;          /* 0x00: base of BTS buffer */
    uint64_t bts_index;                /* 0x08: current write pointer */
    uint64_t bts_absolute_maximum;     /* 0x10: end of BTS buffer */
    uint64_t bts_interrupt_threshold;  /* 0x18: interrupt on BTS threshold */
    uint64_t pebs_buffer_base;         /* 0x20: base of PEBS buffer */
    uint64_t pebs_index;               /* 0x28: current write pointer */
    uint64_t pebs_absolute_maximum;    /* 0x30: end of PEBS buffer */
    uint64_t pebs_interrupt_threshold; /* 0x38: interrupt on PEBS threshold */
    uint64_t pebs_counter_reset[8];    /* 0x40: counter reset values */
} __attribute__((aligned(64)));

/* Architectural PEBS record (PEBSv3+ from Haswell onward).
 * The CPU writes this on each PEBS sample before generating a PMI
 * (or buffer-overflow interrupt).  Must be naturally aligned. */
struct pebs_record {
    uint64_t ip;               /* 0x00: instruction pointer of sampled insn */
    uint64_t ax;               /* 0x08:   RAX at sample time */
    uint64_t bx;               /* 0x10:   RBX */
    uint64_t cx;               /* 0x18:   RCX */
    uint64_t dx;               /* 0x20:   RDX */
    uint64_t si;               /* 0x28:   RSI */
    uint64_t di;               /* 0x30:   RDI */
    uint64_t bp;               /* 0x38:   RBP */
    uint64_t sp;               /* 0x40:   RSP */
    uint64_t flags;            /* 0x48:   RFLAGS */
    uint64_t r8;               /* 0x50:   R8  (PEBSv3+) */
    uint64_t r9;               /* 0x58:   R9 */
    uint64_t r10;              /* 0x60:   R10 */
    uint64_t r11;              /* 0x68:   R11 */
    uint64_t r12;              /* 0x70:   R12 */
    uint64_t r13;              /* 0x78:   R13 */
    uint64_t r14;              /* 0x80:   R14 */
    uint64_t r15;              /* 0x88:   R15 */
    /* ── Remaining fields vary by PEBS version; we soft-pad to 200 bytes ── */
    uint8_t  _pad[200 - 0x90];
} __attribute__((aligned(64)));

/* Size of a single PEBS record */
#define PEBS_RECORD_SIZE  (int)sizeof(struct pebs_record)

/* Default PEBS buffer size (4K = 20 records at ~200 bytes each) */
#define PEBS_BUFFER_SIZE  (4096)
#define PEBS_MAX_RECORDS  (PEBS_BUFFER_SIZE / PEBS_RECORD_SIZE)

/* Maximum number of PEBS-capable counters */
#define PEBS_MAX_COUNTERS  4

/* ── Per-CPU PEBS state ────────────────────────────────────────────── */

struct pebs_cpu_state {
    int initialized;                    /* DS area set up on this CPU */
    struct debug_store *ds_area;        /* virtual address of DS area */
    struct pebs_record *pebs_buffer;    /* circular PEBS record buffer */
    int pebs_counter;                   /* which counter has PEBS enabled (-1 = none) */
    uint64_t pebs_counter_reset;        /* counter reset value for auto-reload */
    int sample_count;                   /* total samples collected */
};

/* ── Public API ───────────────────────────────────────────────────── */


/* Read a hardware performance counter (PMC0-PMC3) */
uint64_t perf_read_pmc(int counter);

/* Enable/disable performance monitoring globally */
void perf_enable(void);
void perf_disable(void);

/* Software event accounting */
void perf_sw_context_switch(void);
void perf_sw_page_fault(void);

/* Read software event counters */
uint64_t perf_sw_read_context_switches(void);
uint64_t perf_sw_read_page_faults(void);

/* Initialize performance monitoring */
void perf_init(void);

/* ── PEBS (Precise Event-Based Sampling) API ─────────────────────────── */

/* Initialise PEBS / DS area for the current CPU.  Must be called on
 * each CPU after perf_init().  Allocates 4K DS area + 4K PEBS buffer
 * from kernel heap.  Returns 0 on success, -ENOMEM on allocation failure. */
int pebs_init(void);

/* Enable PEBS sampling on a specific counter.
 * @counter:      PMC index (0-3)
 * @event_sel:    event select value (same as IA32_PERFEVTSEL format)
 * @reset_val:    counter overflow value (counter reloaded to this after sample)
 * Notes:
 *   - The counter is configured with the given event_sel plus INT and
 *     EN bits forced on.
 *   - The DS area must already be initialised (pebs_init() called).
 *   - Returns 0 on success, negative errno on error. */
int pebs_enable_counter(int counter, uint64_t event_sel, uint64_t reset_val);

/* Disable PEBS sampling on the given counter. */
void pebs_disable_counter(int counter);

/* Read the current PEBS buffer and drain all records.
 * Writes records into @buf (up to @max_count records) and returns the
 * number of records actually read.  The kernel PEBS index is reset to
 * the buffer base, effectively draining the accumulated samples. */
int pebs_read_samples(struct pebs_record *buf, int max_count);

/* Return the number of samples collected so far (since boot or last reset). */
int pebs_total_samples(void);

/* Check if PEBS is available on this CPU (CPUID, DS bit check). */
int pebs_available(void);

#endif /* PERF_EVENTS_H */
