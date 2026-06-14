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

/* ── Topdown Metrics MSRs (Ice Lake+) ──────────────────────────────── */
#define IA32_PERF_METRICS           0x329  /* Topdown metric breakdown */
#define IA32_FIXED_CTR_CTRL         0x38D  /* Fixed counter control (FC0/FC1/FC2) */
#define IA32_FIXED_CTR2             0x30B  /* Fixed counter 2 value (TOPDOWN.SLOTS) */
#define FIXED_CTR2_CTRL_SHIFT       16     /* FC2 control in IA32_FIXED_CTR_CTRL bits 16-23 */
#define FIXED_CTR2_CTRL_EN          (1ULL << 16)  /* Enable FC2 */
#define FIXED_CTR2_CTRL_ANYTHREAD   (1ULL << 17)  /* Count on any thread */
#define FIXED_CTR2_CTRL_KERNEL      (1ULL << 18)  /* OS mode */
#define FIXED_CTR2_CTRL_USER        (1ULL << 19)  /* User mode */
#define FIXED_CTR2_CTRL_PMI         (1ULL << 20)  /* PMI on overflow */

/* Topdown metric slot fractions stored in U2.16 fixed-point format.
 * Each value represents a fraction of total pipeline slots (0.0 to ~4.0).
 * Sum of all four categories equals total pipeline slots (normally 1.0 per cycle). */
struct topdown_metrics {
    uint32_t frontend_bound;        /* Frontend stalls (I-cache, decode) × 2^16 */
    uint32_t bad_speculation;       /* Misprediction waste × 2^16 */
    uint32_t backend_bound;         /* Backend stalls (cache, exec) × 2^16 */
    uint32_t retiring;              /* Useful work done × 2^16 */
};

/* ── LBR (Last Branch Record) MSRs ──────────────────────────────────── */
#define IA32_DEBUGCTL             0x1D9  /* Debug control (LBR, BTS, TR bits) */
#define IA32_DEBUGCTL_LBR        (1ULL << 0)   /* Enable LBR */
#define IA32_DEBUGCTL_TR         (1ULL << 6)   /* Enable branch trace */
#define IA32_DEBUGCTL_BTS        (1ULL << 7)   /* Enable BTS */
#define IA32_DEBUGCTL_BTINT      (1ULL << 8)   /* BTS interrupt on precise */
#define IA32_DEBUGCTL_FREEZE     (1ULL << 11)  /* Freeze LBR on PMI */
#define IA32_DEBUGCTL_FREEZE_LBRs_ON_PMI (1ULL << 11)

/* LBR filtering — MSR_LBR_SELECT (if available, arch LBR >= LBRv3) */
#define MSR_LBR_SELECT            0x1C8
#define LBR_SELECT_CMP_EQ         (1ULL << 0)  /* Filter conditional branches */
#define LBR_SELECT_CALL           (1ULL << 1)  /* Filter near call (incl. call) */
#define LBR_SELECT_IND_CALL       (1ULL << 2)  /* Filter indirect near calls */
#define LBR_SSELECT_RET           (1ULL << 3)  /* Filter near return */
#define LBR_SELECT_IND_JMP        (1ULL << 4)  /* Filter indirect near jmps */
#define LBR_SELECT_REL_CALL       (1ULL << 5)  /* Filter relative near calls */
#define LBR_SELECT_IND_JMP_CALL   (1ULL << 6)  /* Filter indirect calls+jmps */
#define LBR_SELECT_CYCLE_SHIFT    16           /* Cycle count filter shift */
#define LBR_SELECT_CYCLE_MASK     0x3   /* bits 16-17: 0=off,1=eq,2=neq */

/* Architectural LBR (since Ice Lake): MSRs 0xC00-0xC3F */
#define MSR_ARCH_LBR_CTL          0xC00
#define ARCH_LBR_CTL_LBR_EN       (1ULL << 0)
#define ARCH_LBR_CTL_FILTER_SHIFT 1
#define ARCH_LBR_CTL_DEPTH_SHIFT  8
#define ARCH_LBR_CTL_MISPRED_EN   (1ULL << 32)
#define ARCH_LBR_CTL_CYC_EN       1ULL  /* cycle count enable */

#define MSR_ARCH_LBR_DEPTH        0xC01  /* configurable depth register */
#define MSR_ARCH_LBR_FROM_BASE    0xC10
#define MSR_ARCH_LBR_TO_BASE      0xC20
#define MSR_ARCH_LBR_INFO_BASE    0xC30  /* optional LBR_INFO */

/* Legacy LBR (Nehalem through Ice Lake): MSR_LBR_NHM_* */
#define MSR_LBR_NHM_FROM( idx)    (0x680 + (idx))  /* idx 0-15 */
#define MSR_LBR_NHM_TO(idx)       (0x6C0 + (idx))  /* idx 0-15 */

/* Maximum LBR depth across CPU generations */
#define LBR_MAX_DEPTH             32

/* A single LBR entry  */
struct lbr_entry {
    uint64_t from;      /* source address */
    uint64_t to;        /* target address */
    uint64_t info;      /* optional: cycle count, branch-type flags */
};

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


/* Check if the current process is allowed to access perf events.
 * Returns 0 if allowed, -EPERM if denied. */
int perf_paranoid_check(void);

/* Initialize the perf_event_paranoid sysctl */
void perf_paranoid_sysctl_init(void);

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

/* ── LBR (Last Branch Record) API ────────────────────────────────────── */

/* Detect LBR capability — checks CPUID or reads MSR_LBR_SELECT.
 * Returns the number of LBR entries supported (0 if unavailable). */
int lbr_depth(void);

/* Enable LBR recording on the current CPU.
 * @flags: bitmask of filtering options (LBR_SELECT_* constants, 0 = all branches).
 * If arch LBR is available, the configurable depth and filtering are set;
 * otherwise legacy LBR (Nehalem+) is enabled via IA32_DEBUGCTL. */
void lbr_enable(uint64_t flags);

/* Disable LBR recording on the current CPU.  Clears IA32_DEBUGCTL.LBR
 * and, for arch LBR, clears MSR_ARCH_LBR_CTL. */
void lbr_disable(void);

/* Read the current LBR stack into the caller's buffer.
 * @entries: output array of lbr_entry structs (must be at least LBR_MAX_DEPTH).
 * Returns the number of valid entries read (0 if LBR is disabled). */
int lbr_read(struct lbr_entry *entries);

/* Determine the LBR depth (number of MSR pairs) by probing CPUID leaf 0x1C
 * (architectural LBR) or falling back to a safe default (16 for legacy). */
int lbr_detect_depth(void);

/* ── Topdown Metrics API (Item 207) ───────────────────────────────────── */

/* Check if the CPU supports Topdown Metrics (CPUID leaf 0x0A, ECX bit 15).
 * Returns 1 if topdown is available, 0 otherwise. */
int topdown_available(void);

/* Configure fixed counter 2 to count TOPDOWN.SLOTS and enable the
 * IA32_PERF_METRICS MSR.  Must be called on each CPU after perf_init().
 * Returns 0 on success, -ENODEV if topdown is not supported. */
int topdown_enable(void);

/* Read the current Topdown Metrics from IA32_PERF_METRICS.
 * @metrics: output structure receiving the four slot fractions.
 * Returns 0 on success, -ENODEV if topdown is not enabled. */
int topdown_read(struct topdown_metrics *metrics);

/* Disable topdown counters on the current CPU. */
void topdown_disable(void);

#endif /* PERF_EVENTS_H */
