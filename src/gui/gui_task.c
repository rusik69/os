#include "gui.h"
#include "gui_widgets.h"
#include "vga.h"
#include "mouse.h"
#include "keyboard.h"
#include "scheduler.h"
#include "string.h"
#include "heap.h"
#include "printf.h"

/* ================================================================
 * Desktop state (background kernel GUI task)
 * ================================================================ */

static gui_window_t      *g_desktop  = NULL;
static gui_window_t      *g_file_win = NULL;
static gui_filebrowser_t *g_browser  = NULL;

/* Window drag */
static gui_window_t *g_drag_win  = NULL;
static int32_t       g_drag_offx = 0;
static int32_t       g_drag_offy = 0;

#define TASKBAR_Y 750
#define TASKBAR_H  18

/* ================================================================
 * Callbacks
 * ================================================================ */

static void on_file_select(gui_filebrowser_t *fb, const char *name) {
    (void)fb; (void)name;
}

static void close_file_window(gui_widget_t *btn);

static void open_file_window(gui_widget_t *btn) {
    (void)btn;
    if (g_file_win) return;

    g_file_win = gui_window_create("File Browser", 100, 70, 600, 480, GUI_WINDOW_BG);
    if (!g_file_win) return;

    gui_rect_t br = {108, 102, 584, 410};
    g_browser = gui_filebrowser_create(br, "/");
    if (g_browser) {
        gui_filebrowser_set_on_select(g_browser, on_file_select);
        gui_window_add_widget(g_file_win, gui_filebrowser_get_widget(g_browser));
    }

    gui_rect_t cr = {608, 74, 64, 20};
    gui_widget_t *close_btn = gui_button_create(cr, "Close");
    if (close_btn) {
        gui_button_set_on_click(close_btn, close_file_window);
        gui_window_add_widget(g_file_win, close_btn);
    }

    gui_add_window(g_file_win);
}

static void close_file_window(gui_widget_t *btn) {
    (void)btn;
    if (!g_file_win) return;
    if (g_drag_win == g_file_win) g_drag_win = NULL;
    gui_remove_window(g_file_win);
    gui_window_destroy(g_file_win);
    g_file_win = NULL;
    g_browser  = NULL;
}

/* ================================================================
 * Taskbar
 * ================================================================ */

static void draw_taskbar(void) {
    gui_rect_t bar = {0, TASKBAR_Y, 1024, TASKBAR_H};
    gui_window_draw_rect(NULL, bar, GUI_DARK_GRAY);
    gui_rect_t sep = {0, TASKBAR_Y, 1024, 1};
    gui_window_draw_rect(NULL, sep, GUI_GRAY);

    gui_rect_t f = {2, TASKBAR_Y+2, 56, 14};
    gui_window_draw_rect(NULL, f, GUI_COLOR(70,70,100));
    gui_window_draw_text(NULL, 6, TASKBAR_Y+3, "FILES", GUI_WHITE, GUI_COLOR(70,70,100));
}

/* ================================================================
 * Hit-test helpers
 * ================================================================ */

static gui_window_t* find_window_at(int32_t x, int32_t y) {
    for (gui_window_t *w = gui_get_window_list(); w; w = gui_window_next(w)) {
        if (gui_window_contains_point(w, x, y)) return w;
    }
    return NULL;
}

static int on_close_button(gui_window_t *win, int32_t x, int32_t y) {
    gui_rect_t r = gui_window_get_rect(win);
    return x >= (int32_t)(r.x + r.w - 22) &&
           x <  (int32_t)(r.x + r.w) &&
           y >= r.y && y < r.y + 24;
}

static int on_title_bar(gui_window_t *win, int32_t x, int32_t y) {
    gui_rect_t r = gui_window_get_rect(win);
    return y >= r.y && y < r.y + 24 &&
           x >= r.x && x < (int32_t)(r.x + r.w - 22);
}

/* ================================================================
 * Desktop setup
 * ================================================================ */

