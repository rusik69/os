#ifndef FBCON_H
#define FBCON_H

#include "types.h"

/* Framebuffer console dimensions */
#define FBCON_COLS  80
#define FBCON_ROWS  25
#define FBCON_SCROLLBACK_LINES 400

/* Font glyph dimensions */
#define FONT_WIDTH  8
#define FONT_HEIGHT 16

/* Default colours (VGA palette indices) */
#define FBCON_BLACK   0
#define FBCON_BLUE    1
#define FBCON_GREEN   2
#define FBCON_CYAN    3
#define FBCON_RED     4
#define FBCON_MAGENTA 5
#define FBCON_BROWN   6
#define FBCON_LIGHT_GREY  7
#define FBCON_DARK_GREY   8
#define FBCON_LIGHT_BLUE  9
#define FBCON_LIGHT_GREEN 10
#define FBCON_LIGHT_CYAN  11
#define FBCON_LIGHT_RED   12
#define FBCON_LIGHT_MAGENTA 13
#define FBCON_YELLOW      14
#define FBCON_WHITE       15

/* Character cell in the console buffer */
struct fbcon_cell {
    uint8_t ch;
    uint8_t fg;
    uint8_t bg;
};

/* Initialise the framebuffer console.
   fb: pointer to linear framebuffer (BGRX 32bpp).
   w, h: visible resolution in pixels.
   pitch: bytes per scan line (may be > w*4 for alignment). */
void fbcon_init(uint32_t *fb, uint32_t w, uint32_t h, uint32_t pitch);

/* Write a single character to the current cursor position. */
void fbcon_putchar(char c);

/* Write a null-terminated string. */
void fbcon_write(const char *s);

/* Clear the console and home the cursor. */
void fbcon_clear(void);

/* Set foreground and background colours (VGA palette indices). */
void fbcon_set_fg(uint8_t fg);
void fbcon_set_bg(uint8_t bg);

/* Scroll the display by a signed number of lines (+ = forward, - = backward).
   Uses the scrollback buffer when scrolling backward. */
void fbcon_scroll(int lines);

/* Return to live (most recent) view after scrolling backward. */
void fbcon_scroll_home(void);

/* Set cursor position (column, row, 0-based). */
void fbcon_set_cursor(int col, int row);

/* Force a full redraw of the entire console. */
void fbcon_redraw(void);

#endif /* FBCON_H */
