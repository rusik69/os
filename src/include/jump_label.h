#ifndef JUMP_LABEL_H
#define JUMP_LABEL_H

#include "types.h"
#include "atomic.h"

struct static_key {
    atomic_t enabled;
};

#define STATIC_KEY_INIT { ATOMIC_INIT(0) }

/* Initialize a static key */
#define static_key_init(key) do { atomic_set(&(key)->enabled, 0); } while (0)

/* Enable/disable a static key (update the branch) */
void static_key_enable(struct static_key *key);
void static_key_disable(struct static_key *key);

/* Patch a 5-byte relative jump at 'addr' targeting 'target' */
void text_patch_jmp(void *addr, void *target);

/* Patch a 5-byte NOP at 'addr' (for disabling a jump) */
void text_patch_nop(void *addr);

/* Branch macros — emit a 5-byte nop that can be patched to jmp.
 * 'key' must be a struct static_key pointer or expression. */
#define static_branch_likely(key)                                \
    ({                                                           \
        int __branch;                                            \
        __asm__ volatile(                                        \
            "1: jmp 2f\n"                                        \
            ".pushsection __jump_table, \"aw\"\n"                \
            ".quad 1b, 2f, %0\n"                                 \
            ".popsection\n"                                      \
            "2:\n"                                               \
            : : "i" (&(key)->enabled)                            \
        );                                                       \
        __branch = likely(atomic_read(&(key)->enabled));          \
        __branch;                                                \
    })

#define static_branch_unlikely(key)                              \
    ({                                                           \
        int __branch;                                            \
        __asm__ volatile(                                        \
            "1: jmp 2f\n"                                        \
            ".pushsection __jump_table, \"aw\"\n"                \
            ".quad 1b, 2f, %0\n"                                 \
            ".popsection\n"                                      \
            "2:\n"                                               \
            : : "i" (&(key)->enabled)                            \
        );                                                       \
        __branch = unlikely(atomic_read(&(key)->enabled));        \
        __branch;                                                \
    })

/* Initialize jump label subsystem */
void jump_label_init(void);

#endif /* JUMP_LABEL_H */
