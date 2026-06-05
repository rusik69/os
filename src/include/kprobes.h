#ifndef KPROBES_H
#define KPROBES_H

#include "types.h"
#include "idt.h"

/* ── Kprobe operation states ──────────────────────────────────────────
 *
 * Kprobes allow dynamic insertion of breakpoints at any kernel instruction
 * address.  When the breakpoint fires, a pre_handler runs, the original
 * instruction is single-stepped, then a post_handler runs.
 *
 * This implementation uses INT3 (0xCC) insertion with text_poke for safe
 * code modification and the x86 Trap Flag (TF) for single-stepping.
 */

/* Maximum number of simultaneously registered kprobes */
#define KPROBES_MAX 64

/* Flags for struct kprobe */
#define KPROBE_FLAG_ACTIVE   (1 << 0)  /* kprobe is registered */
#define KPROBE_FLAG_GONE     (1 << 1)  /* kprobe has been hit and removed */

/* Pre-handler return values */
enum kprobe_action {
    KPROBE_ACTION_CONTINUE = 0,  /* single-step and call post_handler */
    KPROBE_ACTION_CALLBACK = 1,  /* call pre_handler only (no single-step) */
};

/* struct kprobe — describes a single dynamic probe point
 *
 * @addr:        Address of the instruction to probe (must be kernel text)
 * @pre_handler: Called just before the probed instruction executes.
 *               If non-NULL, called with a copy of the CPU registers.
 *               Return KPROBE_ACTION_CONTINUE to single-step, or
 *               KPROBE_ACTION_CALLBACK to skip single-step.
 * @post_handler: Called just after the probed instruction executes
 *                (only if pre_handler returned CONTINUE).
 * @flags:       KPROBE_FLAG_* flags (managed by kprobes core)
 * @orig_opcode: First opcode byte saved before INT3 patching
 * @insn_len:    Length of the original instruction (for single-step)
 * @pml4:        Page-table root used to map the probe address
 *
 * Each kprobe monitors a single instruction.  When the INT3 at @addr
 * fires, the core saves the register state, calls @pre_handler, then
 * single-steps the original instruction and calls @post_handler.
 */
struct kprobe {
    uint64_t addr;
    int (*pre_handler)(struct kprobe *kp, struct interrupt_frame *frame);
    void (*post_handler)(struct kprobe *kp, struct interrupt_frame *frame);
    int flags;
    uint8_t orig_opcode;         /* saved first byte of the probed instruction */
    int      insn_len;           /* length in bytes of the probed instruction */
    uint64_t *pml4;              /* page table root used at probe addr */
};

/* ── API ─────────────────────────────────────────────────────── */

/* Initialise the kprobes subsystem.  Must be called once after IDT init. */
void kprobes_init(void);

/* Register a kprobe at the given instruction address.
 * Returns 0 on success, or -1 on failure (table full, invalid address, etc.).
 *
 * The caller must fill @addr, @pre_handler, and/or @post_handler.
 * If both handlers are NULL, -EINVAL is returned.
 *
 * After registration, INT3 is written at @addr.  Each hit calls
 * pre_handler → single-step → post_handler.
 */
int register_kprobe(struct kprobe *kp);

/* Unregister a previously registered kprobe.
 * Restores the original opcode and removes the INT3.
 * Returns 0 on success, -1 if kp was not registered.
 */
int unregister_kprobe(struct kprobe *kp);

/* ── Internal (exposed for fault.c / idt.c integration) ───── */

/* INT3 (#BP) handler entry point — called from the IDT stub.
 * Dispatches to the registered kprobe's pre_handler, then
 * sets up single-stepping via the Trap Flag. */
void kprobe_int3_handler(struct interrupt_frame *frame);

/* Debug (#DB) handler entry point — called from the IDT stub.
 * Handles single-step completion after INT3, calling the
 * kprobe's post_handler, then re-patching INT3. */
void kprobe_debug_handler(struct interrupt_frame *frame);

/* Return non-zero if the given address has an active kprobe */
int kprobe_is_probed(uint64_t addr);

#endif /* KPROBES_H */
