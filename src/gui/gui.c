#include "gui.h"
#include "vga.h"
#include "string.h"
#include "printf.h"
#include "heap.h"

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
};

static struct gui_context g_gui_ctx = {0};

/* Built-in 5x7 bitmap font (from vga.c via extern or inline) */
static const uint8_t font5x7[45][7] = {
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
    return 36; /* space */
}

static void render_glyph(int32_t x, int32_t y, char c, gui_color_t fg, gui_color_t bg) {
    int idx = font_char_index(c);
    const uint8_t *glyph = font5x7[idx];
    
    for (int row = 0; row < 7; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 5; col++) {
            gui_color_t color = (bits & (0x80 >> col)) ? fg : bg;
            /* 2x scale for readability */
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    vga_put_pixel(x + col * 2 + dx, y + row * 2 + dy, color);
                }
            }
        }
    }
}

/* ===== Window Management ===== */

struct gui_window {
    gui_rect_t rect;
    char title[64];
    gui_color_t bg;
    int visible;
    gui_widget_t *widgets;
    gui_widget_t *focused_widget;
    gui_window_t *next, *prev;
};

gui_window_t* gui_window_create(const char *title, int32_t x, int32_t y,
                                 uint32_t w, uint32_t h, gui_color_t bg) {
    gui_window_t *win = kmalloc(sizeof(gui_window_t));
    if (!win) return NULL;
    
    memset(win, 0, sizeof(gui_window_t));
    win->rect.x = x;
    win->rect.y = y;
    win->rect.w = w;
    win->rect.h = h;
    win->bg = bg;
    win->visible = 1;
    
    if (title) {
        strncpy(win->title, title, sizeof(win->title) - 1);
        win->title[sizeof(win->title) - 1] = '\0';
    }
    
    return win;
}

void gui_window_destroy(gui_window_t *win) {
    if (!win) return;
    gui_widget_t *w = win->widgets;
    while (w) {
        gui_widget_t *next = (gui_widget_t *)((uint64_t*)w)[15]; /* hack: next ptr */
        gui_widget_destroy(w);
        w = next;
    }
    kfree(win);
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
    if (!win || x < win->rect.x || y < win->rect.y || 
        x >= (int32_t)(win->rect.x + win->rect.w) || 
        y >= (int32_t)(win->rect.y + win->rect.h)) {
        return;
    }
    vga_put_pixel(x, y, color);
}

void gui_window_draw_rect(gui_window_t *win, gui_rect_t rect, gui_color_t color) {
    if (!win) return;
    for (int32_t y = rect.y; y < (int32_t)(rect.y + rect.h); y++) {
        for (int32_t x = rect.x; x < (int32_t)(rect.x + rect.w); x++) {
            gui_window_draw_pixel(win, x, y, color);
        }
    }
}

void gui_window_draw_rect_outline(gui_window_t *win, gui_rect_t rect,
                                   gui_color_t color, int thickness) {
    if (!win) return;
    for (int t = 0; t < thickness; t++) {
        gui_rect_t r = {rect.x + t, rect.y + t, 
                        rect.w - 2*t, rect.h - 2*t};
        /* top */
        for (int32_t x = r.x; x < (int32_t)(r.x + r.w); x++)
            gui_window_draw_pixel(win, x, r.y, color);
        /* bottom */
        for (int32_t x = r.x; x < (int32_t)(r.x + r.w); x++)
            gui_window_draw_pixel(win, x, r.y + r.h - 1, color);
        /* left */
        for (int32_t y = r.y; y < (int32_t)(r.y + r.h); y++)
            gui_window_draw_pixel(win, r.x, y, color);
        /* right */
        for (int32_t y = r.y; y < (int32_t)(r.y + r.h); y++)
            gui_window_draw_pixel(win, r.x + r.w - 1, y, color);
    }
}

