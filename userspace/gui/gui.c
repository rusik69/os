#include "gui.h"
#include "gui_draw.h"
#include <string.h>

/* va_list support for varargs */
typedef __builtin_va_list va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)

/* GUI global context - define the struct here */
struct gui_context {
    gui_window_t *windows;
    gui_window_t *focused_window;
    int32_t mouse_x, mouse_y;
    int mouse_buttons;
    int initialized;
    gui_cursor_t cursor;
    gui_color_t theme_title_bg;
    gui_color_t theme_window_bg;
    gui_color_t theme_button_bg;
};

static struct gui_context g_gui_ctx = {0};

/* Built-in 5x7 bitmap font (from vga.c via extern or inline) */
static const uint8_t font5x7[47][7] = {
    /* A */ {0x7C, 0x12, 0x11, 0x12, 0x7C, 0x00, 0x00},
    /* B */ {0x7E, 0x49, 0x49, 0x49, 0x36, 0x00, 0x00},
    /* C */ {0x3C, 0x42, 0x41, 0x42, 0x3C, 0x00, 0x00},
    /* D */ {0x7E, 0x41, 0x41, 0x42, 0x3C, 0x00, 0x00},
    /* E */ {0x7E, 0x49, 0x49, 0x49, 0x41, 0x00, 0x00},
    /* F */ {0x7E, 0x09, 0x09, 0x09, 0x01, 0x00, 0x00},
    /* G */ {0x3C, 0x42, 0x49, 0x49, 0x3A, 0x00, 0x00},
    /* H */ {0x7F, 0x08, 0x08, 0x08, 0x7F, 0x00, 0x00},
    /* I */ {0x41, 0x41, 0x7F, 0x41, 0x41, 0x00, 0x00},
    /* J */ {0x20, 0x40, 0x41, 0x3F, 0x01, 0x00, 0x00},
    /* K */ {0x7F, 0x08, 0x14, 0x22, 0x41, 0x00, 0x00},
    /* L */ {0x7F, 0x40, 0x40, 0x40, 0x40, 0x00, 0x00},
    /* M */ {0x7F, 0x02, 0x04, 0x02, 0x7F, 0x00, 0x00},
    /* N */ {0x7F, 0x04, 0x08, 0x10, 0x7F, 0x00, 0x00},
    /* O */ {0x3E, 0x41, 0x41, 0x41, 0x3E, 0x00, 0x00},
    /* P */ {0x7F, 0x09, 0x09, 0x09, 0x06, 0x00, 0x00},
    /* Q */ {0x3E, 0x41, 0x51, 0x21, 0x5E, 0x00, 0x00},
    /* R */ {0x7F, 0x09, 0x19, 0x29, 0x46, 0x00, 0x00},
    /* S */ {0x26, 0x49, 0x49, 0x49, 0x32, 0x00, 0x00},
    /* T */ {0x03, 0x01, 0x7F, 0x01, 0x03, 0x00, 0x00},
    /* U */ {0x3F, 0x40, 0x40, 0x40, 0x3F, 0x00, 0x00},
    /* V */ {0x1F, 0x20, 0x40, 0x20, 0x1F, 0x00, 0x00},
    /* W */ {0x3F, 0x40, 0x38, 0x40, 0x3F, 0x00, 0x00},
    /* X */ {0x63, 0x14, 0x08, 0x14, 0x63, 0x00, 0x00},
    /* Y */ {0x07, 0x08, 0x70, 0x08, 0x07, 0x00, 0x00},
    /* Z */ {0x61, 0x51, 0x49, 0x45, 0x43, 0x00, 0x00},
    /* 0 */ {0x3E, 0x45, 0x49, 0x51, 0x3E, 0x00, 0x00},
    /* 1 */ {0x00, 0x42, 0x7F, 0x40, 0x00, 0x00, 0x00},
    /* 2 */ {0x42, 0x61, 0x51, 0x49, 0x46, 0x00, 0x00},
    /* 3 */ {0x21, 0x41, 0x49, 0x49, 0x36, 0x00, 0x00},
    /* 4 */ {0x18, 0x14, 0x12, 0x7F, 0x10, 0x00, 0x00},
    /* 5 */ {0x27, 0x45, 0x45, 0x45, 0x39, 0x00, 0x00},
    /* 6 */ {0x3C, 0x4A, 0x49, 0x49, 0x30, 0x00, 0x00},
    /* 7 */ {0x01, 0x71, 0x09, 0x05, 0x03, 0x00, 0x00},
    /* 8 */ {0x36, 0x49, 0x49, 0x49, 0x36, 0x00, 0x00},
    /* 9 */ {0x06, 0x49, 0x49, 0x29, 0x1E, 0x00, 0x00},
    /* space */ {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    /* . */ {0x00, 0x60, 0x60, 0x00, 0x00, 0x00, 0x00},
    /* , */ {0x00, 0xA0, 0x60, 0x00, 0x00, 0x00, 0x00},
    /* - */ {0x08, 0x08, 0x08, 0x08, 0x08, 0x00, 0x00},
    /* / */ {0x20, 0x10, 0x08, 0x04, 0x02, 0x00, 0x00},
    /* ( */ {0x1C, 0x22, 0x41, 0x00, 0x00, 0x00, 0x00},
    /* ) */ {0x41, 0x22, 0x1C, 0x00, 0x00, 0x00, 0x00},
    /* : */ {0x00, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00},
    /* _ */ {0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00},
    /* + */ {0x08, 0x08, 0x3E, 0x08, 0x08, 0x00, 0x00},
    /* = */ {0x14, 0x14, 0x14, 0x14, 0x14, 0x00, 0x00},
};

static int font_char_index(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    if (c == ' ') return 36;
    if (c == '.') return 37;
    if (c == ',') return 38;
    if (c == '-') return 39;
    if (c == '/') return 40;
    if (c == '(') return 41;
    if (c == ')') return 42;
    if (c == ':') return 43;
    if (c == '_') return 44;
    if (c == '+') return 45;
    if (c == '=') return 46;
    return 36;
}

static void render_glyph(int32_t x, int32_t y, char c, gui_color_t fg, gui_color_t bg,
                          const gui_window_t *clip) {
    int idx = font_char_index(c);
    const uint8_t *glyph = font5x7[idx];
    /* Precompute clip boundaries */
    int32_t cl = clip ? clip->rect.x : INT32_MIN;
    int32_t ct = clip ? clip->rect.y : INT32_MIN;
    int32_t cr = clip ? (int32_t)(clip->rect.x + clip->rect.w) : INT32_MAX;
    int32_t cb = clip ? (int32_t)(clip->rect.y + clip->rect.h) : INT32_MAX;
    for (int row = 0; row < 7; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 5; col++) {
            gui_color_t color = (bits & (0x80 >> col)) ? fg : bg;
            for (int dy = 0; dy < 2; dy++) {
                int py = y + row * 2 + dy;
                if (py < ct || py >= cb) continue;
                for (int dx = 0; dx < 2; dx++) {
                    int px = x + col * 2 + dx;
                    if (px < cl || px >= cr) continue;
                    vga_put_pixel(px, py, color);
                }
            }
        }
    }
}

