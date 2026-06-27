#ifndef GUI_DRAW_H
#define GUI_DRAW_H

#include "gui.h"

/* ===== Core Drawing Primitives ===== */

/* Draw a line (Bresenham) between (x1,y1) and (x2,y2) */
void gui_draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, gui_color_t color);

/* Draw a circle outline (midpoint algorithm) */
void gui_draw_circle(int32_t cx, int32_t cy, int32_t r, gui_color_t color);

/* Draw a filled circle (scan-line fill) */
void gui_draw_circle_filled(int32_t cx, int32_t cy, int32_t r, gui_color_t color);

/* Draw a rectangle with rounded corners */
void gui_draw_rounded_rect(gui_rect_t rect, int radius, gui_color_t color);

/* Draw a filled rounded rectangle */
void gui_draw_rounded_rect_filled(gui_rect_t rect, int radius, gui_color_t color);

/* Draw a vertical gradient fill */
void gui_draw_gradient_v(int32_t x, int32_t y, uint32_t w, uint32_t h,
                          gui_color_t top, gui_color_t bottom);

/* Draw a horizontal gradient fill */
void gui_draw_gradient_h(int32_t x, int32_t y, uint32_t w, uint32_t h,
                          gui_color_t left, gui_color_t right);

/* Draw a radial gradient fill */
void gui_draw_gradient_radial(int32_t cx, int32_t cy, int32_t r,
                               gui_color_t inner, gui_color_t outer);

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

/* ===== Extended Drawing Primitives (100 improvements) ===== */

/* Ellipse outline (midpoint algorithm) */
void gui_draw_ellipse(int32_t cx, int32_t cy, int32_t rx, int32_t ry, gui_color_t color);

/* Filled ellipse */
void gui_draw_ellipse_filled(int32_t cx, int32_t cy, int32_t rx, int32_t ry, gui_color_t color);

/* Arc outline (start_angle, end_angle in degrees, 0=right, CCW) */
void gui_draw_arc(int32_t cx, int32_t cy, int32_t r, int start_deg, int end_deg, gui_color_t color);

/* Filled arc / pie slice */
void gui_draw_pie(int32_t cx, int32_t cy, int32_t r, int start_deg, int end_deg, gui_color_t color);

/* Cubic Bezier curve */
void gui_draw_bezier_cubic(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                            int32_t x2, int32_t y2, int32_t x3, int32_t y3, gui_color_t color);

/* Quadratic Bezier curve */
void gui_draw_bezier_quad(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                           int32_t x2, int32_t y2, gui_color_t color);

/* Polyline (connect an array of points) */
void gui_draw_polyline(const gui_point_t *pts, int n, gui_color_t color);

/* Polygon outline */
void gui_draw_polygon(const gui_point_t *pts, int n, gui_color_t color);

/* Filled polygon (scan-line fill) */
void gui_draw_polygon_filled(const gui_point_t *pts, int n, gui_color_t color);

/* Dashed line */
void gui_draw_dashed_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                           gui_color_t color, int dash_len, int gap_len);

/* Thick line with width */
void gui_draw_thick_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                          int thickness, gui_color_t color);

/* Arrow line (line with arrowhead at end) */
void gui_draw_arrow_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                          gui_color_t color, int arrow_size);

/* Simple arrow shape */
void gui_draw_arrow(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int head_w, int head_h, gui_color_t color);

/* 3D raised or sunken frame */
void gui_draw_frame_3d(gui_rect_t rect, int raised, int thickness);

/* Dashed rectangle outline */
void gui_draw_rect_dashed(gui_rect_t rect, gui_color_t color, int dash_len);

/* Grid of lines */
void gui_draw_grid(int32_t x, int32_t y, uint32_t w, uint32_t h,
                   int cell_w, int cell_h, gui_color_t color);

/* Cross/plus mark */
void gui_draw_cross(int32_t cx, int32_t cy, int size, gui_color_t color);

/* Drop shadow for rectangles */
void gui_draw_rect_shadow(gui_rect_t rect, int offset, gui_color_t shadow_color);

/* Heart shape */
void gui_draw_heart(int32_t cx, int32_t cy, int size, gui_color_t color);

/* Thick circle (donut) outline */
void gui_draw_donut(int32_t cx, int32_t cy, int32_t r_outer, int32_t r_inner, gui_color_t color);

/* Diamond shape */
void gui_draw_diamond(int32_t cx, int32_t cy, int32_t rx, int32_t ry, gui_color_t color);

/* Crosshair with circle */
void gui_draw_crosshair(int32_t cx, int32_t cy, int r, gui_color_t color);

