/* gui_shell.c — GUI desktop shell with app launcher */
#include "gui_shell.h"
#include "gui.h"
#include "gui_widgets.h"
#include "gui_apps.h"
#include "gui_draw.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"

/* Forward declarations */
static void close_file_win(gui_widget_t *btn);
static void open_file_win(gui_widget_t *btn);

/* Desktop state */
static gui_window_t      *g_desktop  = NULL;
static gui_window_t      *g_file_win = NULL;
static gui_filebrowser_t *g_fb       = NULL;
static int                g_running  = 0;
static int                g_page     = 1;

/* Window drag state */
static gui_window_t *g_drag_win  = NULL;
static int32_t       g_drag_offx = 0, g_drag_offy = 0;

/* ── App launcher callbacks ── */
#define LAUNCHER(name) static void launch_##name(gui_widget_t *btn) { (void)btn; gui_app_##name##_run(); }

LAUNCHER(draw)
LAUNCHER(widgets)
LAUNCHER(colors)
LAUNCHER(gradient)
LAUNCHER(shapes)
LAUNCHER(checker)
LAUNCHER(info)
LAUNCHER(mandelbrot)
LAUNCHER(calc)
LAUNCHER(rgb_mixer)
LAUNCHER(analog_clock)
LAUNCHER(digital_clock)
LAUNCHER(paint)
LAUNCHER(minesweeper)
LAUNCHER(snake)
LAUNCHER(tetris)
LAUNCHER(lissajous)
LAUNCHER(starfield)
LAUNCHER(fire)
LAUNCHER(plasma)
LAUNCHER(particles)
LAUNCHER(sort_viz)
LAUNCHER(wave)
LAUNCHER(noise)
LAUNCHER(heatmap)
LAUNCHER(fractal_tree)
LAUNCHER(sierpinski)
LAUNCHER(cellular)
LAUNCHER(moire)
LAUNCHER(tunnel)
LAUNCHER(metaballs)
LAUNCHER(snow)
LAUNCHER(gravity)
LAUNCHER(rotozoom)
LAUNCHER(kaleidoscope)
LAUNCHER(eyes)
LAUNCHER(bars)
LAUNCHER(bouncing_ball)
LAUNCHER(spiral)
LAUNCHER(chart_bar)
LAUNCHER(chart_line)
LAUNCHER(chart_pie)
LAUNCHER(typography)
LAUNCHER(flood_fill)
LAUNCHER(wave_interference)
LAUNCHER(clock_dual)
LAUNCHER(heartbeat)

static void page_next(gui_widget_t *btn) { (void)btn; if (g_page < 3) g_page++; else g_page = 1; }
static void page_prev(gui_widget_t *btn) { (void)btn; if (g_page > 1) g_page--; else g_page = 3; }

static void open_file_win(gui_widget_t *btn) {
    (void)btn;
    if (g_file_win) return;
    g_file_win = gui_window_create("File Browser", 100, 70, 600, 480, GUI_WINDOW_BG);
    if (!g_file_win) return;
    gui_rect_t br = {108, 102, 584, 410};
    g_fb = gui_filebrowser_create(br, "/");
    if (g_fb) gui_window_add_widget(g_file_win, gui_filebrowser_get_widget(g_fb));
    gui_rect_t close_r = {608, 74, 64, 20};
    gui_widget_t *close_btn = gui_button_create(close_r, "Close");
    if (close_btn) {
        gui_button_set_on_click(close_btn, (void(*)(gui_widget_t*))close_file_win);
        gui_window_add_widget(g_file_win, close_btn);
    }
    gui_add_window(g_file_win);
}

static void close_file_win(gui_widget_t *btn) {
    (void)btn;
    if (!g_file_win) return;
    if (g_drag_win == g_file_win) g_drag_win = NULL;
    gui_remove_window(g_file_win);
    gui_window_destroy(g_file_win);
    g_file_win = NULL; g_fb = NULL;
}

static void exit_gui(gui_widget_t *btn) { (void)btn; g_running = 0; }

#define TASKBAR_Y  750
#define TASKBAR_H  18

/* Button descriptor type — explicit struct name to avoid type mismatches */
typedef struct _tb_btn {
    const char *label;
    int x, w;
    void (*click)(gui_widget_t*);
} tb_btn_t;

