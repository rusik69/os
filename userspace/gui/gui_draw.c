#include "gui_draw.h"
#include "gui.h"
#include "string.h"
#include "stdlib.h"

/* ===== Integer math helpers ===== */
static int __abs(int x) { return x < 0 ? -x : x; }
static int __min(int a, int b) { return a < b ? a : b; }
static int __max(int a, int b) { return a > b ? a : b; }
static void __swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }

static void __put(int32_t x, int32_t y, gui_color_t c) { vga_put_pixel(x, y, c); }

/* Integer sqrt (Newton) */
static int __isqrt(int n) {
    if (n <= 0) return 0;
    int x = n;
    int y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

/* ===== Line (Bresenham) ===== */
void gui_draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, gui_color_t color) {
    int dx = __abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -__abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy, e2;
    while (1) {
        __put(x1, y1, color);
        if (x1 == x2 && y1 == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

/* ===== Circle (Midpoint) ===== */
void gui_draw_circle(int32_t cx, int32_t cy, int32_t r, gui_color_t color) {
    int x = 0, y = r, d = 1 - r;
    while (x <= y) {
        __put(cx + x, cy + y, color); __put(cx + y, cy + x, color);
        __put(cx - x, cy + y, color); __put(cx - y, cy + x, color);
        __put(cx + x, cy - y, color); __put(cx + y, cy - x, color);
        __put(cx - x, cy - y, color); __put(cx - y, cy - x, color);
        x++;
        if (d < 0) d += 2 * x + 1;
        else { y--; d += 2 * (x - y) + 1; }
    }
}

void gui_draw_circle_filled(int32_t cx, int32_t cy, int32_t r, gui_color_t color) {
    for (int y = -r; y <= r; y++) {
        int h = __isqrt(r * r - y * y);
        for (int x = -h; x <= h; x++) __put(cx + x, cy + y, color);
    }
}

/* ===== Rounded Rect ===== */
void gui_draw_rounded_rect(gui_rect_t rect, int radius, gui_color_t color) {
    int32_t x = rect.x, y = rect.y, w = (int32_t)rect.w, h = (int32_t)rect.h;
    int r = radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int32_t xi = x + r; xi <= x + w - r - 1; xi++) __put(xi, y, color);
    for (int32_t xi = x + r; xi <= x + w - r - 1; xi++) __put(xi, y + h - 1, color);
    for (int32_t yi = y + r; yi <= y + h - r - 1; yi++) __put(x, yi, color);
    for (int32_t yi = y + r; yi <= y + h - r - 1; yi++) __put(x + w - 1, yi, color);
    /* Draw quarter circles at corners */
    int cx1 = x + r, cy1 = y + r, cx2 = x + w - 1 - r, cy2 = y + h - 1 - r;
    int rx = 0, ry = r, dd = 1 - r;
    while (rx <= ry) {
        __put(cx1 + rx, cy1 + ry, color); __put(cx2 - rx, cy1 + ry, color);
        __put(cx1 + rx, cy2 - ry, color); __put(cx2 - rx, cy2 - ry, color);
        __put(cx1 + ry, cy1 + rx, color); __put(cx2 - ry, cy1 + rx, color);
        __put(cx1 + ry, cy2 - rx, color); __put(cx2 - ry, cy2 - rx, color);
        rx++;
        if (dd < 0) dd += 2 * rx + 1;
        else { ry--; dd += 2 * (rx - ry) + 1; }
    }
}

/* ===== Gradients ===== */
static gui_color_t __lerp_color(gui_color_t a, gui_color_t b, int num, int den) {
    if (den <= 0) return a;
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = (ar * (den - num) + br * num) / den;
    int g = (ag * (den - num) + bg * num) / den;
    int bv = (ab * (den - num) + bb * num) / den;
    return GUI_COLOR(r, g, bv);
}

void gui_draw_gradient_v(int32_t x, int32_t y, uint32_t w, uint32_t h,
                          gui_color_t top, gui_color_t bottom) {
    for (uint32_t yi = 0; yi < h; yi++) {
        gui_color_t c = __lerp_color(top, bottom, (int)yi, (int)h);
        for (uint32_t xi = 0; xi < w; xi++) __put(x + (int32_t)xi, y + (int32_t)yi, c);
    }
}

void gui_draw_gradient_h(int32_t x, int32_t y, uint32_t w, uint32_t h,
                          gui_color_t left, gui_color_t right) {
    for (uint32_t xi = 0; xi < w; xi++) {
        gui_color_t c = __lerp_color(left, right, (int)xi, (int)w);
        for (uint32_t yi = 0; yi < h; yi++) __put(x + (int32_t)xi, y + (int32_t)yi, c);
    }
}

/* ===== Triangle ===== */
void gui_draw_triangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                        int32_t x3, int32_t y3, gui_color_t color) {
    gui_draw_line(x1, y1, x2, y2, color);
    gui_draw_line(x2, y2, x3, y3, color);
    gui_draw_line(x3, y3, x1, y1, color);
}

void gui_draw_triangle_filled(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                               int32_t x3, int32_t y3, gui_color_t color) {
    /* Sort by y */
    if (y1 > y2) { __swap(&y1, &y2); __swap(&x1, &x2); }
    if (y2 > y3) { __swap(&y2, &y3); __swap(&x2, &x3); }
    if (y1 > y2) { __swap(&y1, &y2); __swap(&x1, &x2); }
    if (y1 >= y3) return;
    for (int32_t sy = y1; sy <= y3; sy++) {
        float t1 = (y3 != y1) ? (float)(sy - y1) / (float)(y3 - y1) : 0;
        float t2 = (y2 != y1) ? (float)(sy - y1) / (float)(y2 - y1) : 0;
        t2 = (t2 > 1.0f) ? 1.0f : t2;
        int xa = x1 + (int)((float)(x3 - x1) * t1);
        int xb = (sy < y2) ? x1 + (int)((float)(x2 - x1) * t2) : x2 + (int)((float)(x3 - x2) * (t1 - (float)(y2 - y1)/(float)(y3 - y1)) / (1.0f - (float)(y2 - y1)/(float)(y3 - y1)));
        /* Simpler: two-part fill */
        int xl = xa < xb ? xa : xb;
        int xr = xa < xb ? xb : xa;
        for (int sx = xl; sx <= xr; sx++) __put(sx, sy, color);
    }
}

/* ===== Progress Bar ===== */
void gui_draw_progress_bar(int32_t x, int32_t y, uint32_t w, uint32_t h,
                            int percent, gui_color_t fg, gui_color_t bg) {
    gui_rect_t r = {x, y, w, h};
    gui_window_draw_rect(NULL, r, bg);
    gui_window_draw_rect_outline(NULL, r, GUI_DARK_GRAY, 1);
    if (percent > 0) {
        int fw = (int)((int32_t)w - 2) * percent / 100;
        if (fw < 0) fw = 0;
        if (fw > (int32_t)w - 2) fw = (int32_t)w - 2;
        gui_rect_t fr = {x + 1, y + 1, (uint32_t)fw, h - 2};
        gui_window_draw_rect(NULL, fr, fg);
    }
}

/* ===== Image ===== */
void gui_draw_image_raw(int32_t x, int32_t y, uint32_t w, uint32_t h,
                         const gui_color_t *pixels) {
    for (uint32_t yi = 0; yi < h; yi++)
        for (uint32_t xi = 0; xi < w; xi++)
            __put(x + (int32_t)xi, y + (int32_t)yi, pixels[yi * w + xi]);
}

/* ===== Checkerboard ===== */
void gui_draw_checkerboard(int32_t x, int32_t y, uint32_t w, uint32_t h, int cell_size) {
    if (cell_size <= 0) cell_size = 8;
    for (uint32_t yi = 0; yi < h; yi++)
        for (uint32_t xi = 0; xi < w; xi++) {
            int cx = ((int32_t)xi / cell_size) & 1;
            int cy = ((int32_t)yi / cell_size) & 1;
            gui_color_t c = (cx ^ cy) ? GUI_WHITE : GUI_LIGHT_GRAY;
            __put(x + (int32_t)xi, y + (int32_t)yi, c);
        }
}

/* ===== Star ===== */
void gui_draw_star(int32_t cx, int32_t cy, int32_t outer_r, int32_t inner_r, gui_color_t color) {
    /* 5-pointed star */
    int pts[10][2];
    for (int i = 0; i < 5; i++) {
        int angle1 = i * 72 - 90;
        int angle2 = i * 72 + 36 - 90;
        /* Approximate sin/cos using precomputed values */
        /* Angle -> (sin, cos) table for 0, 36, 72, 108, 144... degrees */
        /* We'll just compute points approximately */
        int si1 = 0, co1 = 1, si2 = 0, co2 = 0;
        (void)angle1; (void)angle2;
        switch (i) {
            case 0: si1 = -1; co1 = 0; si2 = -((int)(0.5878f * 1000))/1000; co2 = ((int)(0.8090f * 1000))/1000; break;
            case 1: si1 = -((int)(0.5878f * 1000))/1000; co1 = -((int)(0.8090f * 1000))/1000; si2 = ((int)(0.9511f * 1000))/1000; co2 = -((int)(0.3090f * 1000))/1000; break;
            case 2: si1 = ((int)(0.9511f * 1000))/1000; co1 = -((int)(0.3090f * 1000))/1000; si2 = -((int)(0.9511f * 1000))/1000; co2 = ((int)(0.3090f * 1000))/1000; break;
            case 3: si1 = -((int)(0.9511f * 1000))/1000; co1 = ((int)(0.3090f * 1000))/1000; si2 = ((int)(0.5878f * 1000))/1000; co2 = ((int)(0.8090f * 1000))/1000; break;
            case 4: si1 = ((int)(0.5878f * 1000))/1000; co1 = ((int)(0.8090f * 1000))/1000; si2 = 1; co2 = 0; break;
        }
        pts[i][0] = cx + (int)((float)outer_r * (float)co1);
        pts[i][1] = cy + (int)((float)outer_r * (float)si1);
        pts[i+5][0] = cx + (int)((float)inner_r * (float)co2);
        pts[i+5][1] = cy + (int)((float)inner_r * (float)si2);
    }
    /* Connect points */
    for (int i = 0; i < 5; i++) {
        int p1 = i, p2 = i + 5, p3 = (i + 1) % 5;
        gui_draw_line(pts[p1][0], pts[p1][1], pts[p2][0], pts[p2][1], color);
        gui_draw_line(pts[p2][0], pts[p2][1], pts[p3][0], pts[p3][1], color);
    }
}

/* ===== Color utilities ===== */
gui_color_t gui_color_lerp(gui_color_t a, gui_color_t b, int t, int max_t) {
    return __lerp_color(a, b, t, max_t);
}
gui_color_t gui_color_darken(gui_color_t c, int amount) {
    int r = __max(0, (int)((c >> 16) & 0xFF) - amount);
    int g = __max(0, (int)((c >> 8) & 0xFF) - amount);
    int b = __max(0, (int)(c & 0xFF) - amount);
    return GUI_COLOR(r, g, b);
}
gui_color_t gui_color_lighten(gui_color_t c, int amount) {
    int r = __min(255, (int)((c >> 16) & 0xFF) + amount);
    int g = __min(255, (int)((c >> 8) & 0xFF) + amount);
    int b = __min(255, (int)(c & 0xFF) + amount);
    return GUI_COLOR(r, g, b);
}

/* ===================================================================
 * New Widgets
 * =================================================================== */

/* ── Checkbox ── */
static void checkbox_draw(gui_widget_t *w) {
    gui_checkbox_data_t *d = (gui_checkbox_data_t *)w->data;
    gui_rect_t box = {w->rect.x, w->rect.y + 2, 14, 14};
    gui_window_draw_rect(NULL, box, GUI_WHITE);
    gui_window_draw_rect_outline(NULL, box, GUI_DARK_GRAY, 1);
    if (d->checked) {
        gui_draw_line(box.x + 2, box.y + 7, box.x + 5, box.y + 10, GUI_BLUE);
        gui_draw_line(box.x + 5, box.y + 10, box.x + 12, box.y + 3, GUI_BLUE);
    }
    gui_window_draw_text(NULL, w->rect.x + 20, w->rect.y + 2, d->label, GUI_TEXT_FG, w->bg);
}

static void checkbox_event(gui_widget_t *w, gui_event_t *evt) {
    gui_checkbox_data_t *d = (gui_checkbox_data_t *)w->data;
    if (evt->type == GUI_EVENT_MOUSE_DOWN && evt->button == 1) {
        d->checked = !d->checked;
        if (d->on_change) d->on_change(w, d->checked);
    }
}

static void checkbox_destroy(gui_widget_t *w) { if (w->data) kfree(w->data); }

gui_widget_t* gui_checkbox_create(gui_rect_t rect, const char *label, int checked) {
    gui_widget_t *w = gui_widget_create(rect);
    if (!w) return NULL;
    gui_checkbox_data_t *d = kmalloc(sizeof(gui_checkbox_data_t));
    if (!d) { kfree(w); return NULL; }
    memset(d, 0, sizeof(gui_checkbox_data_t));
    d->checked = checked;
    if (label) strncpy(d->label, label, sizeof(d->label) - 1);
    w->data = d; w->draw = checkbox_draw; w->on_event = checkbox_event; w->destroy = checkbox_destroy;
    return w;
}
int gui_checkbox_is_checked(gui_widget_t *cb) {
    gui_checkbox_data_t *d = (gui_checkbox_data_t *)cb->data;
    return d ? d->checked : 0;
}
void gui_checkbox_set_checked(gui_widget_t *cb, int checked) {
    gui_checkbox_data_t *d = (gui_checkbox_data_t *)cb->data;
    if (d) d->checked = checked;
}
void gui_checkbox_set_on_change(gui_widget_t *cb, void (*fn)(gui_widget_t *, int)) {
    gui_checkbox_data_t *d = (gui_checkbox_data_t *)cb->data;
    if (d) d->on_change = fn;
}

/* ── Slider ── */
static void slider_draw(gui_widget_t *w) {
    gui_slider_data_t *d = (gui_slider_data_t *)w->data;
    int ww = (int)w->rect.w, hh = (int)w->rect.h;
    int track_h = 6, track_y = (hh - track_h) / 2;
    gui_rect_t track = {w->rect.x, w->rect.y + track_y, (uint32_t)ww, (uint32_t)track_h};
    gui_window_draw_rect(NULL, track, GUI_LIGHT_GRAY);
    gui_window_draw_rect_outline(NULL, track, GUI_GRAY, 1);
    int range = d->max_val - d->min_val;
    int knob_x = range > 0 ? w->rect.x + (ww - 12) * (d->value - d->min_val) / range : w->rect.x;
    gui_rect_t knob = {knob_x, w->rect.y + 2, 12, (uint32_t)hh - 4};
    gui_window_draw_rect(NULL, knob, GUI_BUTTON_BG);
    gui_window_draw_rect_outline(NULL, knob, GUI_DARK_GRAY, 1);
}

static void slider_event(gui_widget_t *w, gui_event_t *evt) {
    gui_slider_data_t *d = (gui_slider_data_t *)w->data;
    if (evt->type == GUI_EVENT_MOUSE_DOWN && evt->button == 1) {
        int ww = (int)w->rect.w - 12;
        if (ww <= 0) return;
        int rel_x = evt->x - w->rect.x - 6;
        if (rel_x < 0) rel_x = 0;
        if (rel_x > ww) rel_x = ww;
        int range = d->max_val - d->min_val;
        d->value = d->min_val + (range > 0 ? rel_x * range / ww : 0);
        if (d->on_change) d->on_change(w, d->value);
    }
}

static void slider_destroy(gui_widget_t *w) { if (w->data) kfree(w->data); }

gui_widget_t* gui_slider_create(gui_rect_t rect, int min_val, int max_val, int initial) {
    gui_widget_t *w = gui_widget_create(rect);
    if (!w) return NULL;
    gui_slider_data_t *d = kmalloc(sizeof(gui_slider_data_t));
    if (!d) { kfree(w); return NULL; }
    memset(d, 0, sizeof(gui_slider_data_t));
    d->min_val = min_val; d->max_val = max_val; d->value = initial;
    w->data = d; w->draw = slider_draw; w->on_event = slider_event; w->destroy = slider_destroy;
    return w;
}
int gui_slider_get_value(gui_widget_t *sl) {
    gui_slider_data_t *d = (gui_slider_data_t *)sl->data;
    return d ? d->value : 0;
}
void gui_slider_set_value(gui_widget_t *sl, int value) {
    gui_slider_data_t *d = (gui_slider_data_t *)sl->data;
    if (d) d->value = value;
}
void gui_slider_set_on_change(gui_widget_t *sl, void (*fn)(gui_widget_t *, int)) {
    gui_slider_data_t *d = (gui_slider_data_t *)sl->data;
    if (d) d->on_change = fn;
}

/* ── Listbox ── */
static void listbox_draw(gui_widget_t *w) {
    gui_listbox_data_t *d = (gui_listbox_data_t *)w->data;
    gui_window_draw_rect(NULL, w->rect, GUI_WHITE);
    gui_window_draw_rect_outline(NULL, w->rect, GUI_GRAY, 1);
    int item_h = 16;
    int vis = (int)w->rect.h / item_h;
    if (vis <= 0) vis = 1;
    for (int i = 0; i < vis && i + d->scroll_offset < d->num_items; i++) {
        int idx = i + d->scroll_offset;
        int yy = w->rect.y + i * item_h;
        if (idx == d->selected) {
            gui_rect_t sel = {w->rect.x + 1, yy, w->rect.w - 2, (uint32_t)item_h};
            gui_window_draw_rect(NULL, sel, GUI_TITLE_BG);
            gui_window_draw_text(NULL, w->rect.x + 4, yy + 2, d->items[idx], GUI_WHITE, GUI_TITLE_BG);
        } else {
            gui_window_draw_text(NULL, w->rect.x + 4, yy + 2, d->items[idx], GUI_TEXT_FG, GUI_WHITE);
        }
    }
}

static void listbox_event(gui_widget_t *w, gui_event_t *evt) {
    gui_listbox_data_t *d = (gui_listbox_data_t *)w->data;
    if (evt->type == GUI_EVENT_MOUSE_DOWN && evt->button == 1) {
        int item_h = 16;
        int idx = d->scroll_offset + (evt->y - w->rect.y) / item_h;
        if (idx >= 0 && idx < d->num_items) {
            d->selected = idx;
            if (d->on_select) d->on_select(w, idx, d->items[idx]);
        }
    }
}

static void listbox_destroy(gui_widget_t *w) { if (w->data) kfree(w->data); }

gui_widget_t* gui_listbox_create(gui_rect_t rect) {
    gui_widget_t *w = gui_widget_create(rect);
    if (!w) return NULL;
    gui_listbox_data_t *d = kmalloc(sizeof(gui_listbox_data_t));
    if (!d) { kfree(w); return NULL; }
    memset(d, 0, sizeof(gui_listbox_data_t));
    d->selected = -1;
    w->data = d; w->draw = listbox_draw; w->on_event = listbox_event; w->destroy = listbox_destroy;
    return w;
}
void gui_listbox_add_item(gui_widget_t *lb, const char *item) {
    gui_listbox_data_t *d = (gui_listbox_data_t *)lb->data;
    if (!d || d->num_items >= 64) return;
    strncpy(d->items[d->num_items], item, 63);
    d->items[d->num_items][63] = '\0';
    d->num_items++;
}
void gui_listbox_clear(gui_widget_t *lb) {
    gui_listbox_data_t *d = (gui_listbox_data_t *)lb->data;
    if (d) { memset(d->items, 0, sizeof(d->items)); d->num_items = 0; d->selected = -1; d->scroll_offset = 0; }
}
int gui_listbox_get_selected(gui_widget_t *lb) {
    gui_listbox_data_t *d = (gui_listbox_data_t *)lb->data;
    return d ? d->selected : -1;
}
const char* gui_listbox_get_selected_text(gui_widget_t *lb) {
    gui_listbox_data_t *d = (gui_listbox_data_t *)lb->data;
    if (!d || d->selected < 0 || d->selected >= d->num_items) return "";
    return d->items[d->selected];
}
void gui_listbox_set_on_select(gui_widget_t *lb, void (*fn)(gui_widget_t *, int, const char *)) {
    gui_listbox_data_t *d = (gui_listbox_data_t *)lb->data;
    if (d) d->on_select = fn;
}
