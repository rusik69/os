/*
 * Machine Check Exception (#MC) handler.
 *
 * The #MC exception (vector 18) fires when the CPU detects a hardware
 * error (memory ECC, cache parity, bus errors, etc.).  This handler
 * scans all available error-reporting banks, classifies the severity,
 * and either logs a corrected error and returns or halts on a fatal
 * uncorrectable error.
 *
 * Reference: Intel SDM Vol. 3 15.3 "Machine-Check Architecture",
 *            AMD APM Vol. 2 13.1 "Machine Check Architecture".
 */

#include "mce.h"
#include "cpu.h"
#include "printf.h"
#include "fault.h"
#include "idt.h"

/* ── Internal helpers ────────────────────────────────────────────── */

/*
 * Read the number of MCE banks from IA32_MCG_CAP.
 * If the CPU doesn't support MCE (rare), return 0.
 */
static inline uint32_t mce_bank_count(void)
{
    uint64_t cap = read_msr(MSR_IA32_MCG_CAP);
    return (uint32_t)(cap & MCG_CAP_COUNT_MASK);
}

/*
 * Read the global machine-check status.
 */
static inline uint64_t mce_read_mcg_status(void)
{
    return read_msr(MSR_IA32_MCG_STATUS);
}

/*
 * Check whether a bank's status register contains a valid error.
 */
static inline int mce_bank_is_valid(uint64_t status)
{
    return (status & MC_STATUS_VAL) != 0;
}

/*
 * Classify a machine-check error by severity.
 */
static enum mce_severity mce_classify(uint64_t status)
{
    if (!mce_bank_is_valid(status))
        return MCE_SEV_NO_ERROR;

    /* If PCC is set, the processor context is corrupted — always fatal. */
    if (status & MC_STATUS_PCC)
        return MCE_SEV_FATAL;

    /* Uncorrectable errors with valid restart IP are "UC" (maybe recover). */
    if (status & MC_STATUS_UC)
        return MCE_SEV_UC;

    /* Corrected (recoverable) error — hardware fixed it. */
    return MCE_SEV_CORRECTED;
}

/*
 * Decode the MCA error code into a human-readable string.
 * This covers common x86 MCA error codes (Intel SDM Table 15-8).
 */
static const char *mce_decode_error_code(uint16_t code)
{
    uint8_t comp = (code >> 4) & 0x0F;   /* component */
    uint8_t type = code & 0x0F;           /* transaction type */
    (void)type;

    switch (comp) {
        case 0x0: return "Generic (unspecified)";
        case 0x1: return "Processor core / internal";
        case 0x2: return "Memory controller";
        case 0x3: return "Cache hierarchy (L1/L2/L3)";
        case 0x4: return "Bus / interconnect";
        case 0x5: return "I/O / integrated device";
        case 0x6: return "System / platform firmware";
        case 0x7: return "Processor bus / external";
        case 0x8: return "Cache TLB";
        case 0x9: return "Memory controller channel";
        case 0xA: return "Coprocessor / external device";
        case 0xB: return "PCIe / DMA";
        case 0xC: return "Memory controller (extended)";
        default:  return "Unknown component";
    }
}

/*
 * Dump a single bank's error information.
 */
static void mce_dump_bank(struct mce_bank_info *info)
{
    uint16_t errcode = (uint16_t)(info->status & MC_STATUS_ERRCODE_MASK);

    kprintf("  MCE[%u]: status=0x%llx", info->bank_num, info->status);

    if (info->severity == MCE_SEV_FATAL)
        kprintf(" [FATAL]");
    else if (info->severity == MCE_SEV_UC)
        kprintf(" [UNCORRECTED]");
    else if (info->severity == MCE_SEV_CORRECTED)
        kprintf(" [CORRECTED]");

    kprintf("\n");

    if (info->status & MC_STATUS_OVER)
        kprintf("    OVERFLOW: previous error lost\n");
    if (info->status & MC_STATUS_PCC)
        kprintf("    PCC: processor context corrupted\n");
    if (info->status & MC_STATUS_UC)
        kprintf("    Uncorrectable error\n");
    if (info->status & MC_STATUS_S)
        kprintf("    Signaled\n");

    kprintf("    Error code: 0x%04x (%s)\n",
            errcode, mce_decode_error_code(errcode));
    kprintf("    Model-specific: 0x%04x\n",
            (uint16_t)((info->status & MC_STATUS_MODCODE_MASK) >> MC_STATUS_MODCODE_SHIFT));

    if (info->status & MC_STATUS_ADDRV)
        kprintf("    Address: 0x%llx\n", info->addr);
    if (info->status & MC_STATUS_MISCV)
        kprintf("    Misc: 0x%llx\n", info->misc);
}

