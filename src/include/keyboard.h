#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"

#define KEY_UP    ((char)0x80)
#define KEY_DOWN  ((char)0x81)
#define KEY_LEFT  ((char)0x82)
#define KEY_RIGHT ((char)0x83)
#define KEY_PAGEUP   ((char)0x84)
#define KEY_PAGEDOWN ((char)0x85)

void keyboard_init(void);
char keyboard_getchar(void);
int keyboard_has_input(void);
int keyboard_is_down(char c);
int keyboard_escape_down(void);
void keyboard_reset_state(void);

/* ── Keyboard layout support ───────────────────────────────────────── */

#define KB_LAYOUT_US 0
#define KB_LAYOUT_UK 1

/* Set the keyboard layout (KB_LAYOUT_US or KB_LAYOUT_UK).
   Returns the previous layout. */
int keyboard_set_layout(int layout);

/* Get the current keyboard layout. */
int keyboard_get_layout(void);

/* ── Keyboard LED control ──────────────────────────────────────────── */

/* LED bit masks (for the 0xED command byte) */
#define KB_LED_SCROLL_LOCK  (1U << 0)
#define KB_LED_NUM_LOCK     (1U << 1)
#define KB_LED_CAPS_LOCK    (1U << 2)

/* Set keyboard LEDs to the given mask (KB_LED_SCROLL_LOCK | KB_LED_NUM_LOCK | KB_LED_CAPS_LOCK).
   Returns 0 on success, -1 on error. */
int keyboard_set_leds(uint8_t leds);

/* Get the current keyboard LED state. */
uint8_t keyboard_get_leds(void);

/* ── SysRq (Magic System Request) support ──────────────────────────── */

/* Register a callback that will be invoked when Alt+SysRq+<key> is
 * pressed on the keyboard.  The callback receives the ASCII character
 * of the command key.  Pass NULL to unregister. */
typedef void (*sysrq_callback_t)(char cmd);
void keyboard_set_sysrq_callback(sysrq_callback_t cb);

#endif
