#ifndef STACK_GUARD_H
#define STACK_GUARD_H

#include "types.h"

/* Setup a guard page below a kernel stack.
 * stack_virt: virtual address of the stack top (highest address).
 * stack_pages: number of stack pages (excluding guard page).
 * The guard page is placed at the bottom of the stack region.
 * Returns 0 on success, negative on error. */
int stack_guard_setup(uint64_t stack_virt, int stack_pages);

/* Check if a fault address is a stack guard page violation.
 * Returns 1 if the address falls in a guard page, 0 otherwise. */
int stack_guard_check(uint64_t fault_addr);

/* Remove the guard page for a given stack (restore mapping).
 * Returns 0 on success. */
int stack_guard_remove(uint64_t stack_virt, int stack_pages);

/* Initialize stack guard subsystem. */
void stack_guard_init(void);

#endif /* STACK_GUARD_H */