void gui_window_draw_text(gui_window_t *win, int32_t x, int32_t y,
                          const char *text, gui_color_t fg, gui_color_t bg) {
    if (!win || !text) return;
    for (int i = 0; text[i]; i++) {
        render_glyph(x + i * 12, y, text[i], fg, bg);
    }
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
    if (win) win->rect = rect;
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

gui_widget_t* gui_window_get_focused_widget(gui_window_t *win) {
    return win ? win->focused_widget : NULL;
}

void gui_window_set_focused_widget(gui_window_t *win, gui_widget_t *widget) {
    if (win) win->focused_widget = widget;
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
    return w;
}

void gui_widget_destroy(gui_widget_t *w) {
    if (!w) return;
    if (w->destroy) w->destroy(w);
    kfree(w);
}

void gui_widget_draw(gui_widget_t *w) {
    if (!w || !w->visible || !w->draw) return;
    w->draw(w);
}

void gui_widget_on_event(gui_widget_t *w, gui_event_t *evt) {
    if (!w || !w->visible || !w->on_event) return;
    w->on_event(w, evt);
}

int gui_widget_contains_point(gui_widget_t *w, int32_t x, int32_t y) {
    if (!w || !w->visible) return 0;
    return x >= w->rect.x && y >= w->rect.y &&
           x < (int32_t)(w->rect.x + w->rect.w) &&
           y < (int32_t)(w->rect.y + w->rect.h);
}

/* ===== Concrete Widgets ===== */

static void button_draw(gui_widget_t *w) {
    gui_button_data_t *data = (gui_button_data_t *)w->data;
    gui_window_draw_rect(NULL, w->rect, GUI_BUTTON_BG);
    gui_window_draw_rect_outline(NULL, w->rect, GUI_DARK_GRAY, 2);
    gui_window_draw_text(NULL, w->rect.x + 8, w->rect.y + 4, 
                        data->label, GUI_BUTTON_FG, GUI_BUTTON_BG);
}

static void button_event(gui_widget_t *w, gui_event_t *evt) {
    gui_button_data_t *data = (gui_button_data_t *)w->data;
    if (evt->type == GUI_EVENT_MOUSE_DOWN && evt->button == 1) {
        if (data->on_click) data->on_click(w);
    }
}

static void button_destroy(gui_widget_t *w) {
    if (w->data) kfree(w->data);
}

gui_widget_t* gui_button_create(gui_rect_t rect, const char *label) {
    gui_widget_t *w = gui_widget_create(rect);
    if (!w) return NULL;
    gui_button_data_t *data = kmalloc(sizeof(gui_button_data_t));
    if (!data) { kfree(w); return NULL; }
    memset(data, 0, sizeof(gui_button_data_t));
    if (label) strncpy(data->label, label, sizeof(data->label) - 1);
    w->data = data;
    w->draw = button_draw;
    w->on_event = button_event;
    w->destroy = button_destroy;
    return w;
}

void gui_button_set_on_click(gui_widget_t *btn, void (*fn)(gui_widget_t *)) {
    if (btn && btn->data) {
        ((gui_button_data_t *)btn->data)->on_click = fn;
    }
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
        data->text[data->cursor_pos++] = evt->ch;
        data->text[data->cursor_pos] = '\0';
    }
}

static void textbox_destroy(gui_widget_t *w) {
    if (w->data) kfree(w->data);
}

gui_widget_t* gui_textbox_create(gui_rect_t rect, int max_len) {
    gui_widget_t *w = gui_widget_create(rect);
    if (!w) return NULL;
    gui_textbox_data_t *data = kmalloc(sizeof(gui_textbox_data_t));
    if (!data) { kfree(w); return NULL; }
    memset(data, 0, sizeof(gui_textbox_data_t));
    data->max_len = max_len > (int)sizeof(data->text) - 1 ? 
                    (int)sizeof(data->text) - 1 : max_len;
    w->data = data;
    w->draw = textbox_draw;
    w->on_event = textbox_event;
    w->destroy = textbox_destroy;
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
    if (text) {
        strncpy(data->text, text, data->max_len);
    }
    data->cursor_pos = strlen(data->text);
}

