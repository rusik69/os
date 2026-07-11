/* smp.c — SMP boot and per-CPU management */

#include "smp.h"
#include "apic.h"
#include "vmm.h"
#include "pmm.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "gdt.h"
#include "export.h"
#include "timer.h"
#include "process.h"
#include "scheduler.h"
#include "signal.h"
#include "cpuhp.h"
#include "cpuidle.h"
#include "perf_events.h"
#include "aslr.h"

/* ── Per-CPU data ──────────────────────────────────────────────────── */
struct cpu_info cpu_info_array[SMP_MAX_CPUS] __attribute__((aligned(64)));
int smp_cpu_count = 1;  /* BSP is always CPU 0 */

/* CPU hotplug state table (defined in cpuhp.h header, storage here) */
enum cpuhp_state cpuhp_cpu_state[CPUHP_MAX_CPUS];

#include "errno.h"

/* ── GS.base accessor ──────────────────────────────────────────────── */
void smp_set_gs_base(struct cpu_info *info) {
    uint64_t base = (uint64_t)info;
    uint32_t lo = (uint32_t)(base & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    __asm__ volatile("wrmsr" :: "c"(0xC0000101), "a"(lo), "d"(hi)); /* MSR_GS_BASE */
}

/* ── BSP initialization ────────────────────────────────────────────── */
void __init smp_init_bsp(void) {
    /* Initialize per-CPU info for BSP (CPU 0) */
    struct cpu_info *bsp = &cpu_info_array[0];
    memset(bsp, 0, sizeof(*bsp));
    bsp->cpu_id = 0;
    bsp->scheduler_enabled = 0;

    bsp->apic_id = 0;  /* will be set after LAPIC init */

    /* Store initial current_process (NULL — scheduler will handle) */
    bsp->current_process = NULL;
    bsp->idle_process = NULL;

    /* Set GS.base to BSP per-CPU struct */
    smp_set_gs_base(bsp);

    /* Initialize CPU hotplug subsystem */
    cpuhp_init();

    kprintf("[OK] Per-CPU data initialized for CPU 0\n");
}

/* ── AP C entry point (called from trampoline) ─────────────────────── */
/* This function runs on each AP after it wakes up */
static void ap_entry_c(void) {
    /* We are running on the AP's stack, in 64-bit long mode.
     * The BSP filled cpu_info_array[cpu_id] and placed our stack top
     * and entry pointer at addresses 0x7300 / 0x7308. */

    /* Find our CPU slot */
    uint32_t apic_id = apic_get_id();
    int cpu_id = -1;

    /* Look up our slot by matching APIC ID */
    for (int i = 0; i < smp_cpu_count; i++) {
        if (cpu_info_array[i].apic_id == apic_id) {
            cpu_id = i;
            break;
        }
    }

    if (cpu_id < 0) {
        /* Unknown CPU — should not happen */
        __asm__ volatile("cli; hlt");
    }

    struct cpu_info *info = &cpu_info_array[cpu_id];

    /* Set GS.base to our per-CPU struct */
    smp_set_gs_base(info);

    /* Initialize local APIC on this CPU */
    apic_init_local();
    info->apic_id = apic_id;

    /* Initialize per-CPU TSS */
    memset(&info->tss, 0, sizeof(info->tss));
    info->tss.iopb_offset = sizeof(struct tss);

    /* Initialize scheduler state */
    info->scheduler_enabled = 1;
    info->idle_ticks = 0;
    info->current_process = NULL;
    info->idle_process = NULL;

    /* Initialize per-CPU cpuidle data */
    cpuidle_init_cpu();

    /* Initialize PEBS / Debug Store for this CPU (if PMU supports it) */
    pebs_init();

    /* Mark CPU as ready */
    info->ready_flag = 1;

    kprintf("[OK] AP #%lu (APIC ID %lu) online\n", (unsigned long)cpu_id, (unsigned long)apic_id);

    /* Enable interrupts and enter idle loop */
    __asm__ volatile("sti");

    /* Simple idle loop — the scheduler will pick processes */
    for (;;) {
        /* Try to find work through the global scheduler */
        if (info->scheduler_enabled) {
            schedule();
        }
        /* Enter idle state using cpuidle */
        cpuidle_idle();
    }
}

/* ── SMP boot ──────────────────────────────────────────────────────── */

/* Trampoline boundaries (from ap_trampoline.asm) */
extern uint8_t ap_trampoline_start[];
extern uint8_t ap_trampoline_end[];

/* AP C entry point wrapper (called from asm trampoline) */
static void ap_entry_c_wrapper(void) {
    ap_entry_c();
}

static int detect_cpus_from_madt(void) {
    /* Parse ACPI MADT to find local APIC entries.
     * We scan for "APIC" in RSDT entries (done in acpi_init extension). */

    /* Simple fallback: search memory for MADT signature */
    /* uint32_t *rsdt = NULL; */
    /* We stored the RSDT address from the earlier scan.
     * Re-scan low memory for RSDP */
    /* For now, use a simplified approach — scan for MADT */
    static int madt_scanned = 0;
    if (madt_scanned) return smp_cpu_count;

    /* Walk RSDT entries, look for "APIC" */
    /* Find RSDP first */
    uint64_t rsdp_addr = 0;
    for (uint64_t addr = 0x80000; addr < 0x9FFFF; addr += 16) {
        if (memcmp(PHYS_TO_VIRT(addr), "RSD PTR ", 8) == 0) {
            uint8_t sum = 0;
            for (int i = 0; i < 20; i++) sum += ((uint8_t *)PHYS_TO_VIRT(addr))[i];
            if (sum == 0) { rsdp_addr = addr; break; }
        }
    }
    if (!rsdp_addr) {
        for (uint64_t addr = 0xE0000; addr < 0xFFFFF; addr += 16) {
            if (memcmp(PHYS_TO_VIRT(addr), "RSD PTR ", 8) == 0) {
                uint8_t sum = 0;
                for (int i = 0; i < 20; i++) sum += ((uint8_t *)PHYS_TO_VIRT(addr))[i];
                if (sum == 0) { rsdp_addr = addr; break; }
            }
        }
    }
    if (!rsdp_addr) return 1; /* no ACPI */

    struct rsdp {
        char sig[8];
        uint8_t chk;
        char oem[6];
        uint8_t rev;
        uint32_t rsdt_addr;
    } __attribute__((packed));

    struct acpi_hdr {
        char sig[4];
        uint32_t len;
        uint8_t rev;
        uint8_t chk;
        char oem[6];
        char tbl[8];
        uint32_t oem_rev;
        uint32_t creator;
        uint32_t creator_rev;
    } __attribute__((packed));

    struct rsdp *rsdp = (struct rsdp *)PHYS_TO_VIRT(rsdp_addr);
    struct acpi_hdr *rsdt_hdr = (struct acpi_hdr *)PHYS_TO_VIRT((uint64_t)rsdp->rsdt_addr);

    if (memcmp(rsdt_hdr->sig, "RSDT", 4) != 0) return 1;

    uint32_t entry_count = (uint32_t)((rsdt_hdr->len - sizeof(struct acpi_hdr)) / 4);
    const uint8_t *entry_data = (const uint8_t *)rsdt_hdr + sizeof(struct acpi_hdr);

    struct acpi_hdr *madt = NULL;
    for (uint32_t i = 0; i < entry_count; i++) {
        uint32_t entry_phys32;
        __builtin_memcpy(&entry_phys32, entry_data + i * 4, sizeof(entry_phys32));
        struct acpi_hdr *hdr = (struct acpi_hdr *)PHYS_TO_VIRT((uint64_t)entry_phys32);
        if (memcmp(hdr->sig, "APIC", 4) == 0) {
            madt = hdr;
            break;
        }
    }

    if (!madt) return 1; /* no MADT */

    /* Parse MADT entries */
    uint8_t *madt_body = (uint8_t *)madt + sizeof(struct acpi_hdr);
    /* First 4 bytes: local APIC address */
    /* uint32_t *lapic_addr = (uint32_t *)madt_body; */
    uint8_t *entry_start = madt_body + 4;
    /* Next 4 bytes: flags */
    uint32_t flags;
    __builtin_memcpy(&flags, entry_start, sizeof(flags));
    (void)flags;
    entry_start += 4;

    uint8_t *end = (uint8_t *)madt + madt->len;
    uint8_t *ptr = entry_start;

    int local_apics = 0;

    while (ptr < end) {
        uint8_t type = ptr[0];
        uint8_t length = ptr[1];

        if (length < 2) break;

        if (type == 0) {
            /* Processor Local APIC */
            uint8_t acpi_id = ptr[2];
            uint8_t apic_id = ptr[3];
            uint32_t flags_entry;
            __builtin_memcpy(&flags_entry, ptr + 4, sizeof(flags_entry));

            if (flags_entry & 1) {  /* bit 0 = enabled */
                if (local_apics < SMP_MAX_CPUS) {
                    cpu_info_array[local_apics].apic_id = apic_id;
                    cpu_info_array[local_apics].cpu_id = local_apics;
                    (void)acpi_id;
                }
                local_apics++;
            }
        }

        ptr += length;
    }

    smp_cpu_count = local_apics;
    if (smp_cpu_count > SMP_MAX_CPUS) smp_cpu_count = SMP_MAX_CPUS;
    madt_scanned = 1;

    kprintf("[OK] MADT: %u CPU%c found\n", (unsigned int)local_apics,
            local_apics == 1 ? ' ' : 's');

    return smp_cpu_count;
}

int smp_boot_aps(void) {
    int ap_count = 0;

    /* Detect available CPUs from MADT */
    int total = detect_cpus_from_madt();

    /* Initialize I/O APIC (needed for interrupt routing even on UP) */
    ioapic_init();

    if (total <= 1) {
        kprintf("[--] SMP: 1 CPU detected (no APs to boot)\n");
        return 0;
    }

    /* Initialize I/O APIC — already done above */
    ioapic_init();

    /* Copy trampoline to low memory (physical page 0x7000) */
    size_t tramp_size = (size_t)((uintptr_t)ap_trampoline_end - (uintptr_t)ap_trampoline_start);
    if (tramp_size > 0x1000) {
        kprintf("[!!] SMP: trampoline too large (%lu bytes)\n", (unsigned long)tramp_size);
        return 0;
    }

    /* Map page 0x7000 temporarily to copy trampoline */
    vmm_map_page(0x7000, 0x7000, VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    memcpy((void *)0x7000, ap_trampoline_start, tramp_size);
    vmm_unmap_page(0x7000);

    /* Store BSP's PML4 address for APs at physical 0x7200 */
    uint64_t *bsp_pml4 = vmm_get_pml4();
    vmm_map_page(0x7200, 0x7200, VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    *(uint64_t *)0x7200 = (uint64_t)bsp_pml4;
    vmm_unmap_page(0x7200);

    /* Boot each AP */
    for (int i = 1; i < smp_cpu_count; i++) {
        struct cpu_info *info = &cpu_info_array[i];

        /* Allocate kernel stack for this AP */
        uint8_t *stack = (uint8_t *)pmm_alloc_frames(32); /* 32 pages = 128 KB */
        if (!stack) {
            kprintf("[!!] SMP: cannot allocate stack for AP %d\n", (int)i);
            continue;
        }
        /* KASLR: randomize the virtual address of each AP stack */
        uint64_t kaslr_shift = aslr_kernel_stack_offset() * PAGE_SIZE;
        /* Map the stack at a known virtual address with KASLR shift */
        uint64_t stack_virt = 0xFFFF8000FFF00000ULL - (unsigned long)i * 0x100000ULL - kaslr_shift;
        for (uint64_t off = 0; off < 128 * 1024; off += 4096) {
            vmm_map_page(stack_virt + off, (uint64_t)(stack + off),
                         VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
        }

        info->kernel_stack = stack_virt;
        info->stack_top = stack_virt + 128 * 1024;

        /* Initialize per-CPU scheduler state */
        for (int q = 0; q < SCHED_LEVELS; q++) {
            info->queue_head[q] = NULL;
            info->queue_tail[q] = NULL;
        }
        info->scheduler_enabled = 0;
        info->idle_ticks = 0;
        info->current_process = NULL;
        info->ready_flag = 0;

        /* Write AP stack top and entry function pointer to known pages */
        vmm_map_page(0x7300, 0x7300, VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
        *(uint64_t *)0x7300 = info->stack_top;     /* stack for AP */
        *(uint64_t *)0x7308 = (uint64_t)ap_entry_c_wrapper;  /* entry function */
        vmm_unmap_page(0x7300);

        /* Send INIT IPI */
        uint32_t ap_apic_id = info->apic_id;
        apic_send_init_ipi(ap_apic_id);

        /* Send first SIPI to vector 0x70 (page 0x7000 >> 12 = 0x70) */
        apic_send_startup_ipi(ap_apic_id, 0x70);

        /* Wait 300 µs and send second SIPI (MP spec) */
        for (volatile int d = 0; d < 300000; d++) __asm__ volatile("pause");
        apic_send_startup_ipi(ap_apic_id, 0x70);

        /* Wait for AP to come online (ready_flag) */
        for (volatile int timeout = 0; timeout < 50000000; timeout++) {
            if (info->ready_flag) break;
            __asm__ volatile("pause");
        }

        if (info->ready_flag) {
            ap_count++;
            cpuhp_cpu_state[i] = CPUHP_STATE_ONLINE;
            kprintf("[OK] SMP: AP #%lu (APIC ID %lu) booted successfully\n",
                    (unsigned long)i, (unsigned long)ap_apic_id);
        } else {
            kprintf("[!!] SMP: AP #%lu (APIC ID %lu) failed to boot (timeout)\n",
                    (unsigned long)i, (unsigned long)ap_apic_id);
        }
    }

    return ap_count;
}

int smp_get_cpu_count(void) {
    return smp_cpu_count;
}
EXPORT_SYMBOL(smp_get_cpu_count);

/* ── CPU hotplug ───────────────────────────────────────────────── */

/*
 * smp_cpu_disable() — Take a CPU offline gracefully.
 *
 * Migrates all runnable tasks away from @cpu_id and marks the CPU
 * as offline. The BSP (CPU 0) cannot be offlined.
 *
 * Returns 0 on success, or a negative CPUHP_ERR_* code on failure.
 *
 * Usage:  smp_cpu_disable(1);   // offline CPU 1
 */
int smp_cpu_disable(int cpu_id)
{
    int ret;

    if (cpu_id < 0 || cpu_id >= smp_cpu_count)
        return CPUHP_ERR_INVAL;

    if (cpu_id == 0) {
        kprintf("[smp] Cannot disable BSP (CPU 0)\n");
        return CPUHP_ERR_BSP;
    }

    kprintf("[smp] Disabling CPU %d...\n", cpu_id);

    /* Delegate to cpuhp subsystem which handles locking, task migration */
    ret = cpuhp_take_cpu_offline(cpu_id);

    if (ret == CPUHP_OK) {
        kprintf("[smp] CPU %d disabled (%d CPUs now online)\n",
                cpu_id, cpuhp_online_count());
    } else {
        kprintf("[smp] Failed to disable CPU %d: error %d\n", cpu_id, ret);
    }

    return ret;
}

/*
 * smp_cpu_enable() — Bring a CPU online.
 *
 * Transitions @cpu_id from OFFLINE back to ONLINE state so that the
 * scheduler can assign tasks to it.
 *
 * Returns 0 on success, or a negative CPUHP_ERR_* code on failure.
 *
 * Usage:  smp_cpu_enable(1);   // online CPU 1
 */
int smp_cpu_enable(int cpu_id)
{
    int ret;

    if (cpu_id < 0 || cpu_id >= smp_cpu_count)
        return CPUHP_ERR_INVAL;

    kprintf("[smp] Enabling CPU %d...\n", cpu_id);

    ret = cpuhp_bring_cpu(cpu_id);

    if (ret == CPUHP_OK) {
        /* Re-enable the scheduler on this CPU */
        cpu_info_array[cpu_id].scheduler_enabled = 1;

        kprintf("[smp] CPU %d enabled (%d CPUs now online)\n",
                cpu_id, cpuhp_online_count());
    } else {
        kprintf("[smp] Failed to enable CPU %d: error %d\n", cpu_id, ret);
    }

    return ret;
}

/* ── Stub: smp_boot_cpus ─────────────────────────────── */
static int smp_boot_cpus(void)
{
    kprintf("[smp] smp_boot_cpus: not yet implemented\n");
    return 0;
}
/* ── Stub: smp_send_reschedule ─────────────────────────────── */
static int smp_send_reschedule(int cpu)
{
    (void)cpu;
    kprintf("[smp] smp_send_reschedule: not yet implemented\n");
    return 0;
}
/* ── Stub: smp_call_function ─────────────────────────────── */
static int smp_call_function(void *func, void *info, int wait)
{
    (void)func;
    (void)info;
    (void)wait;
    kprintf("[smp] smp_call_function: not yet implemented\n");
    return 0;
}
/* ── smp_stop_cpus ─────────────────────────────── */
void smp_stop_cpus(void)
{
    int cpu_count = smp_get_cpu_count();

    if (cpu_count <= 1) {
        kprintf("[smp] smp_stop_cpus: only 1 CPU online, nothing to stop\n");
        return;
    }

    kprintf("[smp] Stopping %d other CPUs via PANIC_HALT IPI...\n",
            cpu_count - 1);

    /*
     * Send the panic-halt IPI to all other CPUs.  The handler
     * (ipi_panic_halt_handler) disables local interrupts on each
     * receiving CPU and enters an infinite HLT loop without
     * acknowledging the APIC (no EOI).  This is safe for kexec
     * because the APIC is masked and re-initialised by the new
     * kernel after the transition.
     */
    apic_send_ipi_all_except(IPI_VECTOR_PANIC_HALT);

    /* Small delay to let IPIs fire before the caller disables its APIC */
    for (volatile int d = 0; d < 10000; d++)
        __asm__ volatile("pause");

    kprintf("[smp] Stop IPIs sent\n");
    return;
}