/* ===== Window Management ===== */

struct gui_window {
    gui_rect_t rect;
    gui_rect_t saved_rect;
    char title[64];
    gui_color_t bg;
    int visible;
    gui_window_state_t state;
    uint32_t min_w, min_h;
    gui_widget_t *widgets;
    gui_widget_t *focused_widget;
    gui_window_t *next, *prev;
};

static const int TITLE_H = 24;
static const int RESIZE_HANDLE = 6;
static const int MIN_W = 80;
static const int MIN_H = 60;

/* Window close button check */
int gui_window_titlebar_at(gui_window_t *win, int32_t x, int32_t y) {
    if (!win || !win->title[0]) return 0;
    return y >= win->rect.y && y < win->rect.y + TITLE_H &&
           x >= win->rect.x && x < (int32_t)(win->rect.x + win->rect.w);
}

int gui_window_resize_handle_at(gui_window_t *win, int32_t x, int32_t y) {
    if (!win) return 0;
    int32_t rx = win->rect.x, ry = win->rect.y;
    int32_t rw = (int32_t)win->rect.w, rh = (int32_t)win->rect.h;
    int h = RESIZE_HANDLE;
    if (x >= rx && x < rx + h && y >= ry && y < ry + h) return 1; /* NW */
    if (x >= rx + rw - h && x < rx + rw && y >= ry && y < ry + h) return 2; /* NE */
    if (x >= rx && x < rx + h && y >= ry + rh - h && y < ry + rh) return 3; /* SW */
    if (x >= rx + rw - h && x < rx + rw && y >= ry + rh - h && y < ry + rh) return 4; /* SE */
    if (x >= rx && x < rx + rw && y >= ry + rh - h && y < ry + rh) return 5; /* S */
    if (x >= rx && x < rx + h && y >= ry && y < ry + rh) return 6; /* W */
    if (x >= rx + rw - h && x < rx + rw && y >= ry && y < ry + rh) return 7; /* E */
    if (x >= rx && x < rx + rw && y >= ry && y < ry + h) return 8; /* N */
    return 0;
}

gui_cursor_t gui_window_get_resize_cursor(gui_window_t *win, int32_t x, int32_t y) {
    int h = gui_window_resize_handle_at(win, x, y);
    if (h == 0) return GUI_CURSOR_ARROW;
    if (h == 1 || h == 4) return GUI_CURSOR_RESIZE_DIAGONAL;
    if (h == 2 || h == 3) return GUI_CURSOR_RESIZE_DIAGONAL;
    if (h == 5 || h == 8) return GUI_CURSOR_RESIZE_V;
    if (h == 6 || h == 7) return GUI_CURSOR_RESIZE_H;
    return GUI_CURSOR_ARROW;
}

gui_window_t* gui_window_create(const char *title, int32_t x, int32_t y,
                                 uint32_t w, uint32_t h, gui_color_t bg) {
    gui_window_t *win = kmalloc(sizeof(gui_window_t));
    if (!win) return NULL;
    memset(win, 0, sizeof(gui_window_t));
    win->rect.x = x; win->rect.y = y;
    win->rect.w = w; win->rect.h = h;
    win->bg = bg;
    win->visible = 1;
    win->state = GUI_WINDOW_NORMAL;
    win->min_w = MIN_W; win->min_h = MIN_H;
    if (title) {
        strncpy(win->title, title, sizeof(win->title) - 1);
        win->title[sizeof(win->title) - 1] = '\0';
    }
    return win;
}

void gui_window_destroy(gui_window_t *win) {
    if (!win) return;
    gui_widget_t *w = win->widgets;
    while (w) { gui_widget_t *next = w->next; gui_widget_destroy(w); w = next; }
    kfree(win);
}

void gui_window_minimize(gui_window_t *win) {
    if (!win) return;
    win->saved_rect = win->rect;
    win->state = GUI_WINDOW_MINIMIZED;
    win->visible = 0;
}

