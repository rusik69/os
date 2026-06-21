/*
 * Machine Check Exception (#MC) Handler — IA32 MCA Architecture
 *
 * Handles CPU-detected hardware errors (memory ECC failures, cache errors,
 * bus errors, thermal events) on x86-64.  When a machine check is signaled
 * via vector 18, we scan all MCA banks, decode the error, log it with
 * human-readable descriptions, and determine whether the system can safely
 * continue or must panic.
 *
 * MCA Architecture:
 *   - IA32_MCG_CAP[7:0]  = number of MCA banks
 *   - IA32_MCG_STATUS    = global status (RIPV, EIPV, MCIP)
 *   - IA32_MCi_STATUS    = per-bank error status (one per bank)
 *   - IA32_MCi_ADDR      = faulting address (if ADDRV set)
 *   - IA32_MCi_MISC      = additional error info   (if MISCV set)
 *
 * Error severity classification:
 *   - Corrected (no UC)       → log and clear, continue
 *   - Uncorrectable no action → kill current process if possible, continue
 *   - Uncorrectable fatal     → panic immediately
 *
 * Reference: Intel SDM Vol 3A Chapter 15; AMD APM Volume 2 Section 7.5.
 */

#include "mce.h"
#include "idt.h"
#include "cpu.h"
#include "printf.h"
#include "panic.h"
#include "process.h"
#include "smp.h"
#include "fault.h"
#include "kdump.h"
#include "string.h"

/* ── Error severity classification (matches mce.h enum) ───────────── */

#define MCE_MAX_BANKS 256

/* ── Table of known MCA error classes ─────────────────────────────── */

static const char *mca_error_class_str(uint8_t class)
{
    switch (class) {
        case 0x0: return "No error";
        case 0x1: return "Unclassified";
        case 0x2: return "Micro-architectural (uop)";
        case 0x3: return "External (bus/interconnect)";
        case 0x4: return "Internal (cache/TLB)";
        case 0x5: return "Internal (uncategorized)";
        case 0x6: return "Internal (generic)";
        case 0x7: return "Internal (memory)";
        case 0x8: return "Bus/interconnect (generic)";
        case 0x9: return "Bus/interconnect (memory)";
        case 0xA: return "Bus/interconnect (other)";
        case 0xB: return "Internal (firmware/hardware)";
        case 0xC: return "Internal (publisher)";
        case 0xD: return "Programmable error";
        case 0xE: return "Deferred error (AMD)";
        case 0xF: return "Intel-defined / custom";
        default:  return "Unknown";
    }
}

/* ── Severity assessment ──────────────────────────────────────────── */

static enum mce_severity mce_assess_severity(uint64_t status)
{
    int valid = !!(status & MC_STATUS_VAL);
    int uc    = !!(status & MC_STATUS_UC);
    int pcc   = !!(status & MC_STATUS_PCC);
    int ar    = !!(status & MC_STATUS_AR);

    if (!valid)
        return MCE_SEV_NO_ERROR;

    /* Corrected error: UC=0, log and continue */
    if (!uc) {
        return MCE_SEV_CORRECTED;
    }

    /* Uncorrectable error */
    if (pcc) {
        /* Processor context corrupted — cannot safely continue */
        return MCE_SEV_FATAL;
    }

    /* Uncorrectable but no context corruption.
     * If action required, we may need to kill the current process. */
    if (ar) {
        return MCE_SEV_UC;
    }

    /* Uncorrectable no action required (UCNA) — treat as UC */
    return MCE_SEV_UC;
}

/* ── Log a single bank error ──────────────────────────────────────── */

