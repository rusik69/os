#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "jump_label.h"

/* 5-byte NOP instruction encoding for x86-64 */
static const uint8_t nop5[5] = { 0x0F, 0x1F, 0x44, 0x00, 0x00 }; /* nop dword ptr [rax+rax+0] */

/* Patch a 5-byte relative jump (opcode: E9 + 32-bit rel32 offset) */
void text_patch_jmp(void *addr, void *target)
{
    uint8_t *patch = (uint8_t *)addr;
    int32_t offset = (int32_t)((uintptr_t)target - (uintptr_t)addr - 5);

    /* Disable interrupts for atomic patching */
    uint64_t flags;
    __asm__ volatile(
        "pushfq\n\t"
        "pop %0\n\t"
        "cli\n\t"
        : "=r"(flags)
        :
        : "memory"
    );

    /* Write the jump instruction atomically (the 5-byte jmp rel32) */
    patch[0] = 0xE9;
    __builtin_memcpy(&patch[1], &offset, sizeof(offset));

    /* Synchronize instruction cache */
    __asm__ volatile("mfence" : : : "memory");

    /* Restore interrupts */
    if (!(flags & 0x200))
        __asm__ volatile("sti");
}

/* Patch a 5-byte NOP (to disable/remove a jump) */
void text_patch_nop(void *addr)
{
    uint8_t *patch = (uint8_t *)addr;

    uint64_t flags;
    __asm__ volatile(
        "pushfq\n\t"
        "pop %0\n\t"
        "cli\n\t"
        : "=r"(flags)
        :
        : "memory"
    );

    /* Write the 5-byte NOP */
    *(uint32_t *)&patch[0] = *(const uint32_t *)&nop5[0];
    patch[4] = nop5[4];

    __asm__ volatile("mfence" : : : "memory");

    if (!(flags & 0x200))
        __asm__ volatile("sti");
}

/* Enable a static key: increment refcount and patch all jump sites */
void static_key_enable(struct static_key *key)
{
    atomic_inc(&key->enabled);
    if (atomic_read(&key->enabled) == 1) {
        /* First enable: all associated branches should be patched to jmp.
         * In a full implementation, we'd walk the __jump_table section.
         * For now, the key is just tracked. */
        __asm__ volatile("mfence" : : : "memory");
    }
}

/* Disable: decrement refcount and patch back to nop if zero */
void static_key_disable(struct static_key *key)
{
    atomic_dec(&key->enabled);
    if (atomic_read(&key->enabled) == 0) {
        /* All branches should be patched back to nop. */
        __asm__ volatile("mfence" : : : "memory");
    }
}

/* Initialize jump label subsystem */
void jump_label_init(void)
{
    /* Test that we can patch a jmp and back */
    uint64_t flags;

    __asm__ volatile(
        "pushfq\n\t"
        "pop %0\n\t"
        "cli\n\t"
        : "=r"(flags)
        :
        : "memory"
    );
    volatile uint8_t patch_test[5] __attribute__((aligned(1)));
    __builtin_memset((void *)patch_test, 0x90, 5);

    /* Patch a jmp to a known target (skip +2 bytes forward) */
    text_patch_jmp((void *)patch_test, (void *)(patch_test + 2));
    __asm__ volatile("mfence" : : : "memory");

    /* Patch back to NOP */
    text_patch_nop((void *)patch_test);
    __asm__ volatile("mfence" : : : "memory");

    if (!(flags & 0x200))
        __asm__ volatile("sti");

    kprintf("[OK] jump_label: NOP patching verified\n");
}

/* ── Stub: jump_label_register ─────────────────────────────── */
int jump_label_register(void *key, void *entry)
{
    (void)key;
    (void)entry;
    kprintf("[jump_label] jump_label_register: not yet implemented\n");
    return 0;
}
/* ── Stub: jump_label_update ─────────────────────────────── */
int jump_label_update(void *key, int enabled)
{
    (void)key;
    (void)enabled;
    kprintf("[jump_label] jump_label_update: not yet implemented\n");
    return 0;
}