void gui_window_maximize(gui_window_t *win) {
    if (!win) return;
    win->saved_rect = win->rect;
    win->state = GUI_WINDOW_MAXIMIZED;
    win->rect.x = 2; win->rect.y = 2;
    win->rect.w = 1020; win->rect.h = 746;
    win->visible = 1;
}

void gui_window_restore(gui_window_t *win) {
    if (!win) return;
    if (win->state == GUI_WINDOW_MAXIMIZED || win->state == GUI_WINDOW_MINIMIZED)
        win->rect = win->saved_rect;
    win->state = GUI_WINDOW_NORMAL;
    win->visible = 1;
}

gui_window_state_t gui_window_get_state(gui_window_t *win) {
    return win ? win->state : GUI_WINDOW_NORMAL;
}

void gui_window_set_min_size(gui_window_t *win, uint32_t min_w, uint32_t min_h) {
    if (win) { win->min_w = min_w; win->min_h = min_h; }
}

void gui_window_set_icon(gui_window_t *win, gui_color_t *icon, uint32_t iw, uint32_t ih) {
    (void)win; (void)icon; (void)iw; (void)ih;
}

void gui_window_bring_to_front(gui_window_t *win) {
    if (!win) return;
    gui_remove_window(win);
    gui_add_window(win);
}

void gui_window_close(gui_window_t *win) {
    if (!win) return;
    gui_window_destroy(win);
}

void gui_window_set_title(gui_window_t *win, const char *title) {
    if (!win || !title) return;
    strncpy(win->title, title, sizeof(win->title) - 1);
    win->title[sizeof(win->title) - 1] = '\0';
}

void gui_window_clear(gui_window_t *win, gui_color_t color) {
    if (!win) return;
    gui_window_draw_rect(win, win->rect, color);
}

void gui_window_draw_pixel(gui_window_t *win, int32_t x, int32_t y, gui_color_t color) {
    if (win) {
        if (x < win->rect.x || y < win->rect.y ||
            x >= (int32_t)(win->rect.x + win->rect.w) ||
            y >= (int32_t)(win->rect.y + win->rect.h)) return;
    }
    vga_put_pixel(x, y, color);
}

void gui_window_draw_rect(gui_window_t *win, gui_rect_t rect, gui_color_t color) {
    for (int32_t y = rect.y; y < (int32_t)(rect.y + rect.h); y++) {
        for (int32_t x = rect.x; x < (int32_t)(rect.x + rect.w); x++) {
            gui_window_draw_pixel(win, x, y, color);
        }
    }
}

void gui_window_draw_rect_outline(gui_window_t *win, gui_rect_t rect,
                                   gui_color_t color, int thickness) {
    for (int t = 0; t < thickness; t++) {
        gui_rect_t r = {rect.x + t, rect.y + t, rect.w - 2*t, rect.h - 2*t};
        for (int32_t x = r.x; x < (int32_t)(r.x + r.w); x++)
            gui_window_draw_pixel(win, x, r.y, color);
        for (int32_t x = r.x; x < (int32_t)(r.x + r.w); x++)
            gui_window_draw_pixel(win, x, r.y + r.h - 1, color);
        for (int32_t y = r.y; y < (int32_t)(r.y + r.h); y++)
            gui_window_draw_pixel(win, r.x, y, color);
        for (int32_t y = r.y; y < (int32_t)(r.y + r.h); y++)
            gui_window_draw_pixel(win, r.x + r.w - 1, y, color);
    }
}

void gui_window_draw_text(gui_window_t *win, int32_t x, int32_t y,
                          const char *text, gui_color_t fg, gui_color_t bg) {
    if (!text) return;
    for (int i = 0; text[i]; i++) {
        render_glyph(x + i * 12, y, text[i], fg, bg, win);
    }
}

void gui_window_draw_text_align(gui_window_t *win, gui_rect_t rect,
                                const char *text, gui_align_t align,
                                gui_color_t fg, gui_color_t bg) {
    if (!text) return;
    int tw = gui_text_width(text);
    int32_t x = rect.x;
    if (align == GUI_ALIGN_CENTER)
        x = rect.x + ((int32_t)rect.w - tw) / 2;
    else if (align == GUI_ALIGN_RIGHT)
        x = rect.x + (int32_t)rect.w - tw;
    int32_t y = rect.y + ((int32_t)rect.h - 14) / 2;
    gui_window_draw_text(win, x, y, text, fg, bg);
}

/* Simplified string drawing without varargs */
void gui_window_draw_string(gui_window_t *win, int32_t x, int32_t y,
                            const char *text) {
    gui_window_draw_text(win, x, y, text, GUI_TEXT_FG, win ? win->bg : GUI_WINDOW_BG);
}

gui_rect_t gui_window_get_rect(gui_window_t *win) {
    return win ? win->rect : (gui_rect_t){0, 0, 0, 0};
}

void gui_window_set_rect(gui_window_t *win, gui_rect_t rect) {
    if (!win) return;
    if (rect.w < win->min_w) rect.w = win->min_w;
    if (rect.h < win->min_h) rect.h = win->min_h;
    win->rect = rect;
}

void gui_window_set_visible(gui_window_t *win, int visible) {
    if (win) win->visible = visible;
}

int gui_window_is_visible(gui_window_t *win) {
    return win ? win->visible : 0;
}

int gui_window_contains_point(gui_window_t *win, int32_t x, int32_t y) {
    if (!win || !win->visible) return 0;
    return x >= win->rect.x && y >= win->rect.y &&
           x < (int32_t)(win->rect.x + win->rect.w) &&
           y < (int32_t)(win->rect.y + win->rect.h);
}

