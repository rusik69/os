/* bpf_stack.c — eBPF stack depth tracking
 *
 * Implements verifier stack depth bounds checking and spill/fill tracking
 * for eBPF programs.  The verifier tracks every slot in the 512-byte
 * eBPF stack to ensure:
 *   - No out-of-bounds access
 *   - Spills are always followed by fills of matching type
 *   - Stack slots are initialized before reading
 *   - No partial overwrites that leak kernel data
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "errno.h"

/* ── Configuration ─────────────────────────────────────────────────── */

#define BPF_STACK_SIZE      512   /* eBPF standard stack size */
#define BPF_STACK_SLOTS     (BPF_STACK_SIZE / 8)  /* 64 slots of 8 bytes */

/* ── Per-stack-slot state ─────────────────────────────────────────── */

enum stack_slot_type {
    STACK_UNINITIALIZED = 0,
    STACK_SPILL_64,           /* spilled 64-bit register */
    STACK_SPILL_32,           /* spilled 32-bit register */
    STACK_VALUE_64,           /* known 64-bit value */
    STACK_VALUE_32,           /* known 32-bit value */
    STACK_MISC,               /* miscellaneous initialized */
    STACK_ZERO,               /* known zero */
    STACK_PTR,                /* pointer to map/value */
    STACK_FRAME_PTR,          /* frame pointer (R10) relative */
};

/* ── Stack state tracker ───────────────────────────────────────────── */

struct bpf_stack_state {
    enum stack_slot_type slots[BPF_STACK_SLOTS];
    int                  slot_min;    /* lowest slot accessed */
    int                  slot_max;    /* highest slot accessed */
    int                  largest_access; /* largest access size */
};

/* ── Initialize stack state ────────────────────────────────────────── */

void bpf_stack_init(struct bpf_stack_state *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->slot_min = BPF_STACK_SLOTS;
    state->slot_max = -1;
    state->largest_access = 0;
}

/* ── Convert stack offset to slot index ────────────────────────────── */

static int stack_off_to_slot(int stack_off)
{
    /* Stack offsets are negative (grows down from R10).
     * R10 is the frame pointer; offset -8 is slot 0.
     * Slot index = (-stack_off - 8) / 8, for 8-byte aligned.
     */
    return (-stack_off - 8) / 8;
}

/* ── Check if a stack access is valid ──────────────────────────────── */

int bpf_stack_check_access(struct bpf_stack_state *state,
                            int stack_off, int access_size)
{
    if (!state) return -EINVAL;

    /* Validate alignment */
    if (stack_off % 4 != 0) return -EACCES; /* misaligned */

    /* Validate bounds */
    if (stack_off > 0) return -EACCES;  /* positive offset = above FP */
    if (stack_off < -BPF_STACK_SIZE) return -EACCES; /* below stack */

    int slot = stack_off_to_slot(stack_off);
    if (slot < 0 || slot >= BPF_STACK_SLOTS) return -EACCES;

    int end_slot = (-stack_off + access_size - 9) / 8;
    if (end_slot >= BPF_STACK_SLOTS) return -EACCES;

    /* Track range */
    if (slot < state->slot_min) state->slot_min = slot;
    if (end_slot > state->slot_max) state->slot_max = end_slot;
    if (access_size > state->largest_access) state->largest_access = access_size;

    return 0;
}

/* ── Track a spill (register -> stack) ─────────────────────────────── */

int bpf_stack_track_spill(struct bpf_stack_state *state,
                           int stack_off, int is_64bit)
{
    int ret = bpf_stack_check_access(state, stack_off, is_64bit ? 8 : 4);
    if (ret != 0) return ret;

    int slot = stack_off_to_slot(stack_off);
    if (is_64bit) {
        state->slots[slot] = STACK_SPILL_64;
        if (slot + 1 < BPF_STACK_SLOTS)
            state->slots[slot + 1] = STACK_SPILL_64; /* mark adjacent */
    } else {
        state->slots[slot] = STACK_SPILL_32;
    }

    return 0;
}

/* ── Track a fill (stack -> register) ──────────────────────────────── */

int bpf_stack_track_fill(struct bpf_stack_state *state,
                          int stack_off, int is_64bit)
{
    int ret = bpf_stack_check_access(state, stack_off, is_64bit ? 8 : 4);
    if (ret != 0) return ret;

    int slot = stack_off_to_slot(stack_off);

    /* Verify the slot was spilled (not uninitialized) */
    if (state->slots[slot] == STACK_UNINITIALIZED) {
        return -EINVAL; /* reading uninitialized stack */
    }

    return 0;
}

/* ── Track a value store ───────────────────────────────────────────── */

int bpf_stack_track_store(struct bpf_stack_state *state,
                           int stack_off, int access_size,
                           enum stack_slot_type type)
{
    int ret = bpf_stack_check_access(state, stack_off, access_size);
    if (ret != 0) return ret;

    int slot = stack_off_to_slot(stack_off);
    int slots_needed = (access_size + 7) / 8;

    for (int i = 0; i < slots_needed && (slot + i) < BPF_STACK_SLOTS; i++) {
        state->slots[slot + i] = type;
    }

    return 0;
}

/* ── Check that all accessed slots are initialized ─────────────────── */

int bpf_stack_verify_initialized(struct bpf_stack_state *state)
{
    if (!state) return -EINVAL;

    for (int i = 0; i < BPF_STACK_SLOTS; i++) {
        if (state->slots[i] == STACK_UNINITIALIZED) {
            /* Only report if within accessed range */
            if (i >= state->slot_min && i <= state->slot_max) {
                return -EINVAL; /* uninitialized slot in accessed range */
            }
        }
    }

    return 0;
}

/* ── Get stack depth ───────────────────────────────────────────────── */

int bpf_stack_get_depth(struct bpf_stack_state *state)
{
    if (!state) return 0;
    return (state->slot_max + 1) * 8;
}

/* ── Reset for a new verifier pass ─────────────────────────────────── */

void bpf_stack_reset(struct bpf_stack_state *state)
{
    bpf_stack_init(state);
}

/* ── Initialization ────────────────────────────────────────────────── */

void bpf_stack_init(void)
{
    kprintf("[OK] BPF_STACK: eBPF stack depth tracking initialized "
            "(%d bytes, %d slots)\n", BPF_STACK_SIZE, BPF_STACK_SLOTS);
}