static void mce_log_bank(const struct mce_bank_info *info)
{
    int ovf  = !!(info->status & MC_STATUS_OVER);
    int uc   = !!(info->status & MC_STATUS_UC);
    int pcc  = !!(info->status & MC_STATUS_PCC);
    int ar   = !!(info->status & MC_STATUS_AR);
    int s    = !!(info->status & MC_STATUS_S);
    uint16_t err_code = (uint16_t)(info->status & MC_STATUS_ERRCODE_MASK);
    uint8_t err_class = (uint8_t)(err_code >> 12);

    kprintf("[MCE] Bank %d: STATUS=0x%016llx%s%s%s%s%s\n",
            info->bank_num,
            (unsigned long long)info->status,
            uc       ? " UC"              : "",
            pcc      ? " PCC"             : "",
            s        ? " SIGNALED"        : "",
            ar       ? " AR"              : "",
            ovf      ? " OVERFLOW"        : "");

    kprintf("[MCE]   Class=%s (0x%x) Code=0x%04x",
            mca_error_class_str(err_class), err_class, err_code);

    if (info->status & MC_STATUS_ADDRV)
        kprintf(" Addr=0x%016llx", (unsigned long long)info->addr);

    if (info->status & MC_STATUS_MISCV)
        kprintf(" Misc=0x%016llx", (unsigned long long)info->misc);

    kprintf("\n");

    if (info->severity == MCE_SEV_CORRECTED)
        kprintf("[MCE]   → Corrected (no action required)\n");
    else if (info->severity == MCE_SEV_UC)
        kprintf("[MCE]   → Uncorrectable%s (may be recoverable)\n",
                (info->status & MC_STATUS_AR) ? " action-required" : "");
    else if (info->severity == MCE_SEV_FATAL)
        kprintf("[MCE]   → FATAL (processor context corrupted)\n");
}

/* ── Main #MC handler (called from exception entry) ───────────────── */