void gui_window_add_widget(gui_window_t *win, gui_widget_t *widget) {
    if (!win || !widget) return;
    widget->next = win->widgets;
    win->widgets = widget;
    if (!win->focused_widget) win->focused_widget = widget;
}

gui_widget_t* gui_window_get_focused_widget(gui_window_t *win) {
    return win ? win->focused_widget : NULL;
}

void gui_window_set_focused_widget(gui_window_t *win, gui_widget_t *widget) {
    if (win) win->focused_widget = widget;
}

gui_widget_t* gui_window_first_widget(gui_window_t *win) {
    return win ? win->widgets : NULL;
}

int gui_window_has_title(gui_window_t *win) {
    return win && win->title[0] != '\0';
}

/* ===== Widget Management ===== */

gui_widget_t* gui_widget_create(gui_rect_t rect) {
    gui_widget_t *w = kmalloc(sizeof(gui_widget_t));
    if (!w) return NULL;
    memset(w, 0, sizeof(gui_widget_t));
    w->rect = rect;
    w->bg = GUI_WINDOW_BG;
    w->fg = GUI_TEXT_FG;
    w->visible = 1;
    w->enabled = 1;
    return w;
}

void gui_widget_destroy(gui_widget_t *w) {
    if (!w) return;
    if (w->destroy) w->destroy(w);
    kfree(w);
}

void gui_widget_default_destroy(gui_widget_t *w) { (void)w; }

void gui_widget_draw(gui_widget_t *w) {
    if (!w || !w->visible || !w->draw) return;
    w->draw(w);
}

void gui_widget_on_event(gui_widget_t *w, gui_event_t *evt) {
    if (!w || !w->visible || !w->enabled || !w->on_event) return;
    w->on_event(w, evt);
}

int gui_widget_contains_point(gui_widget_t *w, int32_t x, int32_t y) {
    if (!w || !w->visible) return 0;
    return x >= w->rect.x && y >= w->rect.y &&
           x < (int32_t)(w->rect.x + w->rect.w) &&
           y < (int32_t)(w->rect.y + w->rect.h);
}

void gui_widget_disable(gui_widget_t *w) { if (w) w->enabled = 0; }
void gui_widget_enable(gui_widget_t *w) { if (w) w->enabled = 1; }
int gui_widget_is_enabled(gui_widget_t *w) { return w ? w->enabled : 0; }

/* ===== Concrete Widgets ===== */

static void button_draw(gui_widget_t *w) {
    gui_button_data_t *data = (gui_button_data_t *)w->data;
    gui_color_t bg = w->enabled ? GUI_BUTTON_BG : GUI_LIGHT_GRAY;
    gui_window_draw_rect(NULL, w->rect, bg);
    gui_window_draw_rect_outline(NULL, w->rect, GUI_DARK_GRAY, 2);
    gui_window_draw_text(NULL, w->rect.x + 8, w->rect.y + 4,
                        data->label, GUI_BUTTON_FG, bg);
}

static void button_event(gui_widget_t *w, gui_event_t *evt) {
    gui_button_data_t *data = (gui_button_data_t *)w->data;
    if (evt->type == GUI_EVENT_MOUSE_DOWN && evt->button == 1) {
        if (data->on_click) data->on_click(w);
    }
}

static void button_destroy(gui_widget_t *w) { if (w->data) kfree(w->data); }

gui_widget_t* gui_button_create(gui_rect_t rect, const char *label) {
    gui_widget_t *w = gui_widget_create(rect);
    if (!w) return NULL;
    gui_button_data_t *data = kmalloc(sizeof(gui_button_data_t));
    if (!data) { kfree(w); return NULL; }
    memset(data, 0, sizeof(gui_button_data_t));
    if (label) strncpy(data->label, label, sizeof(data->label) - 1);
    w->data = data; w->draw = button_draw; w->on_event = button_event; w->destroy = button_destroy;
    return w;
}

void gui_button_set_on_click(gui_widget_t *btn, void (*fn)(gui_widget_t *)) {
    if (btn && btn->data) ((gui_button_data_t *)btn->data)->on_click = fn;
}

static void textbox_draw(gui_widget_t *w) {
    gui_textbox_data_t *data = (gui_textbox_data_t *)w->data;
    gui_window_draw_rect(NULL, w->rect, GUI_WHITE);
    gui_window_draw_rect_outline(NULL, w->rect, GUI_GRAY, 1);
    gui_window_draw_text(NULL, w->rect.x + 4, w->rect.y + 4,
                        data->text, GUI_TEXT_FG, GUI_WHITE);
}

static void textbox_event(gui_widget_t *w, gui_event_t *evt) {
    gui_textbox_data_t *data = (gui_textbox_data_t *)w->data;
    if (evt->type == GUI_EVENT_CHAR && data->cursor_pos < data->max_len) {
        if (evt->ch == 8 && data->cursor_pos > 0) { /* backspace */
            data->text[--data->cursor_pos] = '\0';
        } else if (evt->ch >= 32) {
            data->text[data->cursor_pos++] = evt->ch;
            data->text[data->cursor_pos] = '\0';
        }
    }
}

static void textbox_destroy(gui_widget_t *w) { if (w->data) kfree(w->data); }

