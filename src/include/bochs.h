#ifndef BOCHS_H
#define BOCHS_H
#include "types.h"
void bochs_init(void);
int bochs_is_present(void);
int bochs_get_width(void);
int bochs_get_height(void);
void bochs_set_mode(int w, int h, int bpp);
#endif