void mce_handler(struct interrupt_frame *frame)
{
    uint64_t mcg_cap = read_msr(MSR_IA32_MCG_CAP);
    int num_banks = (int)(mcg_cap & MCG_CAP_COUNT_MASK);
    if (num_banks <= 0 || num_banks > MCE_MAX_BANKS)
        num_banks = MCE_MAX_BANKS;

    /* Step 1: Log the global status */
    uint64_t mcg_status = read_msr(MSR_IA32_MCG_STATUS);
    int ripv = !!(mcg_status & MCG_STATUS_RIPV);
    int eipv = !!(mcg_status & MCG_STATUS_EIPV);
    int mcip = !!(mcg_status & MCG_STATUS_MCIP);

    kprintf("\n*** MACHINE CHECK EXCEPTION (#MC) ***\n");
    kprintf("MCG_STATUS=0x%016llx (RIPV=%d EIPV=%d MCIP=%d)\n",
            (unsigned long long)mcg_status, ripv, eipv, mcip);

    /* Step 2: Log exception context */
    kprintf("RIP=0x%lx  RSP=0x%lx  RBP=0x%lx\n",
            (unsigned long)frame->rip, (unsigned long)frame->rsp,
            (unsigned long)frame->rbp);
    kprintf("RAX=0x%lx  RBX=0x%lx  RCX=0x%lx  RDX=0x%lx\n",
            (unsigned long)frame->rax, (unsigned long)frame->rbx,
            (unsigned long)frame->rcx, (unsigned long)frame->rdx);
    kprintf("RSI=0x%lx  RDI=0x%lx  R8=0x%lx   R9=0x%lx\n",
            (unsigned long)frame->rsi, (unsigned long)frame->rdi,
            (unsigned long)frame->r8,  (unsigned long)frame->r9);
    kprintf("R10=0x%lx  R11=0x%lx  R12=0x%lx  R13=0x%lx\n",
            (unsigned long)frame->r10, (unsigned long)frame->r11,
            (unsigned long)frame->r12, (unsigned long)frame->r13);
    kprintf("R14=0x%lx  R15=0x%lx\n",
            (unsigned long)frame->r14, (unsigned long)frame->r15);
    kprintf("CS=0x%lx  SS=0x%lx  RFLAGS=0x%lx\n",
            (unsigned long)frame->cs, (unsigned long)frame->ss,
            (unsigned long)frame->rflags);

    /* Step 3: Scan all banks, log and classify */
    int found_banks = 0;
    enum mce_severity worst_severity = MCE_SEV_NO_ERROR;

    for (int i = 0; i < num_banks; i++) {
        uint64_t status = read_msr((uint32_t)(MSR_IA32_MC0_STATUS + 4ULL * i));

        if (!(status & MC_STATUS_VAL))
            continue;

        struct mce_bank_info info;
        memset(&info, 0, sizeof(info));
        info.bank_num = (uint32_t)i;
        info.status   = status;
        info.mcg_status = mcg_status;
        info.severity = mce_assess_severity(status);

        if (status & MC_STATUS_ADDRV)
            info.addr = read_msr((uint32_t)(MSR_IA32_MC0_ADDR + 4ULL * i));
        if (status & MC_STATUS_MISCV)
            info.misc = read_msr((uint32_t)(MSR_IA32_MC0_MISC + 4ULL * i));

        mce_log_bank(&info);
        found_banks++;

        if ((int)info.severity > (int)worst_severity)
            worst_severity = info.severity;

        /* Clear this bank's status — must write 0 to each bank's STATUS
         * to allow the CPU to log future errors.  The write-1-to-clear
         * pattern only applies to certain older CPUs; writing 0 is safe
         * across all implementations per Intel SDM. */
        write_msr((uint32_t)(MSR_IA32_MC0_STATUS + 4ULL * i), 0ULL);
    }

    if (!found_banks) {
        kprintf("[MCE] No valid error banks found (spurious #MC?)\n");
    } else {
        kprintf("[MCE] %d bank(s) with errors, worst severity=%d\n",
                found_banks, (int)worst_severity);
    }

    /* Step 4: Log final MCG_STATUS */
    mcg_status = read_msr(MSR_IA32_MCG_STATUS);
    kprintf("[MCE] MCG_STATUS after clear: 0x%016llx\n",
            (unsigned long long)mcg_status);

    /* Step 5: Take action based on worst severity */
    switch (worst_severity) {
    case MCE_SEV_NO_ERROR:
    case MCE_SEV_CORRECTED:
        kprintf("[MCE] Corrected machine check — continuing\n");
        /* Clear MCIP to indicate MCE processing is complete */
        if (mcg_status & MCG_STATUS_MCIP) {
            write_msr(MSR_IA32_MCG_STATUS, mcg_status & ~MCG_STATUS_MCIP);
        }
        return;

    case MCE_SEV_UC:
        /*
         * Uncorrectable — if RIPV is set we can try to kill the current
         * process and continue.  If RIPV is not set, the point of
         * execution is lost and we must panic.
         */
        if (ripv) {
            struct process *proc = process_get_current();
            kprintf("[MCE] Uncorrectable — killing current process"
                    "%s%s and continuing\n",
                    proc && proc->name ? " " : "",
                    proc && proc->name ? proc->name : "");
            if (mcg_status & MCG_STATUS_MCIP) {
                write_msr(MSR_IA32_MCG_STATUS, mcg_status & ~MCG_STATUS_MCIP);
            }
            if (proc) {
                process_exit_code(7); /* SIGBUS — process lost data */
            }
            return;
        }
        /* RIPV not set — fall through to fatal */
        kprintf("[MCE] RIPV not set — cannot safely recover\n");
        if (mcg_status & MCG_STATUS_MCIP) {
            write_msr(MSR_IA32_MCG_STATUS, mcg_status & ~MCG_STATUS_MCIP);
        }
        panic("MACHINE CHECK (#MC) — Uncorrectable at RIP=0x%lx",
              (unsigned long)frame->rip);
        break;

    case MCE_SEV_FATAL:
    case MCE_SEV_DEFERRED:
    default:
        kprintf("[MCE] FATAL — processor context corrupted\n");
        /* Capture state to kdump before panicking */
        {
            char msg[96];
            int n = snprintf(msg, sizeof(msg),
                "MCE FATAL at RIP=0x%lx banks=%d cpu=%u",
                (unsigned long)frame->rip, found_banks,
                smp_get_cpu_id());
            (void)n;
            msg[sizeof(msg) - 1] = '\0';
            kdump_capture(msg, frame->rip);
        }
        if (mcg_status & MCG_STATUS_MCIP) {
            write_msr(MSR_IA32_MCG_STATUS, mcg_status & ~MCG_STATUS_MCIP);
        }
        panic("MACHINE CHECK (#MC) — Fatal at RIP=0x%lx",
              (unsigned long)frame->rip);
        break;
    }

    /* Unreachable */
    for (;;) {}
}

/* ── Initialisation ───────────────────────────────────────────────── */

