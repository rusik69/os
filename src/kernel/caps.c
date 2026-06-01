#define KERNEL_INTERNAL
#include "caps.h"
#include "process.h"
#include "string.h"
#include "printf.h"

/* Capability bounding set — limits what capabilities a process can ever gain */
static uint64_t cap_bset[CAP_BSET_SIZE];

void cap_bset_init(void) {
    memset(cap_bset, 0, sizeof(cap_bset));
    /* By default, allow all capabilities */
    for (int i = 0; i < CAP_BSET_SIZE; i++)
        cap_bset[i] = ~0ULL;
    kprintf("[OK] cap_bset initialized\n");
}

void cap_bset_drop(uint32_t cap) {
    if (cap > CAP_LAST_CAP) return;
    int word = cap / 64;
    int bit = cap % 64;
    if (word < CAP_BSET_SIZE) {
        cap_bset[word] &= ~(1ULL << bit);
    }
}

int cap_bset_has(uint32_t cap) {
    if (cap > CAP_LAST_CAP) return 0;
    int word = cap / 64;
    int bit = cap % 64;
    if (word >= CAP_BSET_SIZE) return 0;
    return (cap_bset[word] >> bit) & 1;
}
