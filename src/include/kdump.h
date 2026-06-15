#ifndef KDUMP_H
#define KDUMP_H

#include "types.h"

/*
 * ── kdump: Kernel crash dump capture ─────────────────────────────────
 *
 * On panic(), the kernel saves complete register state, stack trace,
 * and critical memory regions to a pre-reserved physical memory region.
 * This dump survives a reboot (provided the region isn't overwritten by
 * the bootloader) and can be retrieved by a kdump-aware boot loader or
 * a post-mortem analysis tool.
 *
 * The reserved region is 1 MB at physical address 0x7FC00000, right
 * before the pstore region at 0x7FE00000.
 *
 * Layout of the reserved region:
 *   Offset  | Content
 *   --------+---------------------------------
 *   0x0000  | struct kdump_header
 *   0x0200  | struct kdump_reg_entry[28]
 *   0x0400  | uint64_t stack_frames[64]
 *   0x0600  | struct kdump_mem_desc[16]
 *   0x0A00  | Memory segment data (loop, stack, heap, etc.)
 *
 * Total reserved: 1 MB
 */

#define KDUMP_PHYS_BASE    0x7FC00000ULL
#define KDUMP_REGION_SIZE  (1024UL * 1024)   /* 1 MB */

#define KDUMP_MAGIC        0x4B444D50U       /* "KDMP" */
#define KDUMP_VERSION      1

#define KDUMP_MAX_REGS     28
#define KDUMP_MAX_FRAMES   64
#define KDUMP_MAX_SEGMENTS 16

/* ── Register identifiers (lower-case ASCII packed into uint64_t) ── */
#define KREG_RAX  0x7261780000000000ULL  /* "rax\0\0\0\0\0" */
#define KREG_RBX  0x7262780000000000ULL
#define KREG_RCX  0x7263780000000000ULL
#define KREG_RDX  0x7264780000000000ULL
#define KREG_RSI  0x7273690000000000ULL
#define KREG_RDI  0x7264690000000000ULL
#define KREG_RBP  0x7262700000000000ULL
#define KREG_RSP  0x7273700000000000ULL
#define KREG_R8   0x7238000000000000ULL
#define KREG_R9   0x7239000000000000ULL
#define KREG_R10  0x7231300000000000ULL
#define KREG_R11  0x7231310000000000ULL
#define KREG_R12  0x7231320000000000ULL
#define KREG_R13  0x7231330000000000ULL
#define KREG_R14  0x7231340000000000ULL
#define KREG_R15  0x7231350000000000ULL
#define KREG_RIP  0x7269700000000000ULL
#define KREG_RFL  0x72666c0000000000ULL
#define KREG_CS   0x6373000000000000ULL
#define KREG_DS   0x6473000000000000ULL
#define KREG_ES   0x6573000000000000ULL
#define KREG_FS   0x6673000000000000ULL
#define KREG_GS   0x6773000000000000ULL
#define KREG_SS   0x7373000000000000ULL
#define KREG_CR0  0x6372300000000000ULL
#define KREG_CR2  0x6372320000000000ULL
#define KREG_CR3  0x6372330000000000ULL
#define KREG_CR4  0x6372340000000000ULL

/* ── Memory segment flags ── */
#define KSEG_READ    (1U << 0)
#define KSEG_WRITE   (1U << 1)
#define KSEG_EXEC    (1U << 2)
#define KSEG_KERNEL  (1U << 3)
#define KSEG_USER    (1U << 4)

#ifndef __ASSEMBLER__

/* Header at the beginning of the kdump region */
struct kdump_header {
    uint32_t magic;                 /* KDUMP_MAGIC */
    uint32_t version;               /* KDUMP_VERSION */
    uint64_t timestamp_tsc;         /* TSC value at panic time */
    uint32_t cpu_id;                /* CPU that panicked */
    uint32_t cpu_count;             /* Total number of CPUs */
    uint32_t pid;                   /* Current process PID */
    uint32_t num_regs;              /* Number of reg entries saved */
    uint32_t num_frames;            /* Number of stack frames saved */
    uint32_t num_segments;          /* Number of memory segment descriptors */
    uint32_t total_size;            /* Total bytes written to region */
    char     panic_msg[128];        /* Panic message string */
    uint8_t  reserved[52];          /* Future expansion */
} __attribute__((packed));
_Static_assert(sizeof(struct kdump_header) == 224, "kdump_header size mismatch");

/* Register entry — one per CPU register */
struct kdump_reg_entry {
    uint64_t id;    /* Register identifier (KREG_*) */
    uint64_t value; /* Register value */
} __attribute__((packed));

/* Memory segment descriptor */
struct kdump_mem_desc {
    uint64_t start_vaddr;   /* Virtual address (or physical for low mem) */
    uint64_t size;          /* Size in bytes */
    uint64_t offset;        /* Byte offset in kdump region where data lives */
    uint32_t flags;         /* KSEG_* flags */
    uint32_t _pad;
} __attribute__((packed));

/*
 * ── Public API ───────────────────────────────────────────────────────
 */

/* Initialise the kdump region (reserve physical memory, map it). */
void kdump_init(void);

/*
 * Capture a kernel crash dump.
 *
 * Called from panic() with interrupts disabled.  Writes to the
 * pre-reserved memory region.  @rip is the instruction pointer at
 * the panic call site (use __builtin_return_address(0) or the fault
 * handler's stored RIP).
 */
void kdump_capture(const char *msg, uint64_t rip);

/*
 * Check whether a valid (previously-captured) crash dump exists in the
 * reserved region.  Returns 1 if a dump is present, 0 otherwise.
 */
int kdump_check(void);

/*
 * Return a pointer to the kdump header if a valid dump exists, or NULL.
 */
const struct kdump_header *kdump_get_header(void);

/*
 * Sysfs initialisation for /sys/kernel/kexec_load_disabled and
 * /sys/kernel/crash_kexec_post_notifiers.
 * Called from kernel_main() after sysfs_init().
 */
void kdump_sysfs_init(void);

/*
 * On panic, attempt to boot into a pre-loaded crash kernel to dump memory.
 * Returns 1 if crash kexec was triggered, 0 otherwise.
 * Called from panic() when crash_kernel_reserved is set.
 */
int kdump_crash_kexec_on_panic(void);

#endif /* __ASSEMBLER__ */
#endif /* KDUMP_H */