/*
 * Clear the MCE status for a given bank by writing back the same bits
 * (with the VAL bit set — writing 1 to VAL clears it).
 */
static inline void mce_clear_bank(uint32_t bank)
{
    uint64_t status = read_msr(MCE_BANK_STATUS(MSR_IA32_MC0_STATUS, bank));
    if (status & MC_STATUS_VAL) {
        /* Write the value back to clear (write-1-to-clear semantics). */
        write_msr(MCE_BANK_STATUS(MSR_IA32_MC0_STATUS, bank), status);
    }
}

/*
 * Clear the global MCG_STATUS.MCIP bit so the CPU can take future MCEs.
 */
static inline void mce_clear_mcg_status(void)
{
    uint64_t mcg = read_msr(MSR_IA32_MCG_STATUS);
    mcg &= ~MCG_STATUS_MCIP;
    write_msr(MSR_IA32_MCG_STATUS, mcg);
}

/* ── Public API ──────────────────────────────────────────────────── */

/*
 * Scan all MCE banks and log any errors found.
 * Returns the highest severity encountered.
 */
static enum mce_severity mce_scan_banks(struct mce_bank_info *fatal_out)
{
    uint32_t nbanks = mce_bank_count();
    enum mce_severity highest = MCE_SEV_NO_ERROR;

    if (nbanks == 0) {
        kprintf("  MCE: No error-reporting banks available\n");
        return MCE_SEV_NO_ERROR;
    }

    kprintf("  MCE: Scanning %u bank(s)...\n", nbanks);

    for (uint32_t i = 0; i < nbanks; i++) {
        uint64_t status = read_msr(MCE_BANK_STATUS(MSR_IA32_MC0_STATUS, i));
        if (!mce_bank_is_valid(status))
            continue;

        uint64_t addr  = (status & MC_STATUS_ADDRV)
                            ? read_msr(MCE_BANK_ADDR(MSR_IA32_MC0_ADDR, i))
                            : 0;
        uint64_t misc  = (status & MC_STATUS_MISCV)
                            ? read_msr(MCE_BANK_MISC(MSR_IA32_MC0_MISC, i))
                            : 0;

        struct mce_bank_info info;
        info.bank_num   = i;
        info.status     = status;
        info.addr       = addr;
        info.misc       = misc;
        info.mcg_status = 0;
        info.severity   = mce_classify(status);

        if (info.severity > highest) {
            highest = info.severity;
            if (fatal_out && info.severity >= MCE_SEV_UC)
                *fatal_out = info;
        }

        mce_dump_bank(&info);
        mce_clear_bank(i);
    }

    return highest;
}

/*
 * #MC handler (vector 18).
 *
 * Runs on the dedicated IST3 stack (set up in ist_init()).  Reads the
 * global MCG status, scans all banks, logs errors, and either returns
 * (corrected error) or halts (fatal/uncorrectable).
 */
