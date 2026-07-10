#ifndef BOCHS_H
#define BOCHS_H
#include "types.h"

void bochs_init(void);
int  bochs_is_present(void);
int  bochs_get_width(void);
int  bochs_get_height(void);
int  bochs_set_mode(int w, int h, int bpp);

/* Framebuffer access */
volatile uint8_t *bochs_get_fb_base(void);
uint64_t          bochs_get_fb_phys(void);
uint32_t          bochs_get_fb_size(void);

#endif