gui_widget_t* gui_textbox_create(gui_rect_t rect, int max_len) {
    gui_widget_t *w = gui_widget_create(rect);
    if (!w) return NULL;
    gui_textbox_data_t *data = kmalloc(sizeof(gui_textbox_data_t));
    if (!data) { kfree(w); return NULL; }
    memset(data, 0, sizeof(gui_textbox_data_t));
    data->max_len = max_len > (int)sizeof(data->text) - 1 ? (int)sizeof(data->text) - 1 : max_len;
    w->data = data; w->draw = textbox_draw; w->on_event = textbox_event; w->destroy = textbox_destroy;
    return w;
}

const char* gui_textbox_get_text(gui_widget_t *box) {
    gui_textbox_data_t *data = (gui_textbox_data_t *)box->data;
    return data ? data->text : "";
}

void gui_textbox_set_text(gui_widget_t *box, const char *text) {
    gui_textbox_data_t *data = (gui_textbox_data_t *)box->data;
    if (!data) return;
    memset(data->text, 0, sizeof(data->text));
    if (text) strncpy(data->text, text, data->max_len);
    data->cursor_pos = (int)strlen(data->text);
}

static void label_draw(gui_widget_t *w) {
    gui_label_data_t *data = (gui_label_data_t *)w->data;
    gui_window_draw_text(NULL, w->rect.x, w->rect.y, data->label, w->fg, w->bg);
}

static void label_destroy(gui_widget_t *w) { if (w->data) kfree(w->data); }

gui_widget_t* gui_label_create(gui_rect_t rect, const char *text) {
    gui_widget_t *w = gui_widget_create(rect);
    if (!w) return NULL;
    gui_label_data_t *data = kmalloc(sizeof(gui_label_data_t));
    if (!data) { kfree(w); return NULL; }
    memset(data, 0, sizeof(gui_label_data_t));
    if (text) strncpy(data->label, text, sizeof(data->label) - 1);
    w->data = data; w->draw = label_draw; w->destroy = label_destroy;
    return w;
}

void gui_label_set_text(gui_widget_t *lbl, const char *text) {
    gui_label_data_t *data = (gui_label_data_t *)lbl->data;
    if (!data) return;
    if (text) { strncpy(data->label, text, sizeof(data->label) - 1); data->label[sizeof(data->label)-1] = '\0'; }
}

/* ===== TextEdit widget ===== */
static void textedit_draw(gui_widget_t *w) {
    gui_textedit_data_t *d = (gui_textedit_data_t *)w->data;
    gui_window_draw_rect(NULL, w->rect, GUI_WHITE);
    gui_window_draw_rect_outline(NULL, w->rect, GUI_GRAY, 1);
    int line_h = 14, y = w->rect.y + 4, max_vis = (int)w->rect.h / line_h;
    const char *p = d->text;
    int line = 0;
    while (*p && line - d->scroll_y < max_vis) {
        if (line >= d->scroll_y) {
            char buf[80]; int bi = 0;
            while (*p && *p != '\n' && bi < 79) buf[bi++] = *p++;
            buf[bi] = '\0';
            if (*p == '\n') p++;
            gui_window_draw_text(NULL, w->rect.x + 4, y, buf, GUI_TEXT_FG, GUI_WHITE);
            y += line_h;
        } else {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }
        line++;
    }
}

static void textedit_event(gui_widget_t *w, gui_event_t *evt) {
    gui_textedit_data_t *d = (gui_textedit_data_t *)w->data;
    if (evt->type == GUI_EVENT_CHAR) {
        int len = (int)strlen(d->text);
        if (evt->ch == 8 && d->cursor_pos > 0) {
            for (int i = d->cursor_pos; i <= len; i++)
                d->text[i - 1] = d->text[i];
            d->cursor_pos--;
        } else if (evt->ch == 13 || evt->ch == 10) {
            for (int i = len; i >= d->cursor_pos; i--)
                d->text[i + 1] = d->text[i];
            d->text[d->cursor_pos++] = '\n';
        } else if (evt->ch >= 32 && len < (int)sizeof(d->text) - 1) {
            for (int i = len; i >= d->cursor_pos; i--)
                d->text[i + 1] = d->text[i];
            d->text[d->cursor_pos++] = evt->ch;
        }
    }
}

static void textedit_destroy(gui_widget_t *w) { if (w->data) kfree(w->data); }

gui_widget_t* gui_textedit_create(gui_rect_t rect) {
    gui_widget_t *w = gui_widget_create(rect); if (!w) return NULL;
    gui_textedit_data_t *d = kmalloc(sizeof(gui_textedit_data_t));
    if (!d) { kfree(w); return NULL; }
    memset(d, 0, sizeof(gui_textedit_data_t));
    w->data = d; w->draw = textedit_draw; w->on_event = textedit_event; w->destroy = textedit_destroy;
    return w;
}

void gui_textedit_set_text(gui_widget_t *te, const char *text) {
    gui_textedit_data_t *d = (gui_textedit_data_t *)te->data;
    if (!d) return;
    memset(d->text, 0, sizeof(d->text));
    if (text) strncpy(d->text, text, sizeof(d->text) - 1);
    d->cursor_pos = (int)strlen(d->text);
}

const char* gui_textedit_get_text(gui_widget_t *te) {
    gui_textedit_data_t *d = (gui_textedit_data_t *)te->data;
    return d ? d->text : "";
}

