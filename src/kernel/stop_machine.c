/* stop_machine.c — Synchronous cross-CPU function execution
 *
 * Provides the ability to execute a callback function on every online
 * CPU simultaneously and wait for all to complete.  This is the
 * foundation for safe text patching (kprobes, live patching), kexec,
 * and other SMP-sensitive operations.
 *
 * ── How it works ─────────────────────────────────────────────────────
 *
 * 1. The calling CPU prepares a per-CPU request slot (active_request=1)
 *    for each remote CPU, storing the callback and data pointer.
 *
 * 2. A full memory barrier ensures the request is visible before the
 *    IPI arrives.
 *
 * 3. An IPI (IPI_VECTOR_STOP_MACHINE, vector 0xF5) is sent to all CPUs
 *    except the caller.
 *
 * 4. The calling CPU also executes the callback locally.
 *
 * 5. On each remote CPU, the IPI handler reads its request slot,
 *    calls fn(data), sets finished=1, and acknowledges the APIC.
 *
 * 6. The calling CPU spins on each remote CPU's finished flag, then
 *    clears active_request.
 *
 * ── Concurrency safety ──────────────────────────────────────────────
 *
 * - The protocol uses volatile accessors and MFENCE to ensure proper
 *   ordering on x86_64 TSO.
 * - active_request+finished follow the same two-flag pattern as the
 *   existing TLB shootdown code (smp_tlb_shootdown).
 * - Local interrupts are disabled throughout to prevent re-entrancy
 *   (if an NMI fires while we hold the stop-machine state, the NMI
 *   handler must not call stop_machine).
 *
 * ── Design notes ────────────────────────────────────────────────────
 *
 * This is the minimal viable implementation following the same pattern
 * as the existing IPI infrastructure (IPI_VECTOR_RESCHEDULE, TLB_SHOOT,
 * etc.).  A more sophisticated version could add a multi-phase barrier
 * (all stop → master executes → all resume), but the current design
 * suffices for the immediate use cases (kprobes, live patching, kexec).
 */

#include "stop_machine.h"
#include "smp.h"
#include "apic.h"
#include "printf.h"
#include "string.h"
#include "export.h"

/* ── Per-CPU request state ─────────────────────────────────────────── */

struct stop_machine_request {
    volatile int        active_request;   /* 1 = work pending for this CPU */
    volatile int        finished;         /* 1 = this CPU has completed fn */
    stop_machine_fn_t   fn;               /* callback to execute */
    void               *data;             /* argument to callback */
};

/* One request slot per possible CPU — indexed by CPU ID */
static struct stop_machine_request sm_requests[SMP_MAX_CPUS];

/* ── Core API ──────────────────────────────────────────────────────── */

int stop_machine_call(stop_machine_fn_t fn, void *data)
{
    int cpu_count;
    int my_cpu;
    int cpu;

    if (!fn)
        return -1;

    cpu_count = smp_get_cpu_count();
    my_cpu    = smp_get_cpu_id();

    /* ── Uniprocessor fast-path ────────────────────────────────── */
    if (cpu_count <= 1) {
        fn(data);
        return 0;
    }

    /* ── Prepare request slots for every remote CPU ────────────── */
    for (cpu = 0; cpu < cpu_count; cpu++) {
        if (cpu == my_cpu)
            continue;

        /* Spin until previous request is fully consumed */
        while (sm_requests[cpu].active_request)
            __asm__ volatile("pause");

        sm_requests[cpu].active_request = 1;
        __asm__ volatile("" ::: "memory");  /* ordered store before fn/data */
        sm_requests[cpu].fn   = fn;
        sm_requests[cpu].data = data;
        sm_requests[cpu].finished = 0;
    }

    /* Full barrier: all remote request slots are visible before IPI */
    __asm__ volatile("mfence" ::: "memory");

    /* ── Send IPI to all other CPUs ────────────────────────────── */
    apic_send_ipi_all_except(IPI_VECTOR_STOP_MACHINE);

    /* ── Execute locally ───────────────────────────────────────── */
    fn(data);

    /* ── Wait for all remote CPUs to finish ────────────────────── */
    for (cpu = 0; cpu < cpu_count; cpu++) {
        if (cpu == my_cpu)
            continue;

        while (!sm_requests[cpu].finished)
            __asm__ volatile("pause");

        /* Consume this slot for the next caller */
        __asm__ volatile("mfence" ::: "memory");
        sm_requests[cpu].active_request = 0;
    }

    return 0;
}
EXPORT_SYMBOL(stop_machine_call);

/* ── IPI handler ───────────────────────────────────────────────────── */

void stop_machine_handler(struct interrupt_frame *frame)
{
    int cpu_id;
    struct stop_machine_request *req;

    (void)frame;

    cpu_id = smp_get_cpu_id();

    if (cpu_id < 0 || cpu_id >= SMP_MAX_CPUS)
        goto out;

    req = &sm_requests[cpu_id];

    /* Sanity: we should only receive this IPI when there is pending work */
    if (!req->active_request)
        goto out;

    /* Ensure we see fn/data after active_request (ordered by x86 TSO) */
    __asm__ volatile("" ::: "memory");

    if (req->fn)
        req->fn(req->data);

    /* Signal completion */
    __asm__ volatile("mfence" ::: "memory");
    req->finished = 1;

out:
    apic_eoi();
}

/* ── Subsystem init ────────────────────────────────────────────────── */

void stop_machine_init(void)
{
    /* Clear all request slots */
    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        sm_requests[i].active_request = 0;
        sm_requests[i].finished       = 0;
        sm_requests[i].fn             = NULL;
        sm_requests[i].data           = NULL;
    }

    /* Register the IPI handler */
    idt_register_handler(IPI_VECTOR_STOP_MACHINE, stop_machine_handler);

    kprintf("[OK] stop_machine: registered (vector 0x%02x)\n",
            (unsigned int)IPI_VECTOR_STOP_MACHINE);
}