static void draw_taskbar(int32_t mx, int32_t my) {
    gui_rect_t bar = {0, TASKBAR_Y, 1024, TASKBAR_H};
    gui_window_draw_rect(NULL, bar, GUI_DARK_GRAY);
    gui_rect_t sep = {0, TASKBAR_Y, 1024, 1};
    gui_window_draw_rect(NULL, sep, GUI_GRAY);

    tb_btn_t btns_p1[] = {
        {"DRAW",    2, 44, launch_draw},
        {"WIDGET",  48, 50, launch_widgets},
        {"COLORS",  100, 52, launch_colors},
        {"GRAD",    154, 42, launch_gradient},
        {"SHAPES",  198, 54, launch_shapes},
        {"CHECK",   254, 50, launch_checker},
        {"INFO",    306, 42, launch_info},
        {"FILES",   350, 46, open_file_win},
        {"MANDEL",  398, 52, launch_mandelbrot},
        {"CALC",    452, 42, launch_calc},
        {"RGB",     496, 38, launch_rgb_mixer},
        {"CLOCK",   536, 46, launch_analog_clock},
        {"DCLOCK",  584, 52, launch_digital_clock},
        {"PAINT",   638, 46, launch_paint},
        {"MINE",    686, 42, launch_minesweeper},
        {"SNAKE",   730, 46, launch_snake},
        {"TETRIS",  778, 50, launch_tetris},
        {"LISSAJ",  830, 52, launch_lissajous},
        {"STAR",    884, 44, launch_starfield},
        {"PAGE>",   930, 42, page_next},
        {"EXIT",    974, 44, exit_gui},
    };
    tb_btn_t btns_p2[] = {
        {"FIRE",    2, 38, launch_fire},
        {"PLASMA",  42, 50, launch_plasma},
        {"PART",    94, 42, launch_particles},
        {"SORT",    138, 42, launch_sort_viz},
        {"WAVE",    182, 42, launch_wave},
        {"NOISE",   226, 46, launch_noise},
        {"HEAT",    274, 42, launch_heatmap},
        {"TREE",    318, 42, launch_fractal_tree},
        {"SIER",    362, 42, launch_sierpinski},
        {"LIFE",    406, 42, launch_cellular},
        {"MOIRE",   450, 46, launch_moire},
        {"TUNNEL",  498, 50, launch_tunnel},
        {"META",    550, 42, launch_metaballs},
        {"SNOW",    594, 42, launch_snow},
        {"GRAV",    638, 42, launch_gravity},
        {"ZOOM",    682, 42, launch_rotozoom},
        {"KALEID",  726, 52, launch_kaleidoscope},
        {"EYES",    780, 42, launch_eyes},
        {"BARS",    824, 42, launch_bars},
        {"BALL",    868, 42, launch_bouncing_ball},
        {"SPIRAL",  912, 50, launch_spiral},
        {"<PAGE",   964, 42, page_prev},
        {"EXIT",    1008, 44, exit_gui},
    };
    tb_btn_t btns_p3[] = {
        {"CHRBAR",  2, 56, launch_chart_bar},
        {"CHRLIN",  60, 56, launch_chart_line},
        {"CHRPIE",  118, 56, launch_chart_pie},
        {"TYPO",    176, 44, launch_typography},
        {"FLOOD",   222, 48, launch_flood_fill},
        {"INTERF",  272, 52, launch_wave_interference},
        {"DCLK2",   326, 48, launch_clock_dual},
        {"HEART",   376, 48, launch_heartbeat},
        {"<PAGE",   426, 44, page_prev},
        {"EXIT",    472, 44, exit_gui},
    };

    tb_btn_t *btns;
    int n;
    if (g_page == 1) { btns = btns_p1; n = sizeof(btns_p1)/sizeof(btns_p1[0]); }
    else if (g_page == 2) { btns = btns_p2; n = sizeof(btns_p2)/sizeof(btns_p2[0]); }
    else { btns = btns_p3; n = sizeof(btns_p3)/sizeof(btns_p3[0]); }

    for (int i = 0; i < n; i++) {
        gui_color_t c = (btns[i].click == exit_gui) ? GUI_COLOR(100,50,50) : GUI_COLOR(70,70,100);
        if (mx >= btns[i].x && mx < btns[i].x + btns[i].w && my >= TASKBAR_Y && my < TASKBAR_Y + TASKBAR_H)
            c = gui_color_lighten(c, 30);
        gui_rect_t r = {btns[i].x, TASKBAR_Y+2, btns[i].w, TASKBAR_H-2};
        gui_window_draw_rect(NULL, r, c);
        gui_window_draw_text(NULL, btns[i].x + 3, TASKBAR_Y+3, btns[i].label, GUI_WHITE, c);
    }
    gui_window_draw_text(NULL, 964, TASKBAR_Y+3,
        g_page == 1 ? "Pg1/3" : g_page == 2 ? "Pg2/3" : "Pg3/3",
        GUI_COLOR(160,160,160), GUI_DARK_GRAY);
}