/* ===== TabView widget ===== */
static void tabview_draw(gui_widget_t *w) {
    gui_tabview_data_t *d = (gui_tabview_data_t *)w->data;
    if (!d) return;
    int x = w->rect.x + 2;
    gui_tab_t *tab = d->tabs;
    for (int i = 0; i < d->tab_count && tab; i++) {
        int tw = (int)strlen(tab->title) * 12 + 16;
        gui_color_t c = (i == d->active_tab) ? GUI_WHITE : GUI_LIGHT_GRAY;
        gui_window_draw_rect(NULL, (gui_rect_t){x, w->rect.y, tw, 22}, c);
        if (i != d->active_tab) gui_window_draw_rect_outline(NULL, (gui_rect_t){x, w->rect.y, tw, 22}, GUI_GRAY, 1);
        gui_window_draw_text(NULL, x + 8, w->rect.y + 3, tab->title, GUI_TEXT_FG, c);
        x += tw + 1;
        tab = tab->next;
    }
    /* Content area */
    gui_rect_t cr = {w->rect.x, w->rect.y + 22, w->rect.w, w->rect.h - 22};
    gui_window_draw_rect_outline(NULL, cr, GUI_GRAY, 1);
    if (d->active_tab >= 0 && d->active_tab < d->tab_count) {
        gui_tab_t *t = d->tabs;
        for (int i = 0; i < d->active_tab && t; i++) t = t->next;
        if (t && t->content) {
            gui_window_draw_rect(NULL, cr, GUI_WHITE);
            gui_widget_draw(t->content);
        }
    }
}

static void tabview_event(gui_widget_t *w, gui_event_t *evt) {
    gui_tabview_data_t *d = (gui_tabview_data_t *)w->data;
    if (evt->type == GUI_EVENT_MOUSE_DOWN) {
        int x = w->rect.x + 2;
        gui_tab_t *tab = d->tabs;
        for (int i = 0; i < d->tab_count && tab; i++) {
            int tw = (int)strlen(tab->title) * 12 + 16;
            if (evt->x >= x && evt->x < x + tw) {
                d->active_tab = i;
                return;
            }
            x += tw + 1;
            tab = tab->next;
        }
    }
}

static void tabview_destroy(gui_widget_t *w) {
    gui_tabview_data_t *d = (gui_tabview_data_t *)w->data;
    if (!d) return;
    gui_tab_t *t = d->tabs;
    while (t) { gui_tab_t *next = t->next; if (t->content) gui_widget_destroy(t->content); kfree(t); t = next; }
    kfree(d);
}

gui_widget_t* gui_tabview_create(gui_rect_t rect) {
    gui_widget_t *w = gui_widget_create(rect); if (!w) return NULL;
    gui_tabview_data_t *d = kmalloc(sizeof(gui_tabview_data_t));
    if (!d) { kfree(w); return NULL; }
    memset(d, 0, sizeof(gui_tabview_data_t));
    w->data = d; w->draw = tabview_draw; w->on_event = tabview_event; w->destroy = tabview_destroy;
    return w;
}

int gui_tabview_add_tab(gui_widget_t *tv, const char *title, gui_widget_t *content) {
    gui_tabview_data_t *d = (gui_tabview_data_t *)tv->data;
    if (!d) return -1;
    gui_tab_t *tab = kmalloc(sizeof(gui_tab_t));
    if (!tab) return -1;
    memset(tab, 0, sizeof(gui_tab_t));
    if (title) strncpy(tab->title, title, sizeof(tab->title) - 1);
    tab->content = content;
    tab->next = d->tabs;
    d->tabs = tab;
    d->tab_count++;
    return d->tab_count - 1;
}

void gui_tabview_set_active(gui_widget_t *tv, int idx) {
    gui_tabview_data_t *d = (gui_tabview_data_t *)tv->data;
    if (d && idx >= 0 && idx < d->tab_count) d->active_tab = idx;
}

/* ===== Tooltip widget ===== */
static void tooltip_draw(gui_widget_t *w) {
    gui_tooltip_data_t *d = (gui_tooltip_data_t *)w->data;
    if (!d || !d->text[0]) return;
    int tw = gui_text_width(d->text) + 8;
    gui_rect_t r = {w->rect.x, w->rect.y, tw, 18};
    gui_window_draw_rect(NULL, r, GUI_YELLOW);
    gui_window_draw_rect_outline(NULL, r, GUI_DARK_GRAY, 1);
    gui_window_draw_text(NULL, r.x + 4, r.y + 2, d->text, GUI_TEXT_FG, GUI_YELLOW);
}

static void tooltip_destroy(gui_widget_t *w) { if (w->data) kfree(w->data); }

gui_widget_t* gui_tooltip_create(gui_rect_t rect, const char *text) {
    gui_widget_t *w = gui_widget_create(rect); if (!w) return NULL;
    gui_tooltip_data_t *d = kmalloc(sizeof(gui_tooltip_data_t));
    if (!d) { kfree(w); return NULL; }
    memset(d, 0, sizeof(gui_tooltip_data_t));
    if (text) strncpy(d->text, text, sizeof(d->text) - 1);
    w->data = d; w->draw = tooltip_draw; w->destroy = tooltip_destroy;
    return w;
}

void gui_tooltip_set_text(gui_widget_t *tt, const char *text) {
    gui_tooltip_data_t *d = (gui_tooltip_data_t *)tt->data;
    if (!d) return;
    if (text) strncpy(d->text, text, sizeof(d->text) - 1); else d->text[0] = '\0';
}

/* ===== Notification widget ===== */
static void notification_draw(gui_widget_t *w) {
    gui_notification_data_t *d = (gui_notification_data_t *)w->data;
    int tw = gui_text_width(d->text) + 16;
    gui_rect_t r = {w->rect.x, w->rect.y, tw, 24};
    gui_window_draw_rect(NULL, r, d->notification_color);
    gui_window_draw_rect_outline(NULL, r, GUI_DARK_GRAY, 2);
    gui_window_draw_text(NULL, r.x + 8, r.y + 5, d->text, GUI_WHITE, d->notification_color);
}

