#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "signalfd.h"
#include "string.h"
void signalfd_init(void) {
    kprintf("[OK] signalfd subsystem initialized\n");
}
int signalfd_create(uint64_t mask) {
    (void)mask;
    kprintf("[signalfd] create mask=0x%llx\n", (unsigned long long)mask);
    return 42; /* fake fd */
}