void mce_handler(struct interrupt_frame *frame)
{
    uint64_t mcg_status = mce_read_mcg_status();

    kprintf("\n*** MACHINE CHECK EXCEPTION (#MC) ***\n");
    kprintf("MCG_STATUS: 0x%llx%s%s%s\n", mcg_status,
            (mcg_status & MCG_STATUS_RIPV) ? " RIPV" : "",
            (mcg_status & MCG_STATUS_EIPV) ? " EIPV" : "",
            (mcg_status & MCG_STATUS_MCIP) ? " MCIP" : "");

    /* Show the interrupted context. */
    kprintf("RIP: 0x%llx  RSP: 0x%llx  RBP: 0x%llx\n",
            (unsigned long long)frame->rip,
            (unsigned long long)frame->rsp,
            (unsigned long long)frame->rbp);
    kprintf("CS: 0x%llx  SS: 0x%llx  RFLAGS: 0x%llx\n",
            (unsigned long long)frame->cs,
            (unsigned long long)frame->ss,
            (unsigned long long)frame->rflags);

    /* Scan banks to find the error(s). */
    struct mce_bank_info fatal_info;
    enum mce_severity sev = mce_scan_banks(&fatal_info);

    /* Now clear the global MCIP so future MCEs can be signalled. */
    mce_clear_mcg_status();

    switch (sev) {
    case MCE_SEV_NO_ERROR:
        /* Spurious MCE — no banks indicate a valid error. */
        kprintf("MCE: No valid error found in any bank (spurious?)\n");
        return;

    case MCE_SEV_CORRECTED:
        kprintf("MCE: Corrected (recoverable) error — continuing.\n");
        return;

    case MCE_SEV_DEFERRED:
    case MCE_SEV_UC:
        kprintf("MCE: Uncorrected error detected.\n");
        if (mcg_status & MCG_STATUS_RIPV) {
            /* The CPU claims the instruction pointer is valid; we might
             * be able to recover if the affected data can be discarded.
             * For now, log and halt — production kernels can retry. */
            kprintf("MCE: RIP is valid, but no recovery handler available.\n");
        }
        /* fall through */

    case MCE_SEV_FATAL:
        kprintf("*** FATAL MACHINE CHECK — HALTING ***\n");
        arch_print_backtrace();
        kprintf("*** SYSTEM HALTED (machine check, cannot recover) ***\n");
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
        __builtin_unreachable();
    }
}

/*
 * Initialize MCE support.
 *
 * Steps:
 *   1. Enable MCE globally (IA32_MCG_CTL if present, CR4.MCE).
 *   2. Enable error reporting on all banks.
 *   3. Register the #MC handler (vector 18).
 *
 * Must be called after PMM + IST init (ist_init() sets up IST3).
 */
void mce_init(void)
{
    uint32_t nbanks = mce_bank_count();

    kprintf("[MCE] %u error-reporting bank(s) detected\n", nbanks);

    if (nbanks == 0) {
        /* CPU doesn't support MCE or has no banks — nothing to do. */
        return;
    }

    /* Ensure CR4.MCE is set (machine-check exceptions enabled). */
    uint64_t cr4 = read_cr4();
    if (!(cr4 & (1ULL << 6))) {
        write_cr4(cr4 | (1ULL << 6));
        kprintf("[MCE] CR4.MCE enabled\n");
    }

    /* Enable all banks: write all-ones to each MCi_CTL MSR. */
    for (uint32_t i = 0; i < nbanks; i++) {
        /* Some banks may be read-only; skip if writing fails silently. */
        write_msr(MCE_BANK_CTL(MSR_IA32_MC0_CTL, i), ~0ULL);

        /* Clear any stale status from boot. */
        uint64_t st = read_msr(MCE_BANK_STATUS(MSR_IA32_MC0_STATUS, i));
        if (st & MC_STATUS_VAL) {
            kprintf("[MCE] Bank %u had stale error status from boot (0x%llx)\n",
                    i, (unsigned long long)st);
            mce_clear_bank(i);
        }

        /* Enable corrected MCE interrupt (CMCI) via CTL2 if available. */
        uint64_t ctl2 = read_msr(MCE_BANK_CTL2(i));
        if (ctl2 != 0) {
            /* Set error threshold and enable CMCI. */
            ctl2 |= MC_CTL2_CMCI_EN | MC_CTL2_CLR_CTR;
            write_msr(MCE_BANK_CTL2(i), ctl2);
        }
    }

    /* Register the #MC handler (vector 18). */
    idt_register_handler(18, mce_handler);

    kprintf("[OK] Machine Check Exception handler registered (IST3)\n");
}
