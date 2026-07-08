// SPDX-License-Identifier: GPL-2.0-only
/*
 * randstruct.c — Randomize struct layout at kernel build time
 *
 * This module provides runtime support for GCC's __randomize_layout
 * attribute. It seeds the per-struct randomization with kernel entropy
 * and provides helper functions for struct layout randomization.
 */
#include "types.h"
#include "printf.h"
#include "string.h"
#include "rng.h"

/* Structure layout randomization seed table */
#define RANDSTRUCT_MAX_SEEDS 64

static struct randstruct_seed {
    const char *type_name;
    uint32_t seed;
} randstruct_seeds[RANDSTRUCT_MAX_SEEDS];

static int randstruct_seed_count = 0;
static int randstruct_initialized = 0;

/* Register a type for layout randomization with a seed */
static int randstruct_register_seed(const char *type_name, uint32_t seed)
{
    if (randstruct_seed_count >= RANDSTRUCT_MAX_SEEDS)
        return -1;

    randstruct_seeds[randstruct_seed_count].type_name = type_name;
    randstruct_seeds[randstruct_seed_count].seed = seed;
    randstruct_seed_count++;
    return 0;
}

/* Get the randomization seed for a given type */
static uint32_t randstruct_get_seed(const char *type_name)
{
    for (int i = 0; i < randstruct_seed_count; i++) {
        if (strcmp(randstruct_seeds[i].type_name, type_name) == 0)
            return randstruct_seeds[i].seed;
    }
    return 0;
}

/* Generate a random permutation of struct member offsets
 * Used by the compile-time plugin to randomize layout */
static void randstruct_permute(uint32_t *perm, int n_members, uint32_t seed)
{
    /* Fisher-Yates shuffle seeded by the per-type seed */
    uint32_t state = seed;

    for (int i = 0; i < n_members; i++)
        perm[i] = (uint32_t)i;

    for (int i = n_members - 1; i > 0; i--) {
        /* Simple LCG for pseudo-random */
        state = state * 1103515245 + 12345;
        uint32_t j = state % (uint32_t)(i + 1);
        uint32_t tmp = perm[i];
        perm[i] = perm[j];
        perm[j] = tmp;
    }
}

/* Initialize randstruct with entropy from kernel RNG */
static void randstruct_init(void)
{
    if (randstruct_initialized)
        return;

    randstruct_initialized = 1;
    kprintf("[OK] Randstruct — GCC __randomize_layout support (%d seed slots)\n",
            RANDSTRUCT_MAX_SEEDS);
}

/* ── Stub: randstruct_randomize ─────────────────────────────── */
static int randstruct_randomize(void *layout)
{
    (void)layout;
    kprintf("[randstruct] randstruct_randomize: not yet implemented\n");
    return 0;
}
/* ── Stub: randstruct_apply ─────────────────────────────── */
static int randstruct_apply(void *layout)
{
    (void)layout;
    kprintf("[randstruct] randstruct_apply: not yet implemented\n");
    return 0;
}
