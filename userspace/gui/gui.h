#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

/* ===== Core GUI Types ===== */

typedef struct gui_window gui_window_t;
typedef struct gui_widget gui_widget_t;
struct gui_context;
typedef struct gui_context gui_context_t;

typedef uint32_t gui_color_t;
#define GUI_COLOR(r, g, b) (0xFF000000 | ((r) << 16) | ((g) << 8) | (b))
#define GUI_COLOR_ALPHA(r, g, b, a) (((a) << 24) | ((r) << 16) | ((g) << 8) | (b))

#define GUI_BLACK       GUI_COLOR(0, 0, 0)
#define GUI_WHITE       GUI_COLOR(255, 255, 255)
#define GUI_GRAY        GUI_COLOR(128, 128, 128)
#define GUI_LIGHT_GRAY  GUI_COLOR(200, 200, 200)
#define GUI_DARK_GRAY   GUI_COLOR(64, 64, 64)
#define GUI_RED         GUI_COLOR(255, 0, 0)
#define GUI_GREEN       GUI_COLOR(0, 200, 0)
#define GUI_BLUE        GUI_COLOR(0, 0, 255)
#define GUI_CYAN        GUI_COLOR(0, 200, 200)
#define GUI_YELLOW      GUI_COLOR(255, 255, 0)
#define GUI_TITLE_BG    GUI_COLOR(0, 100, 200)
#define GUI_BUTTON_BG   GUI_COLOR(200, 200, 200)
#define GUI_BUTTON_FG   GUI_COLOR(0, 0, 0)
#define GUI_WINDOW_BG   GUI_COLOR(240, 240, 240)
#define GUI_TEXT_FG     GUI_COLOR(0, 0, 0)

typedef struct {
    int32_t x, y;
    uint32_t w, h;
} gui_rect_t;

typedef struct {
    int32_t x, y;
} gui_point_t;

typedef enum {
    GUI_EVENT_MOUSE_MOVE,
    GUI_EVENT_MOUSE_DOWN,
    GUI_EVENT_MOUSE_UP,
    GUI_EVENT_KEY_DOWN,
    GUI_EVENT_KEY_UP,
    GUI_EVENT_CHAR,
    GUI_EVENT_WINDOW_CLOSE,
} gui_event_type_t;

typedef struct {
    gui_event_type_t type;
    int32_t x, y;
    int button;
    uint32_t keycode;
    char ch;
} gui_event_t;

/* ===== Window API ===== */

gui_window_t* gui_window_create(const char *title, int32_t x, int32_t y,
                                 uint32_t w, uint32_t h, gui_color_t bg);
void gui_window_destroy(gui_window_t *win);
void gui_window_set_title(gui_window_t *win, const char *title);
void gui_window_clear(gui_window_t *win, gui_color_t color);
void gui_window_draw_pixel(gui_window_t *win, int32_t x, int32_t y, gui_color_t color);
void gui_window_draw_rect(gui_window_t *win, gui_rect_t rect, gui_color_t color);
void gui_window_draw_rect_outline(gui_window_t *win, gui_rect_t rect,
                                   gui_color_t color, int thickness);
void gui_window_draw_text(gui_window_t *win, int32_t x, int32_t y,
                           const char *text, gui_color_t fg, gui_color_t bg);
void gui_window_draw_string(gui_window_t *win, int32_t x, int32_t y,
                             const char *text);
gui_rect_t gui_window_get_rect(gui_window_t *win);
void gui_window_set_rect(gui_window_t *win, gui_rect_t rect);
void gui_window_set_visible(gui_window_t *win, int visible);
int gui_window_is_visible(gui_window_t *win);
int gui_window_contains_point(gui_window_t *win, int32_t x, int32_t y);
void gui_window_add_widget(gui_window_t *win, gui_widget_t *widget);
gui_widget_t* gui_window_get_focused_widget(gui_window_t *win);
void gui_window_set_focused_widget(gui_window_t *win, gui_widget_t *widget);
gui_widget_t* gui_window_first_widget(gui_window_t *win);
int gui_window_has_title(gui_window_t *win);

/* ===== Widget API ===== */

typedef void (*gui_widget_draw_fn)(gui_widget_t *w);
typedef void (*gui_widget_event_fn)(gui_widget_t *w, gui_event_t *evt);
typedef void (*gui_widget_destroy_fn)(gui_widget_t *w);

