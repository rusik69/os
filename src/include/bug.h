#ifndef BUG_H
#define BUG_H
#include "printf.h"
#define BUG() do { kprintf("BUG at %s:%d\n", __FILE__, __LINE__); for(;;); } while(0)
#define BUG_ON(c) do { if (__builtin_expect(!!(c), 0)) { kprintf("BUG_ON at %s:%d\n", __FILE__, __LINE__); for(;;); } } while(0)
#define WARN_ON(c) ({ int _c = !!(c); if (_c) kprintf("WARNING at %s:%d\n", __FILE__, __LINE__); _c; })
#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))
#endif