static gui_window_t* find_window_at(int32_t x, int32_t y) {
    for (gui_window_t *w = gui_get_window_list(); w; w = gui_window_next(w)) {
        if (gui_window_contains_point(w, x, y)) return w;
    }
    return NULL;
}

static int on_close_button(gui_window_t *win, int32_t x, int32_t y) {
    gui_rect_t r = gui_window_get_rect(win);
    return x >= (int32_t)(r.x + r.w - 22) && x < (int32_t)(r.x + r.w) && y >= r.y && y < r.y + 24;
}

static int on_title_bar(gui_window_t *win, int32_t x, int32_t y) {
    gui_rect_t r = gui_window_get_rect(win);
    return y >= r.y && y < r.y + 24 && x >= r.x && x < (int32_t)(r.x + r.w - 22);
}

static void build_desktop(void) {
    g_desktop = gui_window_create("", 0, 0, 1024, TASKBAR_Y, GUI_COLOR(30, 78, 140));
    if (!g_desktop) return;

    gui_rect_t lr = {160, 10, 700, 16};
    gui_widget_t *lbl = gui_label_create(lr,
        "OS Desktop v2.0  |  47 GUI apps  |  28 drawing primitives  |  15+ widgets");
    if (lbl) { lbl->fg = GUI_COLOR(200, 220, 255); lbl->bg = GUI_COLOR(30, 78, 140);
               gui_window_add_widget(g_desktop, lbl); }

    gui_add_window(g_desktop);
}

