#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"

#define KEY_UP    ((char)0x80)
#define KEY_DOWN  ((char)0x81)
#define KEY_LEFT  ((char)0x82)
#define KEY_RIGHT ((char)0x83)

void keyboard_init(void);
char keyboard_getchar(void);
int keyboard_has_input(void);

#endif