/* ===== Color Utilities ===== */
gui_color_t gui_color_lerp(gui_color_t a, gui_color_t b, int t, int max_t);
gui_color_t gui_color_darken(gui_color_t c, int amount);
gui_color_t gui_color_lighten(gui_color_t c, int amount);
gui_color_t gui_color_invert(gui_color_t c);
gui_color_t gui_color_blend(gui_color_t fg, gui_color_t bg, int alpha);
gui_color_t gui_color_to_greyscale(gui_color_t c);
gui_color_t gui_color_random(void);
void gui_color_get_rgba(gui_color_t c, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a);
gui_color_t gui_color_from_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
int gui_color_is_dark(gui_color_t c);
int gui_color_is_light(gui_color_t c);
uint32_t gui_color_distance(gui_color_t a, gui_color_t b);
void gui_color_to_hsv(gui_color_t c, int *h, int *s, int *v);
gui_color_t gui_color_from_hsv(int h, int s, int v);
gui_color_t gui_color_contrast(gui_color_t c);

/* ===== New Widget Declarations (implemented in gui_draw.c) ===== */

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

/* ── Slider (horizontal) ── */
typedef struct {
    int min_val, max_val, value;
    void (*on_change)(gui_widget_t *w, int value);
} gui_slider_data_t;

gui_widget_t* gui_slider_create(gui_rect_t rect, int min_val, int max_val, int initial);
int gui_slider_get_value(gui_widget_t *sl);
void gui_slider_set_value(gui_widget_t *sl, int value);
void gui_slider_set_on_change(gui_widget_t *sl, void (*fn)(gui_widget_t *, int));

/* ── Spinbox — numeric up/down spinner ── */
typedef struct {
    int value, min_val, max_val, step;
    void (*on_change)(gui_widget_t *w, int value);
} gui_spinbox_data_t;

gui_widget_t* gui_spinbox_create(gui_rect_t rect, int min_val, int max_val, int initial, int step);
int gui_spinbox_get_value(gui_widget_t *sp);
void gui_spinbox_set_value(gui_widget_t *sp, int val);
void gui_spinbox_set_on_change(gui_widget_t *sp, void (*fn)(gui_widget_t *, int));

/* Toggle switch */
typedef struct {
    int on;
    void (*on_change)(gui_widget_t *w, int on);
} gui_toggle_data_t;

gui_widget_t* gui_toggle_create(gui_rect_t rect, int initial);
int gui_toggle_is_on(gui_widget_t *tg);
void gui_toggle_set_on(gui_widget_t *tg, int on);
void gui_toggle_set_on_change(gui_widget_t *tg, void (*fn)(gui_widget_t *, int));

/* Dropdown list */
typedef struct {
    char items[32][64];
    int num_items;
    int selected;
    int open;
    void (*on_select)(gui_widget_t *w, int index, const char *item);
} gui_dropdown_data_t;

gui_widget_t* gui_dropdown_create(gui_rect_t rect);
void gui_dropdown_add_item(gui_widget_t *dd, const char *item);
void gui_dropdown_clear(gui_widget_t *dd);
int gui_dropdown_get_selected(gui_widget_t *dd);
const char* gui_dropdown_get_selected_text(gui_widget_t *dd);
void gui_dropdown_set_on_select(gui_widget_t *dd, void (*fn)(gui_widget_t *, int, const char *));

/* Scrollbar (vertical) */
typedef struct {
    int content_size, view_size, scroll_pos;
    void (*on_scroll)(gui_widget_t *w, int pos);
} gui_scrollbar_data_t;

gui_widget_t* gui_scrollbar_create(gui_rect_t rect, int content_size, int view_size);
void gui_scrollbar_set_range(gui_widget_t *sb, int content_size, int view_size);
int gui_scrollbar_get_pos(gui_widget_t *sb);
void gui_scrollbar_set_pos(gui_widget_t *sb, int pos);
void gui_scrollbar_set_on_scroll(gui_widget_t *sb, void (*fn)(gui_widget_t *, int));

/* Progress bar widget (indeterminate) */
typedef struct {
    int percent;
    gui_color_t fg, bg;
} gui_progress_data_t;

gui_widget_t* gui_progress_create(gui_rect_t rect, gui_color_t fg, gui_color_t bg);
void gui_progress_set_percent(gui_widget_t *pw, int pct);

/* Radio button */
typedef struct {
    char label[64];
    int selected;
    struct gui_radiogroup *group;
    void (*on_select)(gui_widget_t *w);
} gui_radio_data_t;

typedef struct gui_radiogroup {
    gui_widget_t *buttons[16];
    int count;
    int selected;
} gui_radiogroup_t;

gui_widget_t* gui_radio_create(gui_rect_t rect, const char *label, gui_radiogroup_t *grp);
gui_radiogroup_t* gui_radiogroup_create(void);
void gui_radiogroup_add(gui_radiogroup_t *grp, gui_widget_t *rb);
int gui_radio_is_selected(gui_widget_t *rb);

/* Separator line */
gui_widget_t* gui_separator_create(gui_rect_t rect, int horizontal);

/* Panel (container with border) */
gui_widget_t* gui_panel_create(gui_rect_t rect, gui_color_t bg);

/* Group box with title */
typedef struct {
    char title[64];
    gui_color_t bg;
} gui_groupbox_data_t;

gui_widget_t* gui_groupbox_create(gui_rect_t rect, const char *title, gui_color_t bg);

#endif /* GUI_DRAW_H */