void gui_shell_run(void) {
    if (!vga_is_framebuffer()) {
        if (vga_try_alloc_software_framebuffer() != 0) {
            kprintf("[ERROR] Failed to initialize framebuffer\\n");
            return;
        }
    }

    g_running = 1; g_drag_win = NULL; g_file_win = NULL;
    g_desktop = NULL; g_fb = NULL; g_page = 1;
    gui_init();
    build_desktop();

    int prev_buttons = 0;
    int32_t px = 512, py = 384;

    while (g_running) {
        while (keyboard_has_input()) {
            char c = keyboard_getchar();
            if (c == 27) { g_running = 0; break; }
            gui_window_t *fw = gui_get_focused_window();
            if (fw && fw != g_desktop) {
                gui_widget_t *ww = gui_window_get_focused_widget(fw);
                if (ww) {
                    gui_event_t ke; memset(&ke, 0, sizeof(ke));
                    ke.type = GUI_EVENT_CHAR; ke.ch = c;
                    gui_widget_on_event(ww, &ke);
                }
            }
        }
        if (!g_running) break;

        mouse_get_pixel_pos(&px, &py);
        int buttons = (int)mouse_get_buttons();
        int left_pressed  = (buttons & 1) && !(prev_buttons & 1);
        int left_released = !(buttons & 1) && (prev_buttons & 1);

        gui_update_mouse(px, py, buttons);

        if ((buttons & 1) && g_drag_win) {
            gui_rect_t r = gui_window_get_rect(g_drag_win);
            int32_t nx = px - g_drag_offx, ny = py - g_drag_offy;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx + (int32_t)r.w > 1024) nx = 1024 - (int32_t)r.w;
            if (ny + (int32_t)r.h > TASKBAR_Y) ny = TASKBAR_Y - (int32_t)r.h;
            r.x = nx; r.y = ny;
            gui_window_set_rect(g_drag_win, r);
        }
        if (left_released) g_drag_win = NULL;

        if (left_pressed) {
            if (py >= TASKBAR_Y) {
                tb_btn_t btns_p1[] = {
                    {"", 2, 44, launch_draw},{"", 48, 50, launch_widgets},{"", 100, 52, launch_colors},
                    {"", 154, 42, launch_gradient},{"", 198, 54, launch_shapes},{"", 254, 50, launch_checker},
                    {"", 306, 42, launch_info},{"", 350, 46, open_file_win},{"", 398, 52, launch_mandelbrot},
                    {"", 452, 42, launch_calc},{"", 496, 38, launch_rgb_mixer},{"", 536, 46, launch_analog_clock},
                    {"", 584, 52, launch_digital_clock},{"", 638, 46, launch_paint},{"", 686, 42, launch_minesweeper},
                    {"", 730, 46, launch_snake},{"", 778, 50, launch_tetris},{"", 830, 52, launch_lissajous},
                    {"", 884, 44, launch_starfield},{"", 930, 42, page_next},{"", 974, 44, exit_gui},
                };
                tb_btn_t btns_p2[] = {
                    {"", 2, 38, launch_fire},{"", 42, 50, launch_plasma},{"", 94, 42, launch_particles},
                    {"", 138, 42, launch_sort_viz},{"", 182, 42, launch_wave},{"", 226, 46, launch_noise},
                    {"", 274, 42, launch_heatmap},{"", 318, 42, launch_fractal_tree},{"", 362, 42, launch_sierpinski},
                    {"", 406, 42, launch_cellular},{"", 450, 46, launch_moire},{"", 498, 50, launch_tunnel},
                    {"", 550, 42, launch_metaballs},{"", 594, 42, launch_snow},{"", 638, 42, launch_gravity},
                    {"", 682, 42, launch_rotozoom},{"", 726, 52, launch_kaleidoscope},{"", 780, 42, launch_eyes},
                    {"", 824, 42, launch_bars},{"", 868, 42, launch_bouncing_ball},{"", 912, 50, launch_spiral},
                    {"", 964, 42, page_prev},{"", 1008, 44, exit_gui},
                };
                tb_btn_t btns_p3[] = {
                    {"", 2, 56, launch_chart_bar},{"", 60, 56, launch_chart_line},{"", 118, 56, launch_chart_pie},
                    {"", 176, 44, launch_typography},{"", 222, 48, launch_flood_fill},{"", 272, 52, launch_wave_interference},
                    {"", 326, 48, launch_clock_dual},{"", 376, 48, launch_heartbeat},
                    {"", 426, 44, page_prev},{"", 472, 44, exit_gui},
                };
                tb_btn_t *b;
                int nb;
                if (g_page == 1) { b = btns_p1; nb = sizeof(btns_p1)/sizeof(btns_p1[0]); }
                else if (g_page == 2) { b = btns_p2; nb = sizeof(btns_p2)/sizeof(btns_p2[0]); }
                else { b = btns_p3; nb = sizeof(btns_p3)/sizeof(btns_p3[0]); }
                for (int i = 0; i < nb; i++) {
                    if (px >= b[i].x && px < b[i].x + b[i].w) {
                        b[i].click(NULL); goto frame;
                    }
                }
            }
            gui_window_t *hit = find_window_at(px, py);
            if (hit) {
                if (hit != gui_get_focused_window()) {
                    gui_remove_window(hit); gui_add_window(hit);
                }
                gui_set_focused_window(hit);
                if (gui_window_has_title(hit) && on_close_button(hit, px, py)) {
                    if (hit == g_file_win) close_file_win(NULL);
                }
                else if (gui_window_has_title(hit) && on_title_bar(hit, px, py)) {
                    gui_rect_t r = gui_window_get_rect(hit);
                    g_drag_win = hit; g_drag_offx = px - r.x; g_drag_offy = py - r.y;
                }
                else {
                    gui_widget_t *w = gui_window_first_widget(hit);
                    while (w) {
                        if (gui_widget_contains_point(w, px, py)) {
                            gui_window_set_focused_widget(hit, w);
                            gui_event_t evt; memset(&evt, 0, sizeof(evt));
                            evt.type = GUI_EVENT_MOUSE_DOWN; evt.button = 1; evt.x = px; evt.y = py;
                            gui_widget_on_event(w, &evt); break;
                        }
                        w = w->next;
                    }
                }
            }
        }
        prev_buttons = buttons;

    frame:
        gui_render_frame();
        draw_taskbar(px, py);
        scheduler_yield();
    }

    gui_shutdown();
    g_file_win = NULL; g_fb = NULL; g_desktop = NULL; g_drag_win = NULL;
    kprintf("\\nGUI desktop closed.\\n");
}
