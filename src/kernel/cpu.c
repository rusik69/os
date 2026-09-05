/*
 * cpu.c — CPU security features + hotplug management
 *
 * ── Section 1: CPU security initialization ────────────────────────────
 *   Enables SMEP, SMAP, NXE, UMIP for kernel hardening.
 *
 * ── Section 2: CPU hotplug ────────────────────────────────────────────
 *   Real implementation for CPU online/offline transitions
 *   with proper task migration, locking, and state tracking.
 *   The design mirrors the Linux cpuhp subsystem at a high level:
 *     - Per-CPU state enum tracks aliveness (DEAD / OFFLINE / ONLINE)
 *     - A global spinlock serialises state transitions
 *     - Taking a CPU offline migrates all runnable tasks to other CPUs
 *     - The BSP (CPU 0) cannot be offlined
 */

#include "cpu.h"

#include "apic.h"
#include "cpuhp.h"
#include "irq_work.h"
#include "pmm.h"
#include "printf.h"
#include "process.h"
#include "scheduler.h"
#include "slab.h"
#include "smp.h"
#include "string.h"
#include "timer.h"

/* ═══════════════════════════════════════════════════════════════════════
 * Section 1 — CPU Security Features
 * ═══════════════════════════════════════════════════════════════════════ */

/* Set by cpu_security_init() based on CPUID (SMAP support). */
int smap_supported = 0;

/* Enable CPU security features: SMEP, SMAP, NXE, UMIP.
 *
 * SMEP (CR4 bit 20): Prevents ring-0 execution of user-space pages.
 *   Without this, a kernel bug that jumps to a user-controlled address
 *   would execute in ring 0, bypassing all isolation.
 *
 * SMAP (CR4 bit 21): Prevents ring-0 access to user-space data.
 *   Kernel must use stac/clac instructions to temporarily enable
 *   user data access. Catches bugs where syscall handlers forget
 *   to validate user pointers.
 *
 * NXE  (EFER bit 11): Enables the No-Execute page table bit (bit 63).
 *   Allows marking data pages as non-executable. User stack and
 *   data pages should be marked NX to prevent code injection.
 *
 * UMIP (CR4 bit 11): Prevents user-mode execution of SGDT, SIDT,
 *   SLDT, SMSW, STR instructions. These leak kernel addresses.
 *
 * Returns 0 on success, -1 if any feature is unavailable. */
