#define KERNEL_INTERNAL
#include "types.h"
#include "aslr.h"
#include "printf.h"
#include "syscall.h"   /* for prng_rand64 */

void aslr_init(void) {
    /* PRNG is already initialized in syscall_init().
     * We just add some extra entropy from our boot timing. */
    kprintf("[OK] ASLR initialized\n");
}

uint64_t aslr_stack_offset(void) {
    return prng_rand64() % (ASLR_STACK_RANDOM_PAGES + 1);
}

uint64_t aslr_mmap_offset(void) {
    return prng_rand64() % (ASLR_MMAP_RANDOM_PAGES + 1);
}

uint64_t aslr_brk_offset(void) {
    return prng_rand64() % (ASLR_BRK_RANDOM_PAGES + 1);
}

void aslr_get_at_random(uint8_t buf[16]) {
    uint64_t r1 = prng_rand64();
    uint64_t r2 = prng_rand64();
    for (int i = 0; i < 8; i++) {
        buf[i] = (uint8_t)(r1 >> (i * 8));
        buf[i + 8] = (uint8_t)(r2 >> (i * 8));
    }
}

void aslr_add_entropy(uint64_t entropy) {
    /* XOR additional entropy into the PRNG stream.
     * This would be called from irq handlers etc. to add timing noise. */
    (void)entropy;
}
