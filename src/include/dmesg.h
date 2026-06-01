#ifndef DMESG_H
#define DMESG_H

#include "types.h"

/* dmesg_restrict variable */
extern int dmesg_restrict;

/* ── API ────────────────────────────────────────────────────────── */

void dmesg_init(void);
int  dmesg_check_access(void);  /* returns 1 if caller can read dmesg */

#endif /* DMESG_H */
