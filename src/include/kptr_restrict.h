#ifndef KPTR_RESTRICT_H
#define KPTR_RESTRICT_H

#include "types.h"

/* kptr_restrict values */
#define KPTR_RESTRICT_DISABLED      0   /* show all kernel pointers */
#define KPTR_RESTRICT_RESTRICTED    1   /* hide from non-root */
#define KPTR_RESTRICT_ROOT_HIDE     2   /* hide from everyone (including root) */

/* Kernel pointer restrict variable */
extern int kptr_restrict;

/* ── API ────────────────────────────────────────────────────────── */

void kptr_restrict_init(void);
int  kptr_restrict_check(void);  /* returns 1 if %pK should be hidden */

#endif /* KPTR_RESTRICT_H */
