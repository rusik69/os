#include "stacktrace.h"
#include "printf.h"
#include "string.h"
#define MAX_STACK_FRAMES 64
int save_stack_trace(uint64_t *entries, int max_entries) {
    int count = 0;
    uint64_t *rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    while (rbp && count < max_entries && count < MAX_STACK_FRAMES) {
        uint64_t ret_addr = rbp[1];
        if (ret_addr < 0xFFFF800000000000ULL) break;
        entries[count++] = ret_addr;
        rbp = (uint64_t *)rbp[0];
    }
    return count;
}
void print_stack_trace(void) {
    uint64_t entries[MAX_STACK_FRAMES];
    int n = save_stack_trace(entries, MAX_STACK_FRAMES);
    kprintf("Stack trace (%d frames):\n", n);
    for (int i = 0; i < n; i++)
        kprintf("  [<%016llx>]\n", entries[i]);
}
