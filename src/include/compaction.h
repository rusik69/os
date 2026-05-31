#ifndef COMPACTION_H
#define COMPACTION_H

#include "types.h"

/* Memory compaction — defragments physical memory.
 * Reports fragmentation level and attempts to defragment when needed. */

void compaction_init(void);
uint64_t compaction_fragmentation_pct(void);
uint64_t compaction_run(void);

#endif
