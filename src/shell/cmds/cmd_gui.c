#include "gui.h"
#include "gui_widgets.h"
#include "vga.h"
#include "keyboard.h"
#include "mouse.h"
#include "scheduler.h"
#include "string.h"
#include "heap.h"
#include "printf.h"

/* ================================================================
 * Desktop state
 * ================================================================ */

static gui_window_t      *g_desktop  = NULL;
static gui_window_t      *g_file_win = NULL;
static gui_filebrowser_t *g_fb       = NULL;
static int                g_running  = 0;

/* Window drag state */
static gui_window_t *g_drag_win  = NULL;
static int32_t       g_drag_offx = 0;
static int32_t       g_drag_offy = 0;

/* Forward declarations */
static void open_file_window(gui_widget_t *btn);
static void close_file_window(gui_widget_t *btn);
static void exit_gui(gui_widget_t *btn);

/* ================================================================
 * Callbacks
 * ================================================================ */

static void on_file_select(gui_filebrowser_t *fb, const char *name) {
    (void)fb; (void)name;
}

static void open_file_window(gui_widget_t *btn) {
    (void)btn;
    if (g_file_win) return;

    g_file_win = gui_window_create("File Browser", 100, 70, 600, 480, GUI_WINDOW_BG);
    if (!g_file_win) return;

    gui_rect_t br = {108, 102, 584, 410};
    g_fb = gui_filebrowser_create(br, "/");
    if (g_fb) {
        gui_filebrowser_set_on_select(g_fb, on_file_select);
        gui_window_add_widget(g_file_win, gui_filebrowser_get_widget(g_fb));
    }

    /* Close button inside window body */
    gui_rect_t close_r = {608, 74, 64, 20};
    gui_widget_t *close_btn = gui_button_create(close_r, "Close");
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
    g_fb       = NULL;
}

static void exit_gui(gui_widget_t *btn) {
    (void)btn;
    g_running = 0;
}

/* ================================================================
 * Taskbar rendering
 * ================================================================ */

#define TASKBAR_Y  750
#define TASKBAR_H  18

static void draw_taskbar(int32_t mx, int32_t my) {
    (void)mx; (void)my;
    gui_rect_t bar = {0, TASKBAR_Y, 1024, TASKBAR_H};
    gui_window_draw_rect(NULL, bar, GUI_DARK_GRAY);

    /* Separator line */
    gui_rect_t sep = {0, TASKBAR_Y, 1024, 1};
    gui_window_draw_rect(NULL, sep, GUI_GRAY);

    /* Buttons */
    gui_rect_t f = {2, TASKBAR_Y+2, 56, 14};
    gui_window_draw_rect(NULL, f, GUI_COLOR(70,70,100));
    gui_window_draw_text(NULL, 6, TASKBAR_Y+3, "FILES", GUI_WHITE, GUI_COLOR(70,70,100));

    gui_rect_t e = {62, TASKBAR_Y+2, 48, 14};
    gui_window_draw_rect(NULL, e, GUI_COLOR(100,50,50));
    gui_window_draw_text(NULL, 66, TASKBAR_Y+3, "EXIT", GUI_WHITE, GUI_COLOR(100,50,50));

    gui_window_draw_text(NULL, 750, TASKBAR_Y+3,
        "ESC = quit GUI", GUI_COLOR(160,160,160), GUI_DARK_GRAY);
}

/* ================================================================
 * Front-to-back window hit-test
 * ================================================================ */

static gui_window_t* find_window_at(int32_t x, int32_t y) {
    for (gui_window_t *w = gui_get_window_list(); w; w = gui_window_next(w)) {
        if (gui_window_contains_point(w, x, y)) return w;
    }
    return NULL;
}

/* Check if (x,y) is on the window's title-bar close [X] button */
static int on_close_button(gui_window_t *win, int32_t x, int32_t y) {
    gui_rect_t r = gui_window_get_rect(win);
    /* Close button is top-right 22px of 24px title bar */
    return x >= (int32_t)(r.x + r.w - 22) &&
           x <  (int32_t)(r.x + r.w) &&
           y >= r.y && y < r.y + 24;
}

/* Check if (x,y) is in the drag zone (title bar, excluding close button) */
static int on_title_bar(gui_window_t *win, int32_t x, int32_t y) {
    gui_rect_t r = gui_window_get_rect(win);
    return y >= r.y && y < r.y + 24 &&
           x >= r.x && x < (int32_t)(r.x + r.w - 22);
}