int cpu_security_init(void) {
    uint64_t cr4 = read_cr4();
    uint64_t efer = read_msr(0xC0000080); /* IA32_EFER */
    int rax, rbx, rcx, rdx;

    /* Check CPUID leaf 1 for SMEP (ECX bit 7) */
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx) : "a"(1));

    /* Check CPUID leaf 7, subleaf 0 for SMAP (EBX bit 20) and UMIP (ECX bit 2).
     * Leaf 1 ECX bits 20/2 are SSE4.2/DTES64, NOT SMAP/UMIP — probing the
     * wrong leaf would set CR4.SMAP/CR4.UMIP on CPUs lacking the feature,
     * causing a #GP on the CR4 write (boot crash on pre-Broadwell Intel). */
    int l7_ebx, l7_ecx;
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(l7_ebx), "=c"(l7_ecx), "=d"(rdx) : "a"(7), "c"(0));

    /* Enable SMEP if supported (leaf 1 ECX bit 7) */
    if (rcx & (1U << 7)) {
        cr4 |= CR4_SMEP;
        kprintf("[CPU] SMEP enabled\n");
    } else {
        kprintf("[CPU] SMEP not supported\n");
    }

    /* Enable SMAP if supported (leaf 7 EBX bit 20) */
    if (l7_ebx & (1U << 20)) {
        cr4 |= CR4_SMAP;
        smap_supported = 1;
        kprintf("[CPU] SMAP enabled\n");
    } else {
        smap_supported = 0;
        kprintf("[CPU] SMAP not supported\n");
    }

    /* Enable UMIP if supported (leaf 7 ECX bit 2) */
    if (l7_ecx & (1U << 2)) {
        cr4 |= CR4_UMIP;
        kprintf("[CPU] UMIP enabled\n");
    } else {
        kprintf("[CPU] UMIP not supported\n");
    }

    /* Enable OSFXSR (CR4 bit 9) — required for SSE/FXSAVE/FXRSTOR in user mode.
     * Without this bit, SSE instructions cause #UD in ring 3. */
    cr4 |= (1ULL << 9);
    kprintf("[CPU] OSFXSR enabled (SSE support for userspace)\n");

    /* Enable OSXSAVE (CR4 bit 18) — required for XGETBV/XSETBV/XSAVE/XRSTOR.
     * CPUID.1:ECX bit 27 (OSXSAVE) is DYNAMIC — it only reads 1 AFTER
     * CR4.OSXSAVE is set.  Instead, check bit 26 (XSAVE hardware support)
     * to know if the CPU can do XSAVE at all. */
    if (rcx & (1U << 26)) { /* CPUID.1:ECX bit 26 = XSAVE hardware support */
        cr4 |= (1ULL << 18);
        kprintf("[CPU] OSXSAVE enabled (XSAVE/XRSTOR + XCR0 support)\n");
    }

    /* Write updated CR4 */
    write_cr4(cr4);

    /* Configure XCR0 if OSXSAVE is now enabled — controls which FPU/SSE/AVX
     * state components are saved on context switch. */
    if (cr4 & (1ULL << 18)) {
        /* Build XCR0 from CPUID feature bits we already know.
         * Always enable x87 (bit 0) and SSE (bit 1).
         * Enable AVX (bit 2) if CPUID.1:ECX bit 28 says it's supported.
         * We re-read CPUID.1 here since we need EDX for SSE2 bit 26. */
        int c1_eax, c1_ebx, c1_ecx, c1_edx;
        __asm__ volatile("cpuid" : "=a"(c1_eax), "=b"(c1_ebx), "=c"(c1_ecx), "=d"(c1_edx) : "a"(1));

        uint64_t desired = 0x3;     /* x87 (bit 0) + SSE (bit 1) — always available on x86-64 */
        if (c1_ecx & (1U << 28))    /* CPUID.1:ECX.AVX (bit 28) */
            desired |= (1ULL << 2); /* AVX (bit 2) */

        __asm__ volatile("xsetbv"
                         :
                         : "c"(0), "a"((uint32_t)desired), "d"((uint32_t)(desired >> 32)));

        /* Read back to verify */
        uint32_t xcr0_lo, xcr0_hi;
        __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
        uint64_t xcr0_now = ((uint64_t)xcr0_hi << 32) | xcr0_lo;
        kprintf("[CPU] XCR0 = 0x%llx (x87=%d SSE=%d AVX=%d)\n", (unsigned long long)xcr0_now,
                (int)(xcr0_now & 1), (int)((xcr0_now >> 1) & 1), (int)((xcr0_now >> 2) & 1));
    }

    /* Enable NXE in EFER if supported */
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx) : "a"(0x80000001));
    if (rdx & (1U << 20)) { /* CPUID.80000001h:EDX bit 20 = NX (XD) support */
        efer |= EFER_NXE;
        write_msr(0xC0000080, efer);
        kprintf("[CPU] NXE enabled\n");
    } else {
        kprintf("[CPU] NX not supported\n");
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * Section 2 — CPU Hotplug
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── Forward declarations ──────────────────────────────────────────── */

/* Declared in scheduler.c — migrate all tasks from one CPU to another */
extern int scheduler_migrate_tasks_from(int from_cpu);

/* ── Global state ───────────────────────────────────────────────────── */

/* Lock protecting hotplug state transitions — serialises online/offline */
spinlock_t cpuhp_lock = SPINLOCK_INIT;

/* Notifier list for future expansion (e.g. ACPI thermal, cpufreq) */
#define CPUHP_NOTIFIER_MAX 8
static cpuhp_notify_fn cpuhp_notifiers[CPUHP_NOTIFIER_MAX];
static int cpuhp_notifier_count = 0;

/* ── Initialisation ─────────────────────────────────────────────────── */

void cpuhp_init(void) {
    for (int i = 0; i < CPUHP_MAX_CPUS; i++)
        cpuhp_cpu_state[i] = CPUHP_STATE_DEAD;

    /* BSP (CPU 0) is always present and online after boot */
    cpuhp_cpu_state[0] = CPUHP_STATE_ONLINE;

    cpuhp_notifier_count = 0;
    memset(cpuhp_notifiers, 0, sizeof(cpuhp_notifiers));

    kprintf("[CPU] Hotplug initialized (max %d CPUs)\n", CPUHP_MAX_CPUS);
}

/* ── Notifier registration ──────────────────────────────────────────── */

int cpuhp_register_notify(cpuhp_notify_fn fn) {
    if (!fn)
        return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&cpuhp_lock, &irq_flags);

    if (cpuhp_notifier_count >= CPUHP_NOTIFIER_MAX) {
        spinlock_irqsave_release(&cpuhp_lock, irq_flags);
        return -EINVAL;
    }

    cpuhp_notifiers[cpuhp_notifier_count++] = fn;

    spinlock_irqsave_release(&cpuhp_lock, irq_flags);
    return 0;
}