static void label_draw(gui_widget_t *w) {
    gui_label_data_t *data = (gui_label_data_t *)w->data;
    gui_window_draw_text(NULL, w->rect.x, w->rect.y,
                        data->label, w->fg, w->bg);
}

static void label_destroy(gui_widget_t *w) {
    if (w->data) kfree(w->data);
}

gui_widget_t* gui_label_create(gui_rect_t rect, const char *text) {
    gui_widget_t *w = gui_widget_create(rect);
    if (!w) return NULL;
    gui_label_data_t *data = kmalloc(sizeof(gui_label_data_t));
    if (!data) { kfree(w); return NULL; }
    memset(data, 0, sizeof(gui_label_data_t));
    if (text) strncpy(data->label, text, sizeof(data->label) - 1);
    w->data = data;
    w->draw = label_draw;
    w->destroy = label_destroy;
    return w;
}

void gui_label_set_text(gui_widget_t *lbl, const char *text) {
    gui_label_data_t *data = (gui_label_data_t *)lbl->data;
    if (!data) return;
    if (text) {
        strncpy(data->label, text, sizeof(data->label) - 1);
        data->label[sizeof(data->label) - 1] = '\0';
    }
}

/* ===== GUI Context API ===== */

int gui_init(void) {
    memset(&g_gui_ctx, 0, sizeof(struct gui_context));
    g_gui_ctx.initialized = 1;
    g_gui_ctx.mouse_x = 512;
    g_gui_ctx.mouse_y = 384;
    return 1;
}

void gui_shutdown(void) {
    gui_window_t *win = g_gui_ctx.windows;
    while (win) {
        gui_window_t *next = win->next;
        gui_window_destroy(win);
        win = next;
    }
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

gui_window_t* gui_get_focused_window(void) {
    return g_gui_ctx.focused_window;
}

void gui_set_focused_window(gui_window_t *win) {
    if (win) g_gui_ctx.focused_window = win;
}

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

void gui_render_frame(void) {
    /* Clear framebuffer */
    vga_clear_framebuffer(GUI_BLACK);
    
    /* Draw windows back-to-front */
    gui_window_t *win = g_gui_ctx.windows;
    while (win && win->next) win = win->next; /* find last */
    
    while (win) {
        if (!win->visible) { win = win->prev; continue; }
        
        /* Draw window background */
        gui_window_draw_rect(win, win->rect, win->bg);
        
        /* Draw title bar */
        gui_rect_t title_rect = {win->rect.x, win->rect.y, win->rect.w, 24};
        gui_window_draw_rect(win, title_rect, GUI_TITLE_BG);
        gui_window_draw_text(win, win->rect.x + 4, win->rect.y + 4,
                            win->title, GUI_WHITE, GUI_TITLE_BG);
        
        /* Draw window border */
        gui_window_draw_rect_outline(win, win->rect, GUI_DARK_GRAY, 2);
        
        /* Draw widgets */
        gui_widget_t *w = win->widgets;
        while (w) {
            gui_widget_draw(w);
            w = (gui_widget_t *)((uint64_t*)w)[15]; /* hack */
        }
        
        win = win->prev;
    }
    
    /* Draw mouse cursor */
    int size = 12;
    for (int y = g_gui_ctx.mouse_y; y < g_gui_ctx.mouse_y + size; y++) {
        vga_put_pixel(g_gui_ctx.mouse_x, y, GUI_WHITE);
        vga_put_pixel(g_gui_ctx.mouse_x + 1, y, GUI_WHITE);
    }
    for (int x = g_gui_ctx.mouse_x; x < g_gui_ctx.mouse_x + size; x++) {
        vga_put_pixel(x, g_gui_ctx.mouse_y, GUI_WHITE);
        vga_put_pixel(x, g_gui_ctx.mouse_y + 1, GUI_WHITE);
    }
}

void gui_run_loop(void) {
    /* Placeholder for main GUI loop */
    gui_render_frame();
}