/* ================================================================
 * Desktop build
 * ================================================================ */

static void build_desktop(void) {
    g_desktop = gui_window_create("", 0, 0, 1024, TASKBAR_Y, GUI_COLOR(30, 78, 140));
    if (!g_desktop) return;

    /* Desktop icon: Files */
    gui_rect_t fr = {16, 50, 80, 52};
    gui_widget_t *fb = gui_button_create(fr, "Files");
    if (fb) {
        gui_button_set_on_click(fb, open_file_window);
        gui_window_add_widget(g_desktop, fb);
    }

    /* Desktop icon: Exit */
    gui_rect_t er = {16, 116, 80, 52};
    gui_widget_t *ex = gui_button_create(er, "Exit");
    if (ex) {
        gui_button_set_on_click(ex, exit_gui);
        gui_window_add_widget(g_desktop, ex);
    }

    /* Status bar label */
    gui_rect_t lr = {160, 10, 700, 16};
    gui_widget_t *lbl = gui_label_create(lr, "OS Desktop  |  Files: browse filesystem  |  Exit: quit GUI");
    if (lbl) {
        lbl->fg = GUI_COLOR(200, 220, 255);
        lbl->bg = GUI_COLOR(30, 78, 140);
        gui_window_add_widget(g_desktop, lbl);
    }

    gui_add_window(g_desktop);
}

/* ================================================================
 * Main event loop
 * ================================================================ */

void cmd_gui(void) {
    /* Try to allocate software framebuffer if not already initialized */
    if (!vga_is_framebuffer()) {
        if (vga_try_alloc_software_framebuffer() != 0) {
            kprintf("[ERROR] Failed to initialize framebuffer (hardware or software)\n");
            return;
        }
    }

    /* Reset state */
    g_running  = 1;
    g_drag_win = NULL;
    g_file_win = NULL;
    g_desktop  = NULL;
    g_fb       = NULL;

    gui_init();
    build_desktop();

    int      prev_buttons = 0;
    int32_t  px = 512, py = 384;

    while (g_running) {

        /* ── Keyboard ────────────────────────────────────────── */
        while (keyboard_has_input()) {
            char c = keyboard_getchar();
            if (c == 27) { g_running = 0; break; }   /* ESC */

            /* Forward to focused widget */
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
        if (!g_running) break;

        /* ── Mouse ───────────────────────────────────────────── */
        int32_t new_px, new_py;
        mouse_get_pixel_pos(&new_px, &new_py);
        int buttons      = (int)mouse_get_buttons();
        int left_pressed  = (buttons & 1) && !(prev_buttons & 1);
        int left_released = !(buttons & 1) && (prev_buttons & 1);

        px = new_px;
        py = new_py;
        gui_update_mouse(px, py, buttons);

        /* Window dragging */
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
            /* ─ Taskbar clicks ─ */
            if (py >= TASKBAR_Y) {
                if (px >= 2 && px < 58)  open_file_window(NULL);
                if (px >= 62 && px < 110) exit_gui(NULL);
                goto frame;
            }

            /* ─ Window hit-test ─ */
            gui_window_t *hit = find_window_at(px, py);
            if (hit) {
                /* Bring to front */
                if (hit != gui_get_focused_window()) {
                    gui_remove_window(hit);
                    gui_add_window(hit);
                }

                /* Close button? */
                if (gui_window_has_title(hit) && on_close_button(hit, px, py)) {
                    /* If it's the file window, close it */
                    if (hit == g_file_win) close_file_window(NULL);
                    /* else ignore — desktop has no close */
                }
                /* Title bar drag? */
                else if (gui_window_has_title(hit) && on_title_bar(hit, px, py)) {
                    gui_rect_t r = gui_window_get_rect(hit);
                    g_drag_win  = hit;
                    g_drag_offx = px - r.x;
                    g_drag_offy = py - r.y;
                }
                /* Widget click */
                else {
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

        prev_buttons = buttons;

        /* ── Render ──────────────────────────────────────────── */
    frame:
        gui_render_frame();
        draw_taskbar(px, py);
        scheduler_yield();
    }

    /* Teardown */
    gui_shutdown();
    g_file_win = NULL;
    g_fb       = NULL;
    g_desktop  = NULL;
    g_drag_win = NULL;

    kprintf("\nGUI desktop closed.\n");
}

