#ifndef MOUSE_H
#define MOUSE_H

#include "types.h"

void mouse_init(void);
void mouse_get_pos(int *x, int *y);
void mouse_get_pixel_pos(int *x, int *y);
uint8_t mouse_get_buttons(void);

#endif
