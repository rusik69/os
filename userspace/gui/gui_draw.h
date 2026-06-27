#ifndef GUI_DRAW_H
#define GUI_DRAW_H

#include "gui.h"

/* ===== Extended Drawing Primitives ===== */

/* Draw a line (Bresenham) between (x1,y1) and (x2,y2) */
void gui_draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, gui_color_t color);

/* Draw a circle outline (midpoint algorithm) */
void gui_draw_circle(int32_t cx, int32_t cy, int32_t r, gui_color_t color);

/* Draw a filled circle (scan-line fill) */
void gui_draw_circle_filled(int32_t cx, int32_t cy, int32_t r, gui_color_t color);

/* Draw a rectangle with rounded corners */
void gui_draw_rounded_rect(gui_rect_t rect, int radius, gui_color_t color);

/* Draw a vertical gradient fill */
void gui_draw_gradient_v(int32_t x, int32_t y, uint32_t w, uint32_t h,
                          gui_color_t top, gui_color_t bottom);

/* Draw a horizontal gradient fill */
void gui_draw_gradient_h(int32_t x, int32_t y, uint32_t w, uint32_t h,
                          gui_color_t left, gui_color_t right);

/* Draw a triangle outline */
void gui_draw_triangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                        int32_t x3, int32_t y3, gui_color_t color);

/* Draw a filled triangle (scan-line) */
void gui_draw_triangle_filled(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                               int32_t x3, int32_t y3, gui_color_t color);

/* Draw a progress/loading bar */
void gui_draw_progress_bar(int32_t x, int32_t y, uint32_t w, uint32_t h,
                            int percent, gui_color_t fg, gui_color_t bg);

/* Draw a raw RGB/RGBA image (simple pixel buffer) */
void gui_draw_image_raw(int32_t x, int32_t y, uint32_t w, uint32_t h,
                         const gui_color_t *pixels);

/* Draw a checkerboard pattern (for alpha/transparency display) */
void gui_draw_checkerboard(int32_t x, int32_t y, uint32_t w, uint32_t h, int cell_size);

/* Draw a star shape */
void gui_draw_star(int32_t cx, int32_t cy, int32_t outer_r, int32_t inner_r, gui_color_t color);

/* ===== Color utilities ===== */
gui_color_t gui_color_lerp(gui_color_t a, gui_color_t b, int t, int max_t);
gui_color_t gui_color_darken(gui_color_t c, int amount);
gui_color_t gui_color_lighten(gui_color_t c, int amount);

/* ===== New Concrete Widgets ===== */

/* ── Checkbox ── */
typedef struct {
    char label[64];
    int checked;
    void (*on_change)(gui_widget_t *w, int checked);
} gui_checkbox_data_t;

gui_widget_t* gui_checkbox_create(gui_rect_t rect, const char *label, int checked);
int gui_checkbox_is_checked(gui_widget_t *cb);
void gui_checkbox_set_checked(gui_widget_t *cb, int checked);
void gui_checkbox_set_on_change(gui_widget_t *cb, void (*fn)(gui_widget_t *, int));

/* ── Horizontal Slider ── */
typedef struct {
    int min_val, max_val, value;
    void (*on_change)(gui_widget_t *w, int value);
} gui_slider_data_t;

gui_widget_t* gui_slider_create(gui_rect_t rect, int min_val, int max_val, int initial);
int gui_slider_get_value(gui_widget_t *sl);
void gui_slider_set_value(gui_widget_t *sl, int value);
void gui_slider_set_on_change(gui_widget_t *sl, void (*fn)(gui_widget_t *, int));

/* ── Listbox ── */
typedef struct {
    char items[64][64];
    int num_items;
    int selected;
    int scroll_offset;
    void (*on_select)(gui_widget_t *w, int index, const char *item);
} gui_listbox_data_t;

gui_widget_t* gui_listbox_create(gui_rect_t rect);
void gui_listbox_add_item(gui_widget_t *lb, const char *item);
void gui_listbox_clear(gui_widget_t *lb);
int gui_listbox_get_selected(gui_widget_t *lb);
const char* gui_listbox_get_selected_text(gui_widget_t *lb);
void gui_listbox_set_on_select(gui_widget_t *lb, void (*fn)(gui_widget_t *, int, const char *));

#endif /* GUI_DRAW_H */