void cpuhp_notify(void) {
    for (int i = 0; i < cpuhp_notifier_count; i++) {
        if (cpuhp_notifiers[i])
            cpuhp_notifiers[i]();
    }
}

/* ── State query helpers ────────────────────────────────────────────── */

int cpuhp_is_online(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= CPUHP_MAX_CPUS)
        return 0;
    __asm__ volatile("" ::: "memory");
    int state = (int)cpuhp_cpu_state[cpu_id];
    __asm__ volatile("" ::: "memory");
    return state == CPUHP_STATE_ONLINE;
}

int cpuhp_online_count(void) {
    int count = 0;
    for (int i = 0; i < smp_cpu_count; i++) {
        __asm__ volatile("" ::: "memory");
        if (cpuhp_cpu_state[i] == CPUHP_STATE_ONLINE)
            count++;
    }
    return count;
}

/* ── Task migration ─────────────────────────────────────────────────── */

/*
 * Migrate all runnable tasks from @cpu_id to other online CPUs.
 *
 * Called with cpuhp_lock held and interrupts disabled.
 * Delegates the actual migration to the scheduler's
 * scheduler_migrate_tasks_from() function.
 *
 * Returns CPUHP_OK on success, CPUHP_ERR_BUSY if migration was incomplete.
 */
int cpuhp_migrate_tasks_away(int cpu_id) {
    int migrated;

    if (cpu_id < 0 || cpu_id >= smp_cpu_count)
        return CPUHP_ERR_INVAL;

    /* Ensure at least one other online CPU exists to receive tasks */
    int other_online = 0;
    for (int i = 0; i < smp_cpu_count; i++) {
        if (i != cpu_id && cpuhp_cpu_state[i] == CPUHP_STATE_ONLINE) {
            other_online++;
        }
    }

    if (!other_online) {
        kprintf("[CPU] Cannot migrate from CPU %d: no other online CPUs\n", cpu_id);
        return CPUHP_ERR_BUSY;
    }

    /* Delegate to scheduler */
    migrated = scheduler_migrate_tasks_from(cpu_id);
    if (migrated < 0) {
        kprintf("[CPU] Task migration from CPU %d failed (err=%d)\n", cpu_id, migrated);
        return CPUHP_ERR_BUSY;
    }

    /* Handle the currently running process on that CPU.
     * This process was executing (not on the runqueue) and must
     * be re-enqueued on a destination CPU — otherwise it becomes
     * permanently stranded (PROCESS_READY state, but no runqueue).
     *
     * We use the public scheduler_add() API which acquires sched_lock
     * internally and performs NUMA-aware placement on the current
     * (running) CPU's queue. */
    struct cpu_info *ci = &cpu_info_array[cpu_id];
    if (ci->current_process && ci->current_process->state == PROCESS_RUNNING) {
        struct process *cur = ci->current_process;
        cur->state = PROCESS_READY;
        cur->on_cpu = 0;
        scheduler_add(cur);
        migrated++;
    }

    kprintf("[CPU] Migrated %d tasks from CPU %d\n", migrated, cpu_id);
    return CPUHP_OK;
}

/* ── State transition helpers ───────────────────────────────────────── */