static void build_desktop(void) {
    g_desktop = gui_window_create("", 0, 0, 1024, TASKBAR_Y, GUI_COLOR(30, 78, 140));
    if (!g_desktop) return;

    gui_rect_t fr = {16, 50, 80, 52};
    gui_widget_t *files_btn = gui_button_create(fr, "Files");
    if (files_btn) {
        gui_button_set_on_click(files_btn, open_file_window);
        gui_window_add_widget(g_desktop, files_btn);
    }

    gui_rect_t lr = {160, 10, 700, 16};
    gui_widget_t *lbl = gui_label_create(lr, "OS Desktop  |  Files: browse filesystem");
    if (lbl) {
        lbl->fg = GUI_COLOR(200, 220, 255);
        lbl->bg = GUI_COLOR(30, 78, 140);
        gui_window_add_widget(g_desktop, lbl);
    }

    gui_add_window(g_desktop);
}

/* ================================================================
 * Main GUI kernel task
 * ================================================================ */

void gui_task(void) {
    if (!vga_is_framebuffer()) return;

    kprintf("[OK] GUI task started\n");
    gui_init();
    build_desktop();

    int     prev_buttons = 0;
    int32_t px = 512, py = 384;

    for (;;) {
        /* ── Mouse ─────────────────────────────────────── */
        mouse_get_pixel_pos(&px, &py);
        int buttons      = (int)mouse_get_buttons();
        int left_pressed  = (buttons & 1) && !(prev_buttons & 1);
        int left_released = !(buttons & 1) && (prev_buttons & 1);

        gui_update_mouse(px, py, buttons);

        /* Drag */
        if ((buttons & 1) && g_drag_win) {
            gui_rect_t r = gui_window_get_rect(g_drag_win);
            int32_t nx = px - g_drag_offx;
            int32_t ny = py - g_drag_offy;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx + (int32_t)r.w > 1024) nx = 1024 - (int32_t)r.w;
            if (ny + (int32_t)r.h > TASKBAR_Y) ny = TASKBAR_Y - (int32_t)r.h;
            r.x = nx; r.y = ny;
            gui_window_set_rect(g_drag_win, r);
        }
        if (left_released) g_drag_win = NULL;

        if (left_pressed) {
            /* Taskbar */
            if (py >= TASKBAR_Y) {
                if (px >= 2 && px < 58) open_file_window(NULL);
                goto frame;
            }

            gui_window_t *hit = find_window_at(px, py);
            if (hit) {
                if (hit != gui_get_focused_window()) {
                    gui_remove_window(hit);
                    gui_add_window(hit);
                }
                if (gui_window_has_title(hit) && on_close_button(hit, px, py)) {
                    if (hit == g_file_win) close_file_window(NULL);
                } else if (gui_window_has_title(hit) && on_title_bar(hit, px, py)) {
                    gui_rect_t r = gui_window_get_rect(hit);
                    g_drag_win  = hit;
                    g_drag_offx = px - r.x;
                    g_drag_offy = py - r.y;
                } else {
                    gui_widget_t *w = gui_window_first_widget(hit);
                    while (w) {
                        if (gui_widget_contains_point(w, px, py)) {
                            gui_window_set_focused_widget(hit, w);
                            gui_event_t evt;
                            memset(&evt, 0, sizeof(evt));
                            evt.type   = GUI_EVENT_MOUSE_DOWN;
                            evt.button = 1;
                            evt.x      = px;
                            evt.y      = py;
                            gui_widget_on_event(w, &evt);
                            break;
                        }
                        w = w->next;
                    }
                }
            }
        }

        /* ── Keyboard ──────────────────────────────────── */
        if (keyboard_has_input()) {
            char c = keyboard_getchar();
            gui_window_t *fw = gui_get_focused_window();
            if (fw) {
                gui_widget_t *ww = gui_window_get_focused_widget(fw);
                if (ww) {
                    gui_event_t ke;
                    memset(&ke, 0, sizeof(ke));
                    ke.type = GUI_EVENT_CHAR;
                    ke.ch   = c;
                    gui_widget_on_event(ww, &ke);
                }
            }
        }

        prev_buttons = buttons;

        /* ── Render ────────────────────────────────────── */
    frame:
        gui_render_frame();
        draw_taskbar();
        scheduler_yield();
    }
}