static void notification_destroy(gui_widget_t *w) { if (w->data) kfree(w->data); }

gui_widget_t* gui_notification_create(gui_rect_t rect, const char *text, gui_color_t color) {
    gui_widget_t *w = gui_widget_create(rect); if (!w) return NULL;
    gui_notification_data_t *d = kmalloc(sizeof(gui_notification_data_t));
    if (!d) { kfree(w); return NULL; }
    memset(d, 0, sizeof(gui_notification_data_t));
    if (text) strncpy(d->text, text, sizeof(d->text) - 1);
    d->notification_color = color;
    w->data = d; w->draw = notification_draw; w->destroy = notification_destroy;
    return w;
}

/* ===== GUI Context API ===== */

int gui_init(void) {
    memset(&g_gui_ctx, 0, sizeof(struct gui_context));
    g_gui_ctx.initialized = 1;
    g_gui_ctx.mouse_x = 512;
    g_gui_ctx.mouse_y = 384;
    g_gui_ctx.cursor = GUI_CURSOR_ARROW;
    g_gui_ctx.theme_title_bg = GUI_TITLE_BG;
    g_gui_ctx.theme_window_bg = GUI_WINDOW_BG;
    g_gui_ctx.theme_button_bg = GUI_BUTTON_BG;
    return 1;
}

void gui_shutdown(void) {
    gui_window_t *win = g_gui_ctx.windows;
    while (win) { gui_window_t *next = win->next; gui_window_destroy(win); win = next; }
    memset(&g_gui_ctx, 0, sizeof(struct gui_context));
}

void gui_add_window(gui_window_t *win) {
    if (!win) return;
    win->next = g_gui_ctx.windows;
    if (g_gui_ctx.windows) g_gui_ctx.windows->prev = win;
    g_gui_ctx.windows = win;
    g_gui_ctx.focused_window = win;
}

void gui_remove_window(gui_window_t *win) {
    if (!win) return;
    if (win->prev) win->prev->next = win->next;
    if (win->next) win->next->prev = win->prev;
    if (g_gui_ctx.windows == win) g_gui_ctx.windows = win->next;
    if (g_gui_ctx.focused_window == win) g_gui_ctx.focused_window = g_gui_ctx.windows;
}

gui_window_t* gui_get_focused_window(void) { return g_gui_ctx.focused_window; }
void gui_set_focused_window(gui_window_t *win) { if (win) g_gui_ctx.focused_window = win; }

void gui_handle_event(gui_event_t *evt) {
    if (!g_gui_ctx.focused_window) return;
    gui_widget_t *w = g_gui_ctx.focused_window->focused_widget;
    if (w) gui_widget_on_event(w, evt);
}

void gui_update_mouse(int32_t x, int32_t y, int buttons) {
    g_gui_ctx.mouse_x = x;
    g_gui_ctx.mouse_y = y;
    g_gui_ctx.mouse_buttons = buttons;
}

void gui_set_cursor(gui_cursor_t cursor) { g_gui_ctx.cursor = cursor; }
gui_cursor_t gui_get_cursor(void) { return g_gui_ctx.cursor; }

void gui_set_theme_color(gui_color_t title_bg, gui_color_t window_bg, gui_color_t button_bg) {
    g_gui_ctx.theme_title_bg = title_bg;
    g_gui_ctx.theme_window_bg = window_bg;
    g_gui_ctx.theme_button_bg = button_bg;
}

void gui_get_theme_colors(gui_color_t *title_bg, gui_color_t *window_bg, gui_color_t *button_bg) {
    if (title_bg) *title_bg = g_gui_ctx.theme_title_bg;
    if (window_bg) *window_bg = g_gui_ctx.theme_window_bg;
    if (button_bg) *button_bg = g_gui_ctx.theme_button_bg;
}

void gui_screenshot(void) {
    /* Dummy: just outputs a message */
    kprintf("[GUI] Screenshot captured\\n");
}

void gui_typewriter(int32_t x, int32_t y, const char *text, gui_color_t fg, gui_color_t bg, int delay) {
    (void)x; (void)y; (void)text; (void)fg; (void)bg; (void)delay;
    /* Typewriter effect - for animation loop use */
}

/* Draw mouse cursor based on current type */
static void draw_mouse_cursor(void) {
    int mx = g_gui_ctx.mouse_x, my = g_gui_ctx.mouse_y;
    gui_color_t c = GUI_WHITE;
    switch (g_gui_ctx.cursor) {
        case GUI_CURSOR_ARROW: {
            for (int i = 0; i < 12; i++) {
                vga_put_pixel(mx + i/3, my + i, c);
                vga_put_pixel(mx + i/3 + 1, my + i, c);
            }
            break;
        }
        case GUI_CURSOR_TEXT: {
            for (int i = 0; i < 16; i++) {
                vga_put_pixel(mx, my + i, c);
                vga_put_pixel(mx + 2, my + i, c);
            }
            break;
        }
        case GUI_CURSOR_HAND: {
            gui_draw_circle_filled(mx + 4, my + 4, 5, c);
            break;
        }
        case GUI_CURSOR_CROSSHAIR: {
            int s = 8;
            gui_draw_line(mx - s, my, mx + s, my, c);
            gui_draw_line(mx, my - s, mx, my + s, c);
            gui_draw_circle(mx, my, 4, c);
            break;
        }
        case GUI_CURSOR_RESIZE_H:
            gui_draw_line(mx - 6, my, mx + 6, my, c);
            gui_draw_line(mx - 6, my - 4, mx - 6, my + 4, c);
            gui_draw_line(mx + 6, my - 4, mx + 6, my + 4, c);
            break;
        case GUI_CURSOR_RESIZE_V:
            gui_draw_line(mx, my - 6, mx, my + 6, c);
            gui_draw_line(mx - 4, my - 6, mx + 4, my - 6, c);
            gui_draw_line(mx - 4, my + 6, mx + 4, my + 6, c);
            break;
        default: {
            for (int y = my; y < my + 12; y++) {
                vga_put_pixel(mx, y, c); vga_put_pixel(mx + 1, y, c);
            }
            for (int x = mx; x < mx + 12; x++) {
                vga_put_pixel(x, my, c); vga_put_pixel(x, my + 1, c);
            }
            break;
        }
    }
}