/*
 * Drain any outstanding timers/IPIs on the target CPU.
 * In a full implementation this would wait for the CPU to acknowledge
 * the offline request via an IPI. Here we do a lightweight spin-wait
 * to let any pending operations drain.
 */
static void cpuhp_drain_pending(int cpu_id) {
    (void)cpu_id;
    /* Brief pause to allow pending operations to complete */
    for (volatile int i = 0; i < 10000; i++) {
        __asm__ volatile("pause");
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * CPU hotplug state machine (CPUHP_* states)
 *
 * Each CPU is driven through an ordered sequence of states between the
 * OFFLINE and ONLINE terminals.  Every intermediate state owns at most one
 * startup callback (run while bringing the CPU online) and one teardown
 * callback (run while taking it offline).  The sequence is derived from the
 * numeric ordering of the state enum: ascending for bring-up, descending
 * for take-down — mirroring Linux's cpuhp_up()/cpuhp_down() at this kernel's
 * scale.
 * ═══════════════════════════════════════════════════════════════════════ */

/* One step in the bring-up/tear-down sequence.  A NULL callback means that
 * particular state has no work to do on that side of a transition. */
struct cpuhp_step {
    enum cpuhp_state state; /* state this step represents */
    const char *name;
    int (*startup)(int cpu);  /* run bringing CPU online */
    int (*teardown)(int cpu); /* run taking CPU offline */
};

static int cpuhp_step_irqwork_online(int cpu) {
    irq_work_cpu_online(cpu);
    return CPUHP_OK;
}
static int cpuhp_step_irqwork_offline(int cpu) {
    irq_work_cpu_offline(cpu);
    return CPUHP_OK;
}
static int cpuhp_step_slab_online(int cpu) {
    slab_cpu_online(cpu);
    return CPUHP_OK;
}
static int cpuhp_step_pmm_online(int cpu) {
    pmm_cpu_online(cpu);
    return CPUHP_OK;
}
static int cpuhp_step_pmm_offline(int cpu) {
    pmm_cpu_offline(cpu);
    return CPUHP_OK;
}

static int cpuhp_step_slab_offline(int cpu) {
    return slab_cpu_offline(cpu) == 0 ? CPUHP_OK : CPUHP_ERR_BUSY;
}

/* Take-down step 3: stop scheduling new tasks on @cpu and migrate the
 * runnable ones to other online CPUs.  Re-enables scheduling if migrated
 * tasks could not all be moved, so a failed offline never strands the CPU
 * with its scheduler permanently disabled. */
static int cpuhp_step_migrate(int cpu) {
    cpu_info_array[cpu].scheduler_enabled = 0;
    int ret = cpuhp_migrate_tasks_away(cpu);
    if (ret != CPUHP_OK)
        cpu_info_array[cpu].scheduler_enabled = 1;
    return ret;
}

static int cpuhp_step_drain(int cpu) {
    cpuhp_drain_pending(cpu);
    return CPUHP_OK;
}

/* Take-down step 1: refuse to accept new tasks on @cpu. */
static int cpuhp_step_sched_offline(int cpu) {
    cpu_info_array[cpu].scheduler_enabled = 0;
    return CPUHP_OK;
}

/* The transition table, in ascending state order.  Bring-up runs each step's
 * startup callback (skipping NULLs) as it walks upward to ONLINE; take-down
 * walks downward from ONLINE running each step's teardown callback. */
static const struct cpuhp_step cpuhp_steps[] = {
    {CPUHP_STATE_PMM, "pmm", cpuhp_step_pmm_online, cpuhp_step_pmm_offline},
    {CPUHP_STATE_SLAB, "slab", cpuhp_step_slab_online, cpuhp_step_slab_offline},
    {CPUHP_STATE_IRQWORK, "irq_work", cpuhp_step_irqwork_online, cpuhp_step_irqwork_offline},
    {CPUHP_STATE_MIGRATE, "migrate", NULL, cpuhp_step_migrate},
    {CPUHP_STATE_DRAIN, "drain", NULL, cpuhp_step_drain},
    {CPUHP_STATE_SCHED, "sched", NULL, cpuhp_step_sched_offline},
    {CPUHP_STATE_ONLINE, "online", NULL, NULL},
};
#define CPUHP_STEP_COUNT (sizeof(cpuhp_steps) / sizeof(cpuhp_steps[0]))

/* Walk a CPU from its current state upward to ONLINE, running each step's
 * startup callback in sequence.  Called with cpuhp_lock held and interrupts
 * disabled.  Returns CPUHP_OK on success, or the failing step's error. */
static int cpuhp_bringup(int cpu_id) {
    enum cpuhp_state cur = cpuhp_cpu_state[cpu_id];
    int i;

    if (cur >= CPUHP_STATE_ONLINE)
        return CPUHP_OK;

    for (i = 0; i < (int)CPUHP_STEP_COUNT; i++) {
        const struct cpuhp_step *s = &cpuhp_steps[i];
        if (s->state <= cur || s->state >= CPUHP_STATE_ONLINE)
            continue;
        if (s->startup) {
            int r = s->startup(cpu_id);
            if (r != CPUHP_OK) {
                kprintf("[CPU] bring CPU %d online: step %s failed (err=%d)\n", cpu_id, s->name, r);
                return r;
            }
        }
        cpuhp_cpu_state[cpu_id] = s->state;
    }

    cpuhp_cpu_state[cpu_id] = CPUHP_STATE_ONLINE;
    return CPUHP_OK;
}

/* Walk a CPU downward from its current state to OFFLINE, running each step's
 * teardown callback in reverse order.  On failure the CPU is left at the last
 * successfully-reached state (never counted as online).  Called with
 * cpuhp_lock held and interrupts disabled. */
static int cpuhp_teardown(int cpu_id) {
    enum cpuhp_state cur = cpuhp_cpu_state[cpu_id];
    int i;

    if (cur < CPUHP_STATE_ONLINE)
        return CPUHP_OK; /* already offline or below */

    for (i = (int)CPUHP_STEP_COUNT - 1; i >= 0; i--) {
        const struct cpuhp_step *s = &cpuhp_steps[i];
        if (s->state >= cur)
            continue;
        if (s->teardown) {
            int r = s->teardown(cpu_id);
            if (r != CPUHP_OK) {
                kprintf("[CPU] take CPU %d offline: step %s failed (err=%d)\n", cpu_id, s->name, r);
                return r;
            }
        }
        cpuhp_cpu_state[cpu_id] = s->state;
    }

    cpuhp_cpu_state[cpu_id] = CPUHP_STATE_OFFLINE;
    return CPUHP_OK;
}

/* ── Public API ─────────────────────────────────────────────────────── */

int cpuhp_bring_cpu(int cpu_id) {
    int ret = CPUHP_OK;

    if (cpu_id < 0 || cpu_id >= CPUHP_MAX_CPUS)
        return CPUHP_ERR_INVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&cpuhp_lock, &irq_flags);

    enum cpuhp_state cur = cpuhp_cpu_state[cpu_id];

    if (cur == CPUHP_STATE_ONLINE) {
        /* Already online — no-op */
        spinlock_irqsave_release(&cpuhp_lock, irq_flags);
        return CPUHP_OK;
    }

    if (cur == CPUHP_STATE_DEAD) {
        /* Cannot bring a dead (physically absent) CPU online */
        kprintf("[CPU] Cannot bring CPU %d online: not present\n", cpu_id);
        ret = CPUHP_ERR_INVAL;
        goto out;
    }

    /* Drive the CPU through the bring-up sequence (OFFLINE → ONLINE),
     * running each step's startup callback. */
    ret = cpuhp_bringup(cpu_id);

    if (ret == CPUHP_OK)
        kprintf("[CPU] CPU %d brought online (now %d online)\n", cpu_id, cpuhp_online_count());
    cpuhp_notify();

out:
    spinlock_irqsave_release(&cpuhp_lock, irq_flags);
    return ret;
}

int cpuhp_take_cpu_offline(int cpu_id) {
    int ret = CPUHP_OK;

    if (cpu_id < 0 || cpu_id >= CPUHP_MAX_CPUS)
        return CPUHP_ERR_INVAL;

    if (cpu_id == 0) {
        kprintf("[CPU] Cannot offline BSP (CPU 0)\n");
        return CPUHP_ERR_BSP;
    }

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&cpuhp_lock, &irq_flags);

    enum cpuhp_state cur = cpuhp_cpu_state[cpu_id];

    if (cur != CPUHP_STATE_ONLINE) {
        /* Already offline or dead */
        spinlock_irqsave_release(&cpuhp_lock, irq_flags);
        return cur == CPUHP_STATE_OFFLINE ? CPUHP_OK : CPUHP_ERR_INVAL;
    }

    /* Ensure there's at least one other online CPU to receive tasks */
    int other_online = 0;
    for (int i = 0; i < smp_cpu_count; i++) {
        if (i != cpu_id && cpuhp_cpu_state[i] == CPUHP_STATE_ONLINE) {
            other_online++;
            break;
        }
    }

    if (!other_online) {
        kprintf("[CPU] Refusing to offline CPU %d: no other online CPUs\n", cpu_id);
        ret = CPUHP_ERR_BUSY;
        goto out;
    }

    /* Drive the CPU down through the teardown sequence (ONLINE → OFFLINE),
     * running each step's teardown callback in reverse order.  The machine
     * disables scheduling, drains pending ops, migrates tasks, then drains
     * the per-CPU irq-work, slab and PMM caches. */
    ret = cpuhp_teardown(cpu_id);

    if (ret == CPUHP_OK)
        kprintf("[CPU] CPU %d taken offline (now %d online)\n", cpu_id, cpuhp_online_count());
    cpuhp_notify();

out:
    spinlock_irqsave_release(&cpuhp_lock, irq_flags);
    return ret;
}