struct gui_widget {
    gui_rect_t rect;
    gui_color_t bg, fg;
    int visible;
    struct gui_widget *next;
    void *data;
    gui_widget_draw_fn draw;
    gui_widget_event_fn on_event;
    gui_widget_destroy_fn destroy;
};

gui_widget_t* gui_widget_create(gui_rect_t rect);
void gui_widget_destroy(gui_widget_t *w);
void gui_widget_draw(gui_widget_t *w);
void gui_widget_on_event(gui_widget_t *w, gui_event_t *evt);
int gui_widget_contains_point(gui_widget_t *w, int32_t x, int32_t y);

/* ===== Concrete Widgets ===== */

typedef struct {
    char label[64];
    void (*on_click)(gui_widget_t *w);
} gui_button_data_t;

gui_widget_t* gui_button_create(gui_rect_t rect, const char *label);
void gui_button_set_on_click(gui_widget_t *btn, void (*fn)(gui_widget_t *));

typedef struct {
    char text[256];
    int cursor_pos;
    int max_len;
} gui_textbox_data_t;

gui_widget_t* gui_textbox_create(gui_rect_t rect, int max_len);
const char* gui_textbox_get_text(gui_widget_t *box);
void gui_textbox_set_text(gui_widget_t *box, const char *text);

typedef struct {
    char label[128];
} gui_label_data_t;

gui_widget_t* gui_label_create(gui_rect_t rect, const char *text);
void gui_label_set_text(gui_widget_t *lbl, const char *text);

/* ===== GUI Context API ===== */

int gui_init(void);
void gui_shutdown(void);

void gui_add_window(gui_window_t *win);
void gui_remove_window(gui_window_t *win);
gui_window_t* gui_get_focused_window(void);
void gui_set_focused_window(gui_window_t *win);

void gui_handle_event(gui_event_t *evt);
void gui_render_frame(void);
void gui_update_mouse(int32_t x, int32_t y, int buttons);
void gui_run_loop(void);

/* Walk all windows front-to-back (head = most recently added) */
gui_window_t* gui_get_window_list(void);
gui_window_t* gui_window_next(gui_window_t *win);

/* ===== Syscall wrappers ===== */

#define kprintf printf
#define scheduler_yield() yield()

struct gui_mouse_state {
    int x;
    int y;
    uint8_t buttons;
};

static inline void mouse_get_pixel_pos(int *x, int *y) {
    struct gui_mouse_state s;
    memset(&s, 0, sizeof(s));
    mouse_get_state(&s);
    *x = s.x;
    *y = s.y;
}

static inline uint8_t mouse_get_buttons(void) {
    struct gui_mouse_state s;
    memset(&s, 0, sizeof(s));
    mouse_get_state(&s);
    return s.buttons;
}

static inline int vga_is_framebuffer(void) { return 1; }
static inline int vga_try_alloc_software_framebuffer(void) { return 0; }

/* Stub net/fs/timer functions */

#define TIMER_FREQ 1000
static inline uint64_t timer_get_ticks(void) {
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));
    clock_gettime(0, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static inline void fs_get_usage(const char *path, uint64_t *total, uint32_t *used, uint32_t *free) {
    (void)path;
    if (total) *total = 0;
    if (used) *used = 0;
    if (free) *free = 0;
}

#define FS_MAX_NAME 64
#define FS_NAME_LEN 64
#define FS_TYPE_DIR 1

static inline int vfs_readdir_names(const char *path, char names[][64], int max) {
    (void)path; (void)names; (void)max;
    return 0;
}

static inline int fs_list_names(const char *path, const char *prefix, char names[][64], int max) {
    (void)path; (void)prefix; (void)names; (void)max;
    return 0;
}

static inline int fs_stat(const char *path, uint32_t *size, uint8_t *type) {
    (void)path;
    if (size) *size = 0;
    if (type) *type = 0;
    return 0;
}

/* Heap wrappers */
static inline void *heap_alloc(uint32_t size) { return malloc(size); }
static inline void heap_free(void *p) { free(p); }
#define kmalloc heap_alloc
#define kfree heap_free

#endif /* GUI_H */