void mce_init(void)
{
    uint64_t mcg_cap = read_msr(MSR_IA32_MCG_CAP);
    int mcg_ctl_present = !!(mcg_cap & MCG_CAP_CTL_P);
    int num_banks = (int)(mcg_cap & MCG_CAP_COUNT_MASK);

    if (num_banks <= 0) {
        kprintf("[MCE] No MCA banks reported — machine check support disabled\n");
        return;
    }

    if (num_banks > MCE_MAX_BANKS)
        num_banks = MCE_MAX_BANKS;

    /* Enable global machine check control if available */
    if (mcg_ctl_present) {
        write_msr(MSR_IA32_MCG_CTL, ~0ULL);
    }

    /* Enable all banks: set IA32_MCi_CTL to ~0ULL to enable all error types */
    for (int i = 0; i < num_banks; i++) {
        write_msr((uint32_t)(MSR_IA32_MC0_CTL + 4ULL * i), ~0ULL);
    }

    kprintf("[MCE] Enabled machine check on CPU %u (%d banks%s)\n",
            smp_get_cpu_id(), num_banks,
            mcg_ctl_present ? ", MCG_CTL present" : "");
}

/* ── Diagnostic dump ──────────────────────────────────────────────── */

void mce_dump_banks(void)
{
    uint64_t mcg_cap = read_msr(MSR_IA32_MCG_CAP);
    int num_banks = (int)(mcg_cap & MCG_CAP_COUNT_MASK);
    int mcg_ctl_present = !!(mcg_cap & MCG_CAP_CTL_P);

    if (num_banks > MCE_MAX_BANKS)
        num_banks = MCE_MAX_BANKS;

    kprintf("[MCE] MCG_CAP=0x%llx banks=%d ctl_p=%d ext_p=%d cmci_p=%d lmce_p=%d\n",
            (unsigned long long)mcg_cap, num_banks,
            mcg_ctl_present,
            !!(mcg_cap & MCG_CAP_EXT_P),
            !!(mcg_cap & MCG_CAP_CMCI_P),
            !!(mcg_cap & MCG_CAP_LMCE_P));

    uint64_t mcg_status = read_msr(MSR_IA32_MCG_STATUS);
    kprintf("[MCE] MCG_STATUS=0x%llx (RIPV=%d EIPV=%d MCIP=%d)\n",
            (unsigned long long)mcg_status,
            !!(mcg_status & MCG_STATUS_RIPV),
            !!(mcg_status & MCG_STATUS_EIPV),
            !!(mcg_status & MCG_STATUS_MCIP));

    if (mcg_ctl_present) {
        uint64_t mcg_ctl = read_msr(MSR_IA32_MCG_CTL);
        kprintf("[MCE] MCG_CTL=0x%llx%s\n",
                (unsigned long long)mcg_ctl,
                mcg_ctl == ~0ULL ? " (all enabled)" : "");
    }

    for (int i = 0; i < num_banks; i++) {
        uint64_t ctl   = read_msr((uint32_t)(MSR_IA32_MC0_CTL + 4ULL * i));
        uint64_t status = read_msr((uint32_t)(MSR_IA32_MC0_STATUS + 4ULL * i));
        uint64_t addr  = read_msr((uint32_t)(MSR_IA32_MC0_ADDR + 4ULL * i));
        uint64_t misc  = read_msr((uint32_t)(MSR_IA32_MC0_MISC + 4ULL * i));

        if (status & MC_STATUS_VAL) {
            struct mce_bank_info info;
            memset(&info, 0, sizeof(info));
            info.bank_num = (uint32_t)i;
            info.status   = status;
            info.addr     = addr;
            info.misc     = misc;
            info.mcg_status = mcg_status;
            info.severity = mce_assess_severity(status);
            mce_log_bank(&info);
        }

        kprintf("[MCE]   Bank %d: CTL=0x%016llx STATUS=0x%016llx%s\n",
                i,
                (unsigned long long)ctl,
                (unsigned long long)status,
                (status & MC_STATUS_VAL) ? " [VALID]" : "");
    }
}

/* ── Stub: mce_register ────────────────────────────────────────────── */
int mce_register(void *handler)
{
    (void)handler;
    kprintf("[MCE] mce_register: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: mce_unregister ──────────────────────────────────────────── */
int mce_unregister(void *handler)
{
    (void)handler;
    kprintf("[MCE] mce_unregister: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: mce_log ─────────────────────────────────────────────────── */
int mce_log(void *mce)
{
    (void)mce;
    kprintf("[MCE] mce_log: not yet implemented\n");
    return -ENOSYS;
}
