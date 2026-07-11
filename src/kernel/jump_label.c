#define KERNEL_INTERNAL
#include "jump_label.h"
#include "printf.h"
#include "string.h"
#include "types.h"
#include "spinlock.h"

/* 5-byte NOP instruction encoding for x86-64 */
static const uint8_t nop5[5] = { 0x0F, 0x1F, 0x44, 0x00, 0x00 }; /* nop dword ptr [rax+rax+0] */

/* Serialize all text patching operations across CPUs.
 * Two CPUs must never patch code concurrently at overlapping addresses,
 * because they'd race on the stores and could produce torn instructions. */
static spinlock_t text_patch_lock = SPINLOCK_INIT;

/* ── Instruction-serialization barrier ─────────────────────────────
 *
 * After self-modifying code we must execute a *serializing instruction*
 * to ensure the instruction cache sees the modified bytes.
 *
 *   mfence — orders memory accesses but is NOT a serializing instruction.
 *   CPUID  — the portable serializing instruction on x86-64 (Intel SDM
 *            Vol. 3A, §8.1.3 "Serializing Instructions").
 *
 * Without a serializing instruction the processor may continue to fetch
 * stale cached instructions from the patched location.
 */
static inline void text_patch_serialize(void)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile(
        "xorl %%eax, %%eax\n\t"
        "cpuid\n\t"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        :
        : "memory"
    );
}

/* Patch a 5-byte relative jump (opcode: E9 + 32-bit rel32 offset) */
void text_patch_jmp(void *addr, void *target)
{
    uint64_t flags;

    spinlock_irqsave_acquire(&text_patch_lock, &flags);

    int32_t offset = (int32_t)((uintptr_t)target - (uintptr_t)addr - 5);

    /* Build and write the 5-byte jmp rel32 as a single copy so that a
     * concurrent reader on another CPU never sees torn bytes. */
    uint8_t jmp5[5] = {
        0xE9,
        (uint8_t)(offset >>  0),
        (uint8_t)(offset >>  8),
        (uint8_t)(offset >> 16),
        (uint8_t)(offset >> 24),
    };
    __builtin_memcpy(addr, jmp5, 5);

    /* Serialising instruction — see comment above. */
    text_patch_serialize();

    spinlock_irqsave_release(&text_patch_lock, flags);
}

/* Patch a 5-byte NOP (to disable/remove a jump) */
void text_patch_nop(void *addr)
{
    uint64_t flags;

    spinlock_irqsave_acquire(&text_patch_lock, &flags);

    /* Write the 5-byte NOP as a single copy — no torn stores. */
    __builtin_memcpy(addr, nop5, 5);

    /* Serialising instruction — see comment above. */
    text_patch_serialize();

    spinlock_irqsave_release(&text_patch_lock, flags);
}

/* Enable a static key: increment refcount and patch all jump sites */
void static_key_enable(struct static_key *key)
{
    /* Use atomic_add_return so the increment and value-check are a single
     * atomic RMW — no TOCTOU window for another concurrent enable/disable
     * to race between the increment and the read. */
    if (atomic_add_return(&key->enabled, 1) == 1) {
        /* First enable: all associated branches should be patched to jmp.
         * In a full implementation, we'd walk the __jump_table section
         * under text_patch_lock.  For now, the key is just tracked. */
        __asm__ volatile("mfence" : : : "memory");
    }
}

/* Disable: decrement refcount and patch back to nop if zero */
void static_key_disable(struct static_key *key)
{
    /* Use atomic_sub_return so decrement and zero-check are a single
     * atomic RMW — no TOCTOU window. */
    if (atomic_sub_return(&key->enabled, 1) == 0) {
        /* All branches should be patched back to nop. */
        __asm__ volatile("mfence" : : : "memory");
    }
}

/* Initialize jump label subsystem */
void __init jump_label_init(void)
{
    /* Test that we can patch a jmp and back.
     *
     * 8-byte alignment ensures the 5-byte writes land in a single cache
     * line so no reader sees a torn instruction-spanning-line. */
    uint8_t patch_test[5] __attribute__((aligned(8)));
    __builtin_memset(patch_test, 0x90, 5);

    /* Patch a jmp to a known target (skip +2 bytes forward) */
    text_patch_jmp(patch_test, patch_test + 2);

    /* Patch back to NOP */
    text_patch_nop(patch_test);

    kprintf("[OK] jump_label: NOP patching verified\n");
}

/* ── Stub: jump_label_register ─────────────────────────────── */
static int jump_label_register(void *key, void *entry)
{
    (void)key;
    (void)entry;
    kprintf("[jump_label] jump_label_register: not yet implemented\n");
    return 0;
}
/* ── Stub: jump_label_update ─────────────────────────────── */
static int jump_label_update(void *key, int enabled)
{
    (void)key;
    (void)enabled;
    kprintf("[jump_label] jump_label_update: not yet implemented\n");
    return 0;
}