void gui_render_frame(void) {
    vga_clear_framebuffer(GUI_BLACK);
    gui_window_t *win = g_gui_ctx.windows;
    while (win && win->next) win = win->next;
    while (win) {
        if (!win->visible) { win = win->prev; continue; }
        /* Background */
        gui_window_draw_rect(win, win->rect, win->bg);
        /* Title bar */
        int is_focused = (win == g_gui_ctx.focused_window);
        gui_color_t tbar_col = is_focused ? g_gui_ctx.theme_title_bg : GUI_COLOR(60, 60, 90);
        int has_title = (win->title[0] != '\0');
        if (has_title) {
            gui_rect_t tr = {win->rect.x, win->rect.y, win->rect.w, TITLE_H};
            gui_window_draw_rect(win, tr, tbar_col);
            gui_window_draw_text(win, win->rect.x + 4, win->rect.y + 4, win->title, GUI_WHITE, tbar_col);
            /* Minimize button [_] */
            gui_rect_t min_r = {(int32_t)(win->rect.x + win->rect.w - 46), win->rect.y + 2, 20, 20};
            gui_window_draw_rect(win, min_r, GUI_COLOR(60, 60, 60));
            gui_draw_line(min_r.x + 4, min_r.y + 14, min_r.x + 16, min_r.y + 14, GUI_WHITE);
            /* Maximize button [ ] */
            gui_rect_t max_r = {(int32_t)(win->rect.x + win->rect.w - 24), win->rect.y + 2, 20, 20};
            gui_window_draw_rect(win, max_r, GUI_COLOR(60, 100, 60));
            gui_window_draw_rect_outline(win, (gui_rect_t){max_r.x + 4, max_r.y + 4, 12, 12}, GUI_WHITE, 1);
            /* Close button [X] */
            gui_rect_t close_r = {(int32_t)(win->rect.x + win->rect.w - 68), win->rect.y + 2, 20, 20};
            gui_window_draw_rect(win, close_r, GUI_COLOR(180, 40, 40));
            gui_window_draw_text(win, close_r.x + 4, close_r.y + 3, "X", GUI_WHITE, GUI_COLOR(180, 40, 40));
        }
        /* Border */
        gui_color_t border_col = is_focused ? GUI_WHITE : GUI_DARK_GRAY;
        gui_window_draw_rect_outline(win, win->rect, border_col, 2);
        /* Resize handles (small corner indicators) */
        if (has_title) {
            int32_t rx = win->rect.x, ry = win->rect.y, rw = (int32_t)win->rect.w, rh = (int32_t)win->rect.h;
            int s = RESIZE_HANDLE;
            vga_put_pixel(rx + rw - 1, ry + rh - 1, GUI_WHITE);
            for (int i = 0; i < s; i++) {
                vga_put_pixel(rx + rw - 1 - i, ry + rh - 1, GUI_WHITE);
                vga_put_pixel(rx + rw - 1, ry + rh - 1 - i, GUI_WHITE);
            }
        }
        /* Widgets */
        gui_widget_t *w = win->widgets;
        while (w) { gui_widget_draw(w); w = w->next; }
        win = win->prev;
    }
    draw_mouse_cursor();
}

void gui_run_loop(void) {
    int running = 1;
    uint64_t last_render = timer_get_ticks();
    const int render_interval = 33;
    while (running) {
        uint64_t now = timer_get_ticks();
        int redraw = 0;
        if (keyboard_has_input()) {
            int c = keyboard_getchar();
            if (c == 27) { running = 0; break; }
            gui_event_t evt; memset(&evt, 0, sizeof(evt));
            evt.type = GUI_EVENT_CHAR; evt.ch = (char)c;
            gui_handle_event(&evt); redraw = 1;
        }
        if (keyboard_is_down(1)) { running = 0; break; }
        int mx, my; uint8_t mbuttons;
        mouse_get_pixel_pos(&mx, &my); mbuttons = mouse_get_buttons();
        gui_update_mouse(mx, my, mbuttons);
        gui_window_t *focused = gui_get_focused_window();
        if (focused && mbuttons) {
            gui_event_t evt; memset(&evt, 0, sizeof(evt));
            if (mbuttons & 1) { evt.type = GUI_EVENT_MOUSE_DOWN; evt.x = mx; evt.y = my; evt.button = 1; gui_handle_event(&evt); redraw = 1; }
        }
        if (redraw || (now - last_render) >= (uint64_t)render_interval) {
            gui_render_frame(); vga_refresh_console(); last_render = now;
        }
        scheduler_yield();
    }
}

gui_window_t* gui_get_window_list(void) { return g_gui_ctx.windows; }
gui_window_t* gui_window_next(gui_window_t *win) { return win ? win->next : NULL; }
