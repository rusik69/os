#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "timerfd.h"
#include "string.h"
void timerfd_init(void) {
    kprintf("[OK] timerfd subsystem initialized\n");
}
int timerfd_create(int clockid) {
    (void)clockid;
    return 43; /* fake fd */
}
