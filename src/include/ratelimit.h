#ifndef RATELIMIT_H
#define RATELIMIT_H
#include "types.h"
struct ratelimit_state {
    uint64_t begin;
    int interval;
    int burst;
    int printed;
};
int __ratelimit(struct ratelimit_state *rs);
#define ratelimit(rs) __ratelimit(rs)
#endif
