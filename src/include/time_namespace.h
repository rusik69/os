#ifndef TIME_NAMESPACE_H
#define TIME_NAMESPACE_H

#include "types.h"

int time_ns_create(void);
int time_ns_destroy(int ns_id);
int time_ns_set_offsets(int ns_id, int64_t monotonic_offset_ns,
                        int64_t boottime_offset_ns);
int time_ns_get_offsets(int ns_id, int64_t *monotonic_offset_ns,
                        int64_t *boottime_offset_ns);
uint64_t time_ns_clock_gettime_ns(int ns_id, int clock_id);
void time_ns_init(void);

#endif /* TIME_NAMESPACE_H */
