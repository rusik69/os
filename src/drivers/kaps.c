#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "kaps.h"
static uint64_t caps_effective = 0xFFFFFFFFFFFFFFFFULL;
static __attribute__((unused)) uint64_t caps_permitted = 0xFFFFFFFFFFFFFFFFULL;
static __attribute__((unused)) uint64_t caps_inheritable = 0;
static uint64_t caps_bounding = 0xFFFFFFFFFFFFFFFFULL;
void caps_init(void) {
    kprintf("[OK] Capability system initialized\n");
}
int caps_capable(uint64_t cap) {
    return (caps_effective & (1ULL << cap)) ? 1 : 0;
}
int caps_set_effective(uint64_t cap, int set) {
    if (cap >= 64) return -1;
    if (set) caps_effective |= (1ULL << cap);
    else caps_effective &= ~(1ULL << cap);
    return 0;
}
int caps_set_bounding(uint64_t cap, int drop) {
    if (cap >= 64) return -1;
    if (drop) caps_bounding &= ~(1ULL << cap);
    return 0;
}
