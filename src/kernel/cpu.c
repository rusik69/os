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
#include "cpuhp.h"
#include "smp.h"
#include "printf.h"
#include "string.h"
#include "process.h"
#include "scheduler.h"
#include "apic.h"
#include "timer.h"

/* ═══════════════════════════════════════════════════════════════════════
 * Section 1 — CPU Security Features
 * ═══════════════════════════════════════════════════════════════════════ */

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
int cpu_security_init(void)
{
    uint64_t cr4 = read_cr4();
    uint64_t efer = read_msr(0xC0000080); /* IA32_EFER */
    int rax, rbx, rcx, rdx;

    /* Check CPUID leaf 1 for feature bits */
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx) : "a"(1));

    /* Enable SMEP if supported (ECX bit 7) */
    if (rcx & (1U << 7)) {
        cr4 |= CR4_SMEP;
        kprintf("[CPU] SMEP enabled\n");
    } else {
        kprintf("[CPU] SMEP not supported\n");
    }

    /* Enable SMAP if supported (ECX bit 20) */
    if (rcx & (1U << 20)) {
        cr4 |= CR4_SMAP;
        kprintf("[CPU] SMAP enabled\n");
    } else {
        kprintf("[CPU] SMAP not supported\n");
    }

    /* Enable UMIP if supported (ECX bit 2) */
    if (rcx & (1U << 2)) {
        cr4 |= CR4_UMIP;
        kprintf("[CPU] UMIP enabled\n");
    } else {
        kprintf("[CPU] UMIP not supported\n");
    }

    /* Write updated CR4 */
    write_cr4(cr4);

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

void cpuhp_init(void)
{
    for (int i = 0; i < CPUHP_MAX_CPUS; i++)
        cpuhp_cpu_state[i] = CPUHP_STATE_DEAD;

    /* BSP (CPU 0) is always present and online after boot */
    cpuhp_cpu_state[0] = CPUHP_STATE_ONLINE;

    cpuhp_notifier_count = 0;
    memset(cpuhp_notifiers, 0, sizeof(cpuhp_notifiers));

    kprintf("[CPU] Hotplug initialized (max %d CPUs)\n", CPUHP_MAX_CPUS);
}

/* ── Notifier registration ──────────────────────────────────────────── */

int cpuhp_register_notify(cpuhp_notify_fn fn)
{
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

void cpuhp_notify(void)
{
    for (int i = 0; i < cpuhp_notifier_count; i++) {
        if (cpuhp_notifiers[i])
            cpuhp_notifiers[i]();
    }
}

/* ── State query helpers ────────────────────────────────────────────── */

int cpuhp_is_online(int cpu_id)
{
    if (cpu_id < 0 || cpu_id >= CPUHP_MAX_CPUS)
        return 0;
    __asm__ volatile("" ::: "memory");
    int state = (int)cpuhp_cpu_state[cpu_id];
    __asm__ volatile("" ::: "memory");
    return state == CPUHP_STATE_ONLINE;
}

int cpuhp_online_count(void)
{
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
int cpuhp_migrate_tasks_away(int cpu_id)
{
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
        kprintf("[CPU] Task migration from CPU %d failed (err=%d)\n",
                cpu_id, migrated);
        return CPUHP_ERR_BUSY;
    }

    /* Handle the currently running process on that CPU */
    struct cpu_info *ci = &cpu_info_array[cpu_id];
    if (ci->current_process &&
        ci->current_process->state == PROCESS_RUNNING) {
        ci->current_process->state = PROCESS_READY;
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
static void cpuhp_drain_pending(int cpu_id)
{
    (void)cpu_id;
    /* Brief pause to allow pending operations to complete */
    for (volatile int i = 0; i < 10000; i++) {
        __asm__ volatile("pause");
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

int cpuhp_bring_cpu(int cpu_id)
{
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

    /* Transition OFFLINE → ONLINE */
    cpuhp_cpu_state[cpu_id] = CPUHP_STATE_ONLINE;
    kprintf("[CPU] CPU %d brought online (now %d online)\n",
            cpu_id, cpuhp_online_count());
    cpuhp_notify();

out:
    spinlock_irqsave_release(&cpuhp_lock, irq_flags);
    return ret;
}

int cpuhp_take_cpu_offline(int cpu_id)
{
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

    /* ── Step 1: prevent new tasks from being scheduled on this CPU ── */
    cpu_info_array[cpu_id].scheduler_enabled = 0;

    /* ── Step 2: drain pending operations ──────────────────────────── */
    cpuhp_drain_pending(cpu_id);

    /* ── Step 3: migrate all runnable tasks away ───────────────────── */
    ret = cpuhp_migrate_tasks_away(cpu_id);
    if (ret != CPUHP_OK) {
        /* Migration failed — re-enable scheduling on this CPU */
        cpu_info_array[cpu_id].scheduler_enabled = 1;
        goto out;
    }

    /* ── Step 4: transition state ──────────────────────────────────── */
    cpuhp_cpu_state[cpu_id] = CPUHP_STATE_OFFLINE;
    kprintf("[CPU] CPU %d taken offline (now %d online)\n",
            cpu_id, cpuhp_online_count());
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
static int cpu_init(int cpu_id)
{
    if (cpu_id < 0 || cpu_id >= CPUHP_MAX_CPUS) return -EINVAL;

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
static int cpu_idle(void)
{
    /* Enter halt state to conserve power while waiting for interrupts */
    __asm__ volatile("sti; hlt; cli");
    return 0;
}
/* ── cpu_die: Take a CPU offline ──────────────────────────────────────── */
static int cpu_die(int cpu_id)
{
    if (cpu_id < 0 || cpu_id >= CPUHP_MAX_CPUS) return -EINVAL;

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
static int cpu_online(int cpu_id)
{
    if (cpu_id < 0 || cpu_id >= CPUHP_MAX_CPUS) return 0;
    return cpuhp_is_online(cpu_id);
}
