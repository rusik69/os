#define KERNEL_INTERNAL
#include "kaps.h"
#include "printf.h"
#include "types.h"
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

/* ── Stub: kaps_init ─────────────────────────────── */
static int kaps_init(void)
{
    kprintf("[kaps] kaps_init: not yet implemented\n");
    return 0;
}
/* ── Stub: kaps_set_key ─────────────────────────────── */
static int kaps_set_key(const void *key, size_t len)
{
    (void)key;
    (void)len;
    kprintf("[kaps] kaps_set_key: not yet implemented\n");
    return 0;
}
/* ── Stub: kaps_encrypt ─────────────────────────────── */
static int kaps_encrypt(const void *plain, size_t plen, void *cipher, size_t *clen)
{
    (void)plain;
    (void)plen;
    (void)cipher;
    (void)clen;
    kprintf("[kaps] kaps_encrypt: not yet implemented\n");
    return 0;
}
/* ── Stub: kaps_decrypt ─────────────────────────────── */
static int kaps_decrypt(const void *cipher, size_t clen, void *plain, size_t *plen)
{
    (void)cipher;
    (void)clen;
    (void)plain;
    (void)plen;
    kprintf("[kaps] kaps_decrypt: not yet implemented\n");
    return 0;
}