/**
 * cpu_init - Initialize a specific CPU and bring it online
 * @cpu_id: The ID of the CPU to initialize
 *
 * Brings the specified CPU online via the cpuhp infrastructure.
 * The target CPU transitions from DEAD or OFFLINE to ONLINE state.
 *
 * Context: May be called during boot or hotplug. Takes cpuhp_lock internally.
 * Return: 0 on success, -EINVAL if @cpu_id is out of range, or a negative
 *         error code from cpuhp_bring_cpu().
 */
static int cpu_init(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= CPUHP_MAX_CPUS)
        return -EINVAL;

    kprintf("[CPU] cpu_init: initializing CPU %d\n", cpu_id);

    /* Bring the CPU online via the existing cpuhp infrastructure */
    int ret = cpuhp_bring_cpu(cpu_id);
    if (ret < 0) {
        kprintf("[CPU] cpu_init: failed to bring CPU %d online (ret=%d)\n", cpu_id, ret);
        return ret;
    }

    kprintf("[CPU] cpu_init: CPU %d is now online\n", cpu_id);
    return 0;
}
/**
 * cpu_idle - Enter idle state until next interrupt
 *
 * Halts the CPU with interrupts enabled (sti; hlt; cli) to conserve
 * power while waiting for the next interrupt. After the interrupt
 * is handled, interrupts are disabled again before returning.
 *
 * Context: Must be called with interrupts enabled. Disables interrupts
 *          before returning.
 * Return: 0 on success.
 */
static int cpu_idle(void) {
    /* Enter halt state to conserve power while waiting for interrupts */
    __asm__ volatile("sti; hlt; cli");
    return 0;
}
/* ── cpu_die: Take a CPU offline ──────────────────────────────────────── */
static int cpu_die(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= CPUHP_MAX_CPUS)
        return -EINVAL;

    kprintf("[CPU] cpu_die: taking CPU %d offline\n", cpu_id);

    int ret = cpuhp_take_cpu_offline(cpu_id);
    if (ret < 0) {
        kprintf("[CPU] cpu_die: failed to take CPU %d offline (ret=%d)\n", cpu_id, ret);
        return ret;
    }

    kprintf("[CPU] cpu_die: CPU %d is now offline\n", cpu_id);
    return 0;
}
/* ── cpu_online: Check if a CPU is online ─────────────────────────────── */
static int cpu_online(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= CPUHP_MAX_CPUS)
        return 0;
    return cpuhp_is_online(cpu_id);
}
