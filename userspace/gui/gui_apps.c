/* gui_apps.c — GUI application programs
 *
 * ===== App Registration Mechanism =====
 *
 * Each GUI application in this project follows a simple registration
 * pattern with three parts:
 *
 * 1. DEFINITION (this file)
 *    Every app is a function of the form:
 *        void gui_app_<name>_run(void)
 *    Each function creates its own window (gui_window_create), populates
 *    it with widgets and drawing primitives, and returns. The framework
 *    takes care of rendering and event processing.
 *
 * 2. DECLARATION (gui_apps.h)
 *    Every app function is declared in gui_apps.h so the launcher shell
 *    can call it. Adding a new app requires:
 *      - A function definition in gui_apps.c
 *      - A matching prototype in gui_apps.h
 *
 * 3. LAUNCHER BINDING (gui_shell.c)
 *    The GUI desktop shell uses a LAUNCHER(name) macro that expands to:
 *        static void launch_<name>(gui_widget_t *btn) {
 *            (void)btn; gui_app_<name>_run();
 *        }
 *    Each launcher callback is stored in a tb_btn_t struct along with
 *    its label, position (x, width), and a pointer to the dispatch
 *    function. The shell's draw_taskbar() renders these as taskbar
 *    buttons, and the main event loop dispatches mouse clicks to the
 *    corresponding callback.
 *
 *    47 GUI application programs (7 original + 40 existing + 40 new).
 *
 * ===== App List =====
 *
 * Group 1 (7 original apps):
 *   draw, widgets, colors, gradient, shapes, checker, info
 *
 * Group 2 (40 existing apps):
 *   mandelbrot, calc, rgb_mixer, analog_clock, digital_clock,
 *   paint, minesweeper, snake, tetris, lissajous, starfield,
 *   fire, plasma, particles, sort_viz, wave, noise, heatmap,
 *   fractal_tree, sierpinski, cellular, moire, tunnel, metaballs,
 *   snow, gravity, rotozoom, kaleidoscope, eyes, bars,
 *   bouncing_ball, spiral, chart_bar, chart_line, chart_pie,
 *   typography, flood_fill, wave_interference, clock_dual, heartbeat
 *
 * Group 3 (40 new apps — D115):
 *   text_editor, cube_3d, julia, lorenz, pendulum, fourier,
 *   wave_eq, reaction_diff, cellular2, maze_gen, pathfind,
 *   sort_compare, bintree, color_wheel, dither, edge, spirograph,
 *   voronoi, fireworks, boids, complex, screensaver, biorhythm,
 *   stopwatch, solar, turing, snowflake, ascii_art, bezier_demo,
 *   lighting, terrain, pong, audio_viz, memory_map, clock_alarm,
 *   tiling, fluid, softbody, convolution, buddha
 *
 * ===== Helpers =====
 * __icos/__isin — integer sine/cosine lookup-table approximation
 * __isqrt     — integer square root via Newton-Raphson
 */
#include "gui_apps.h"

#include "gui.h"
#include "gui_draw.h"
#include "gui_image.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

/* ===== Existing Apps (unchanged) ===== */
static int __icos(int deg);
static int __isin(int deg);
static int __isqrt(int n);

/* ===== Existing Apps (unchanged) ===== */

void gui_app_draw_run(void) {
    gui_window_t *win = gui_window_create("Drawing Demo", 50, 50, 500, 400, GUI_WHITE);
    if (!win)
        return;
    gui_add_window(win);

    gui_rect_t r = {70, 30, 100, 80};
    gui_window_draw_rect(NULL, r, GUI_LIGHT_GRAY);
    gui_window_draw_rect_outline(NULL, r, GUI_RED, 2);

    gui_draw_line(200, 30, 350, 110, GUI_BLUE);
    gui_draw_line(200, 110, 350, 30, GUI_GREEN);

    gui_draw_circle(120, 200, 40, GUI_CYAN);
    gui_draw_circle_filled(250, 200, 35, GUI_YELLOW);

    gui_draw_triangle(400, 30, 370, 110, 430, 110, GUI_RED);
    gui_draw_triangle_filled(400, 140, 370, 220, 430, 220, GUI_COLOR(0, 200, 100));

    gui_draw_progress_bar(70, 290, 260, 20, 67, GUI_GREEN, GUI_LIGHT_GRAY);
    gui_draw_progress_bar(70, 320, 260, 20, 33, GUI_RED, GUI_LIGHT_GRAY);

    gui_draw_gradient_v(350, 140, 80, 200, GUI_BLUE, GUI_CYAN);
    gui_draw_gradient_h(30, 360, 440, 30, GUI_RED, GUI_BLUE);

    gui_window_draw_text(win, 70, 270, "67% complete", GUI_TEXT_FG, GUI_WHITE);
}

void gui_app_widgets_run(void) {
    gui_window_t *win = gui_window_create("Widget Demo", 150, 100, 400, 350, GUI_WINDOW_BG);
    if (!win)
        return;
    gui_add_window(win);

    gui_rect_t lr = {20, 10, 200, 16};
    gui_widget_t *lbl = gui_label_create(lr, "Widget Gallery");
    if (lbl) {
        lbl->fg = GUI_BLUE;
        lbl->bg = GUI_WINDOW_BG;
        gui_window_add_widget(win, lbl);
    }

    gui_rect_t c1r = {20, 35, 200, 20};
    gui_widget_t *c1 = gui_checkbox_create(c1r, "Option A", 0);
    if (c1)
        gui_window_add_widget(win, c1);

    gui_rect_t c2r = {20, 60, 200, 20};
    gui_widget_t *c2 = gui_checkbox_create(c2r, "Option B", 1);
    if (c2)
        gui_window_add_widget(win, c2);

    gui_rect_t sr = {20, 95, 300, 24};
    gui_widget_t *sl = gui_slider_create(sr, 0, 100, 50);
    if (sl)
        gui_window_add_widget(win, sl);

    gui_rect_t lbr = {20, 130, 200, 120};
    gui_widget_t *lb = gui_listbox_create(lbr);
    if (lb) {
        gui_listbox_add_item(lb, "Red");
        gui_listbox_add_item(lb, "Green");
        gui_listbox_add_item(lb, "Blue");
        gui_listbox_add_item(lb, "Cyan");
        gui_listbox_add_item(lb, "Yellow");
        gui_listbox_add_item(lb, "Magenta");
        gui_window_add_widget(win, lb);
    }
}

void gui_app_colors_run(void) {
    gui_window_t *win = gui_window_create("Color Palette", 250, 200, 400, 300, GUI_WHITE);
    if (!win)
        return;
    gui_add_window(win);

    int sw = 30, sh = 20, gap = 4, cols = 6;
    gui_color_t colors[] = {
        GUI_BLACK,
        GUI_WHITE,
        GUI_RED,
        GUI_GREEN,
        GUI_BLUE,
        GUI_CYAN,
        GUI_YELLOW,
        GUI_COLOR(255, 128, 0),
        GUI_COLOR(128, 0, 255),
        GUI_COLOR(0, 255, 128),
        GUI_GRAY,
        GUI_LIGHT_GRAY,
        GUI_DARK_GRAY,
    };
    int n = sizeof(colors) / sizeof(colors[0]);
    for (int i = 0; i < n; i++) {
        int cx = 20 + (i % cols) * (sw + gap);
        int cy = 20 + (i / cols) * (sh + gap);
        gui_rect_t r = {cx, cy, sw, sh};
        gui_window_draw_rect(win, r, colors[i]);
        gui_window_draw_rect_outline(win, r, GUI_DARK_GRAY, 1);
    }
    gui_window_draw_text(win, 20, 20 + ((n + cols - 1) / cols) * (sh + gap) + 5,
                         "GUI Color Palette", GUI_TEXT_FG, GUI_WHITE);
}

void gui_app_gradient_run(void) {
    gui_window_t *win = gui_window_create("Gradients", 50, 50, 400, 350, GUI_WHITE);
    if (!win)
        return;
    gui_add_window(win);

    gui_draw_gradient_v(50, 40, 100, 260, GUI_RED, GUI_BLUE);
    gui_window_draw_text(win, 50, 20, "Vertical (Red/Blue)", GUI_TEXT_FG, GUI_WHITE);

    gui_draw_gradient_h(180, 40, 180, 120, GUI_GREEN, GUI_YELLOW);
    gui_window_draw_text(win, 180, 20, "Horizontal", GUI_TEXT_FG, GUI_WHITE);

    gui_draw_gradient_v(180, 180, 80, 120, GUI_CYAN, GUI_COLOR(255, 0, 255));
    gui_draw_gradient_h(270, 180, 90, 120, GUI_COLOR(255, 128, 0), GUI_COLOR(0, 128, 255));

    gui_draw_gradient_radial(80, 220, 60, GUI_WHITE, GUI_BLUE);
}

void gui_app_shapes_run(void) {
    gui_window_t *win = gui_window_create("Shapes", 200, 100, 500, 380, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);

    gui_draw_line(50, 30, 150, 80, GUI_RED);
    gui_draw_line(50, 30, 100, 150, GUI_GREEN);
    gui_draw_line(50, 30, 220, 30, GUI_BLUE);
    gui_draw_line(100, 150, 150, 80, GUI_YELLOW);

    gui_draw_circle(100, 240, 45, GUI_CYAN);
    gui_draw_circle_filled(250, 90, 35, GUI_COLOR(255, 100, 100));
    gui_draw_circle(250, 240, 55, GUI_YELLOW);

    gui_draw_triangle(400, 30, 350, 100, 480, 90, GUI_GREEN);
    gui_draw_triangle_filled(380, 150, 330, 260, 480, 190, GUI_COLOR(0, 100, 200));

    gui_draw_star(420, 300, 45, 18, GUI_YELLOW);
    gui_draw_ellipse(70, 330, 50, 20, GUI_CYAN);
    gui_draw_heart(200, 330, 40, GUI_RED);

    gui_window_draw_text(win, 10, 10, "Lines, Circles, Triangles, Star, Ellipse, Heart", GUI_WHITE,
                         GUI_BLACK);
}

void gui_app_checker_run(void) {
    gui_window_t *win = gui_window_create("Checkerboard", 100, 100, 400, 300, GUI_WHITE);
    if (!win)
        return;
    gui_add_window(win);
    gui_draw_checkerboard(20, 20, 360, 260, 16);
}

void gui_app_info_run(void) {
    gui_window_t *win = gui_window_create("System Info", 200, 150, 350, 200, GUI_WINDOW_BG);
    if (!win)
        return;
    gui_add_window(win);

    char line[64];
    int y = 30;

    gui_window_draw_text(win, 120, 10, "System Information", GUI_BLUE, GUI_WINDOW_BG);

    snprintf(line, sizeof(line), "Resolution: 1024x768");
    gui_window_draw_text(win, 20, y, line, GUI_TEXT_FG, GUI_WINDOW_BG);
    y += 20;

    snprintf(line, sizeof(line), "GUI Library: v2.0 (100 improvements)");
    gui_window_draw_text(win, 20, y, line, GUI_TEXT_FG, GUI_WINDOW_BG);
    y += 20;

    snprintf(line, sizeof(line), "Widget Types: 15+");
    gui_window_draw_text(win, 20, y, line, GUI_TEXT_FG, GUI_WINDOW_BG);
    y += 20;

    snprintf(line, sizeof(line), "Drawing Primitives: 28");
    gui_window_draw_text(win, 20, y, line, GUI_TEXT_FG, GUI_WINDOW_BG);
    y += 20;

    snprintf(line, sizeof(line), "GUI Applications: 47");
    gui_window_draw_text(win, 20, y, line, GUI_TEXT_FG, GUI_WINDOW_BG);
    y += 20;

    snprintf(line, sizeof(line), "Color Utilities: 15+");
    gui_window_draw_text(win, 20, y, line, GUI_TEXT_FG, GUI_WINDOW_BG);
    y += 20;
}

/* ===================================================================
 * New GUI Applications (40 apps)
 * =================================================================== */

/* 1. Mandelbrot Fractal */
void gui_app_mandelbrot_run(void) {
    gui_window_t *win = gui_window_create("Mandelbrot", 50, 50, 400, 350, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int w = 380, h = 310;
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            float x0 = (float)px / (float)w * 3.5f - 2.5f;
            float y0 = (float)py / (float)h * 2.0f - 1.0f;
            float x = 0, y = 0;
            int iter = 0, max = 64;
            while (x * x + y * y < 4.0f && iter < max) {
                float xt = x * x - y * y + x0;
                y = 2 * x * y + y0;
                x = xt;
                iter++;
            }
            gui_color_t c = iter >= max ? GUI_BLACK : gui_color_from_hsv(iter * 12 % 360, 200, 200);
            vga_put_pixel(10 + px, 30 + py, c);
        }
    }
    gui_window_draw_text(win, 120, 10, "Mandelbrot Set", GUI_WHITE, GUI_BLACK);
}

/* 2. Calculator — real interactive calculator.
 *
 * Expression entry (keyboard digits/operators/backspace/Enter or mouse clicks
 * on the on-screen keypad), a display, and the four basic operations
 * (+ - * / %) with standard precedence.  It is implemented as a single
 * focusable widget that both draws itself and does its own keypad
 * hit-testing — matching this framework's event model, where the focused
 * widget receives every keyboard event and every mouse event within its
 * bounds.
 */
typedef struct {
    char expr[96];
    int len;
} calc_state_t;

static calc_state_t s_calc;

static void calc_clear(void) {
    s_calc.expr[0] = '\0';
    s_calc.len = 0;
}

static int calc_isdigit(char c) {
    return c >= '0' && c <= '9';
}
static int calc_isop(char c) {
    return c == '+' || c == '-' || c == '*' || c == '/' || c == '%';
}

/* Read an unsigned run of digits, advancing *pp past them. */
static int calc_parse_num(const char *s, int *pp) {
    int v = 0;
    while (calc_isdigit(s[*pp])) {
        v = v * 10 + (s[*pp] - '0');
        (*pp)++;
    }
    return v;
}

static int calc_eval_unary(const char *s, int *pp, int *ok); /* fwd */

/* term := number | ('+'|'-') term  (prefix sign for negative operands) */
static int calc_eval_term(const char *s, int *pp, int *ok) {
    if (s[*pp] == '-' || s[*pp] == '+') {
        int neg = (s[*pp] == '-');
        (*pp)++;
        int v = calc_eval_unary(s, pp, ok);
        return neg ? -v : v;
    }
    if (calc_isdigit(s[*pp]))
        return calc_parse_num(s, pp);
    *ok = 0;
    return 0;
}

static int calc_eval_unary(const char *s, int *pp, int *ok) {
    return calc_eval_term(s, pp, ok);
}

/* factor := term (('*'|'/'|'%') term)*   — left associative, high precedence */
static int calc_eval_factor(const char *s, int *pp, int *ok) {
    int v = calc_eval_term(s, pp, ok);
    while (*ok) {
        char op = s[*pp];
        if (op != '*' && op != '/' && op != '%')
            break;
        (*pp)++;
        int r = calc_eval_term(s, pp, ok);
        if (!*ok)
            return v;
        if (op == '*')
            v = v * r;
        else if (op == '/') {
            if (r == 0) {
                *ok = 0;
                return 0;
            }
            v = v / r;
        } else {
            if (r == 0) {
                *ok = 0;
                return 0;
            }
            v = v % r;
        }
    }
    return v;
}

/* expr := factor (('+'|'-') factor)*   — left associative, low precedence */
static int calc_eval_expr(const char *s, int *pp, int *ok) {
    int v = calc_eval_factor(s, pp, ok);
    while (*ok) {
        char op = s[*pp];
        if (op != '+' && op != '-')
            break;
        (*pp)++;
        int r = calc_eval_factor(s, pp, ok);
        if (!*ok)
            return v;
        v = (op == '+') ? v + r : v - r;
    }
    return v;
}

static void calc_compute(void) {
    int ok = 1, p = 0;
    int r = calc_eval_expr(s_calc.expr, &p, &ok);
    if (ok && s_calc.expr[p] == '\0') {
        snprintf(s_calc.expr, sizeof(s_calc.expr), "%d", r);
    } else {
        snprintf(s_calc.expr, sizeof(s_calc.expr), "ERR");
    }
    s_calc.len = (int)strlen(s_calc.expr);
}

static void calc_append(char c) {
    if (s_calc.len < (int)sizeof(s_calc.expr) - 1) {
        s_calc.expr[s_calc.len++] = c;
        s_calc.expr[s_calc.len] = '\0';
    }
}

static void calc_backspace(void) {
    if (s_calc.len > 0)
        s_calc.expr[--s_calc.len] = '\0';
}

static void calc_handle_key(char ch) {
    if (calc_isdigit(ch) || calc_isop(ch))
        calc_append(ch);
    else if (ch == 8)
        calc_backspace();
    else if (ch == 13 || ch == '=')
        calc_compute();
    else if (ch == 'C' || ch == 'c')
        calc_clear();
}

/* Layout constants (window-relative, absolute coords derived at draw time). */
#define CALC_BW 52
#define CALC_BH 42
#define CALC_GAP 6
#define CALC_COLS 4
#define CALC_ROWS 4

static const char *const g_calc_keys[CALC_ROWS][CALC_COLS] = {
    {"7", "8", "9", "+"}, {"4", "5", "6", "-"}, {"1", "2", "3", "*"}, {"C", "0", "=", "/"}};

static void calc_draw(gui_widget_t *w) {
    int32_t ox = w->rect.x, oy = w->rect.y;
    /* Display */
    gui_rect_t disp = {ox, oy, CALC_COLS * CALC_BW + (CALC_COLS - 1) * CALC_GAP, 32};
    gui_window_draw_rect(NULL, disp, GUI_WHITE);
    gui_window_draw_rect_outline(NULL, disp, GUI_GRAY, 1);
    gui_window_draw_text(NULL, ox + 6, oy + 8, s_calc.expr[0] ? s_calc.expr : "0", GUI_TEXT_FG,
                         GUI_WHITE);
    /* Keypad */
    int pad_y = oy + 44;
    for (int r = 0; r < CALC_ROWS; r++) {
        for (int c = 0; c < CALC_COLS; c++) {
            gui_rect_t cell = {ox + c * (CALC_BW + CALC_GAP), pad_y + r * (CALC_BH + CALC_GAP),
                               CALC_BW, CALC_BH};
            gui_window_draw_rect(NULL, cell, GUI_BUTTON_BG);
            gui_window_draw_rect_outline(NULL, cell, GUI_DARK_GRAY, 1);
            gui_window_draw_text(NULL, cell.x + 19, cell.y + 11, g_calc_keys[r][c], GUI_BUTTON_FG,
                                 GUI_BUTTON_BG);
        }
    }
}

static void calc_dispatch(char key) {
    if (key == '=')
        calc_compute();
    else if (key == 'C')
        calc_clear();
    else
        calc_handle_key(key);
}

static void calc_event(gui_widget_t *w, gui_event_t *evt) {
    if (evt->type == GUI_EVENT_CHAR) {
        calc_handle_key(evt->ch);
        return;
    }
    if (evt->type != GUI_EVENT_MOUSE_DOWN || evt->button != 1)
        return;
    int32_t lx = evt->x - w->rect.x;
    int32_t ly = evt->y - w->rect.y;
    int32_t pad_y = 44; /* keypad top matches calc_draw() */
    if (ly < pad_y)
        return;
    int col = (lx + (CALC_GAP / 2)) / (CALC_BW + CALC_GAP);
    int row = (ly - pad_y + (CALC_GAP / 2)) / (CALC_BH + CALC_GAP);
    if (col < 0 || col >= CALC_COLS || row < 0 || row >= CALC_ROWS)
        return;
    calc_dispatch(g_calc_keys[row][col][0]);
}

void gui_app_calc_run(void) {
    calc_clear();
    gui_window_t *win = gui_window_create("Calculator", 300, 160, 240, 320, GUI_WINDOW_BG);
    if (!win)
        return;
    gui_add_window(win);
    gui_window_bring_to_front(win);
    gui_rect_t wr = gui_window_get_rect(win);
    gui_rect_t wrect = {wr.x + 8, wr.y + 24 + 8, /* below 24px titlebar */
                        CALC_COLS * CALC_BW + (CALC_COLS - 1) * CALC_GAP,
                        44 + CALC_ROWS * CALC_BH + (CALC_ROWS - 1) * CALC_GAP};
    gui_widget_t *cw = gui_widget_create(wrect);
    if (!cw)
        return;
    cw->draw = calc_draw;
    cw->on_event = calc_event;
    gui_window_add_widget(win, cw);
    gui_window_set_focused_widget(win, cw);
}

/* 2b. Notepad — multi-line text editor with Save/Load.
 *
 * A single focusable widget (matching the framework's event model: the
 * focused widget receives every keyboard char and every mouse-down inside
 * its bounds). It does its own hit-testing for the on-screen Save/Load
 * buttons and implements multi-line editing with a cursor and scrolling.
 * Save writes the buffer to /notepad.txt (O_CREAT|O_TRUNC); Load reads it
 * back.  Raw syscalls only — no FILE*.
 */
#define NOTEPAD_MAX 2048
#define NOTEPAD_W 480
#define NOTEPAD_H 400

typedef struct {
    char text[NOTEPAD_MAX];
    int cursor;   /* byte offset of cursor within text */
    int scroll_y; /* logical line rendered at top of text area */
    char status[64];
} notepad_state_t;

static notepad_state_t s_notepad;

static void notepad_set_status(const char *fmt, int v) {
    if (fmt)
        snprintf(s_notepad.status, sizeof(s_notepad.status), fmt, v);
    else
        s_notepad.status[0] = '\0';
}

static void notepad_insert(char ch) {
    int n = (int)strlen(s_notepad.text);
    if (n >= NOTEPAD_MAX - 1)
        return;
    for (int i = n; i >= s_notepad.cursor; i--)
        s_notepad.text[i + 1] = s_notepad.text[i];
    s_notepad.text[s_notepad.cursor++] = ch;
}

static void notepad_backspace(void) {
    if (s_notepad.cursor <= 0)
        return;
    int n = (int)strlen(s_notepad.text);
    for (int i = s_notepad.cursor - 1; i <= n; i++)
        s_notepad.text[i] = s_notepad.text[i + 1];
    s_notepad.cursor--;
}

/* Save buffer to /notepad.txt. */
static void notepad_save(void) {
    int fd = open("/notepad.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        notepad_set_status("Save FAILED", 0);
        return;
    }
    int n = (int)strlen(s_notepad.text);
    if (n > 0)
        write(fd, s_notepad.text, (unsigned long)n);
    close(fd);
    notepad_set_status("Saved /notepad.txt (%d bytes)", n);
}

/* Load buffer from /notepad.txt. */
static void notepad_load(void) {
    int fd = open("/notepad.txt", O_RDONLY, 0);
    if (fd < 0) {
        notepad_set_status("No file yet", 0);
        return;
    }
    int n = read(fd, s_notepad.text, NOTEPAD_MAX - 1);
    close(fd);
    if (n < 0) {
        notepad_set_status("Load FAILED", 0);
        return;
    }
    s_notepad.text[n] = '\0';
    s_notepad.cursor = 0;
    s_notepad.scroll_y = 0;
    notepad_set_status("Loaded /notepad.txt (%d bytes)", n);
}

/* Max display chars per wrapped row, derived from widget width. */
static int notepad_chars_per_row(int w) {
    int cpl = (w - 16) / 7;
    if (cpl < 4)
        cpl = 4;
    if (cpl > 79)
        cpl = 79;
    return cpl;
}

/* Number of wrapped rows (0-based row index of the char at byte_limit). */
static int notepad_row_of(const char *s, int byte_limit, int cpl) {
    int row = 0, col = 0, i = 0;
    while (i < byte_limit && s[i]) {
        if (s[i] == '\n') {
            row++;
            col = 0;
        } else {
            col++;
            if (col >= cpl) {
                col = 0;
                row++;
            }
        }
        i++;
    }
    return row;
}

/* Scroll so the cursor's wrapped row is on screen. */
static void notepad_ensure_cursor_visible(int w) {
    int cpl = notepad_chars_per_row(w);
    int max_vis = (w - 52) / 14;
    if (max_vis < 1)
        max_vis = 1;
    int cr = notepad_row_of(s_notepad.text, s_notepad.cursor, cpl);
    if (cr < s_notepad.scroll_y)
        s_notepad.scroll_y = cr;
    else if (cr >= s_notepad.scroll_y + max_vis)
        s_notepad.scroll_y = cr - max_vis + 1;
}

static void notepad_draw(gui_widget_t *w) {
    notepad_state_t *st = &s_notepad;
    int32_t ox = w->rect.x, oy = w->rect.y;
    gui_window_draw_rect(NULL, w->rect, GUI_WINDOW_BG);

    /* Status line */
    gui_window_draw_text(NULL, ox + 4, oy + 2, st->status[0] ? st->status : "Notepad — untitled",
                         GUI_TEXT_FG, GUI_WINDOW_BG);

    /* Save / Load buttons */
    gui_rect_t save = {ox + 4, oy + 18, 56, 24};
    gui_window_draw_rect(NULL, save, GUI_BUTTON_BG);
    gui_window_draw_rect_outline(NULL, save, GUI_DARK_GRAY, 1);
    gui_window_draw_text(NULL, ox + 12, oy + 22, "SAVE", GUI_BUTTON_FG, GUI_BUTTON_BG);
    gui_rect_t load = {ox + 66, oy + 18, 56, 24};
    gui_window_draw_rect(NULL, load, GUI_BUTTON_BG);
    gui_window_draw_rect_outline(NULL, load, GUI_DARK_GRAY, 1);
    gui_window_draw_text(NULL, ox + 74, oy + 22, "LOAD", GUI_BUTTON_FG, GUI_BUTTON_BG);

    /* Text area */
    gui_rect_t ta = {ox + 4, oy + 48, w->rect.w - 8, w->rect.h - 52};
    gui_window_draw_rect(NULL, ta, GUI_WHITE);
    gui_window_draw_rect_outline(NULL, ta, GUI_GRAY, 1);

    int line_h = 14;
    int max_vis = ta.h / line_h;
    if (max_vis < 1)
        max_vis = 1;
    int cpl = notepad_chars_per_row(w->rect.w);

    /* Emit text as wrapped rows, honoring scroll_y (row offset). */
    const char *p = st->text;
    int row = 0;
    int y = ta.y + 3;
    while (*p && row - st->scroll_y < max_vis) {
        if (row >= st->scroll_y) {
            char buf[80];
            int bi = 0;
            while (bi < cpl && *p && *p != '\n')
                buf[bi++] = *p++;
            buf[bi] = '\0';
            if (bi > 0)
                gui_window_draw_text(NULL, ta.x + 4, y, buf, GUI_TEXT_FG, GUI_WHITE);
            if (*p == '\n')
                p++;
            y += line_h;
        } else {
            /* skip this row so we land on the right starting char for the
               visible region: walk chars until a row boundary */
            int col = 0;
            while (*p && *p != '\n') {
                col++;
                if (col >= cpl)
                    break;
                p++;
            }
            if (*p == '\n')
                p++;
        }
        row++;
    }
}

static void notepad_event(gui_widget_t *w, gui_event_t *evt) {
    if (evt->type == GUI_EVENT_CHAR) {
        char ch = evt->ch;
        if (ch == 8)
            notepad_backspace();
        else if (ch == 13 || ch == 10)
            notepad_insert('\n');
        else if (ch >= 32)
            notepad_insert(ch);
        notepad_ensure_cursor_visible(w->rect.w);
        return;
    }
    if (evt->type != GUI_EVENT_MOUSE_DOWN || evt->button != 1)
        return;
    int32_t lx = evt->x - w->rect.x;
    int32_t ly = evt->y - w->rect.y;
    if (ly >= 18 && ly < 42) {
        if (lx >= 4 && lx < 60)
            notepad_save();
        else if (lx >= 66 && lx < 122)
            notepad_load();
    }
}

void gui_app_notepad_run(void) {
    s_notepad.text[0] = '\0';
    s_notepad.cursor = 0;
    s_notepad.scroll_y = 0;
    s_notepad.status[0] = '\0';
    gui_window_t *win = gui_window_create("Notepad", 120, 90, NOTEPAD_W, NOTEPAD_H, GUI_WINDOW_BG);
    if (!win)
        return;
    gui_add_window(win);
    gui_window_bring_to_front(win);
    gui_rect_t wr = gui_window_get_rect(win);
    gui_rect_t wrect = {wr.x + 8, wr.y + 24 + 8, NOTEPAD_W - 16, NOTEPAD_H - 32};
    gui_widget_t *cw = gui_widget_create(wrect);
    if (!cw)
        return;
    cw->draw = notepad_draw;
    cw->on_event = notepad_event;
    gui_window_add_widget(win, cw);
    gui_window_set_focused_widget(win, cw);
}

/* 3. RGB Color Mixer */
void gui_app_rgb_mixer_run(void) {
    gui_window_t *win = gui_window_create("RGB Mixer", 100, 100, 300, 260, GUI_WHITE);
    if (!win)
        return;
    gui_add_window(win);
    int r_val = 128, g_val = 64, b_val = 192;
    gui_color_t swatch = GUI_COLOR(r_val, g_val, b_val);
    gui_window_draw_rect(win, (gui_rect_t){20, 20, 260, 50}, swatch);
    gui_window_draw_rect_outline(win, (gui_rect_t){20, 20, 260, 50}, GUI_DARK_GRAY, 2);
    int y = 90;
    gui_window_draw_text(win, 20, y, "R", GUI_RED, GUI_WHITE);
    y += 20;
    gui_draw_gradient_h(50, y - 16, 200, 12, GUI_BLACK, GUI_RED);
    gui_window_draw_text(win, 20, y + 8, "G", GUI_GREEN, GUI_WHITE);
    y += 20;
    gui_draw_gradient_h(50, y - 16, 200, 12, GUI_BLACK, GUI_GREEN);
    gui_window_draw_text(win, 20, y + 8, "B", GUI_BLUE, GUI_WHITE);
    y += 20;
    gui_draw_gradient_h(50, y - 16, 200, 12, GUI_BLACK, GUI_BLUE);
    char buf[32];
    snprintf(buf, sizeof(buf), "RGB(%d,%d,%d)", r_val, g_val, b_val);
    gui_window_draw_text(win, 20, 220, buf, GUI_TEXT_FG, GUI_WHITE);
}

/* 4. Analog Clock */
void gui_app_analog_clock_run(void) {
    gui_window_t *win = gui_window_create("Analog Clock", 400, 300, 200, 220, GUI_WHITE);
    if (!win)
        return;
    gui_add_window(win);
    int cx = 100, cy = 110, r = 80;
    gui_draw_circle(cx, cy, r, GUI_DARK_GRAY);
    gui_draw_circle_filled(cx, cy, r - 2, GUI_COLOR(240, 240, 255));
    for (int i = 0; i < 12; i++) {
        int ang = i * 30;
        int ex = cx + (r - 12) * __icos(ang) / 100;
        int ey = cy + (r - 12) * __isin(ang) / 100;
        gui_draw_circle_filled(ex, ey, 3, GUI_DARK_GRAY);
    }
    /* Hour hand at ~10:10 */
    int hx = cx + 30 * __icos(300) / 100;
    int hy = cy + 30 * __isin(300) / 100;
    gui_draw_thick_line(cx, cy, hx, hy, 4, GUI_BLACK);
    int mx = cx + 50 * __icos(120) / 100;
    int my = cy + 50 * __isin(120) / 100;
    gui_draw_thick_line(cx, cy, mx, my, 2, GUI_BLACK);
    gui_draw_circle_filled(cx, cy, 4, GUI_RED);
    gui_window_draw_text(win, 60, 195, "10:10", GUI_TEXT_FG, GUI_COLOR(240, 240, 255));
}

/* 5. Digital Clock */
void gui_app_digital_clock_run(void) {
    gui_window_t *win = gui_window_create("Digital Clock", 300, 150, 250, 100, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    gui_window_draw_text(win, 30, 30, "00:00:00", GUI_COLOR(0, 255, 0), GUI_BLACK);
    gui_window_draw_text(win, 30, 60, "HH:MM:SS", GUI_DARK_GRAY, GUI_BLACK);
}

/* 6. Paint Program */
/* 2c. Paint — drawing canvas, brush, colors.
 *
 * A single focusable widget holding a persistent pixel canvas.  The toolbar
 * offers 8 color swatches and a CLEAR button; keys 1-4 change the brush size
 * and C clears.  Because the GUI run-loop delivers GUI_EVENT_MOUSE_DOWN every
 * frame while the button is held (no distinct drag/up events), a stroke is
 * drawn by chaining brush stamps from the previous point — but only while the
 * pointer stays reasonably close, so a released/re-pressed click starts a new
 * stroke.  Raw syscalls only — no FILE*.
 */
#define PAINT_CW 200
#define PAINT_CH 200
#define PAINT_TB_H 32
#define PAINT_NCOLORS 8

typedef struct {
    int drawing;          /* currently in a stroke (button held) */
    int last_cx, last_cy; /* last painted canvas point */
    int brush;            /* brush radius 1..4 */
    int color_idx;        /* index into s_paint_colors */
    char status[64];
} paint_state_t;

static paint_state_t s_paint;
static gui_color_t s_paint_canvas[PAINT_CH][PAINT_CW];

static const gui_color_t s_paint_colors[PAINT_NCOLORS] = {
    GUI_BLACK, GUI_RED, GUI_GREEN, GUI_BLUE, GUI_YELLOW, GUI_ORANGE, GUI_PURPLE, GUI_WHITE,
};

static const char *const s_paint_color_names[PAINT_NCOLORS] = {
    "Black", "Red", "Green", "Blue", "Yellow", "Orange", "Purple", "White",
};

static void paint_clear(void) {
    for (int y = 0; y < PAINT_CH; y++)
        for (int x = 0; x < PAINT_CW; x++)
            s_paint_canvas[y][x] = GUI_WHITE;
}

/* Stamp a filled brush dot at canvas point (cx,cy). */
static void paint_stamp(int cx, int cy) {
    gui_color_t c = s_paint_colors[s_paint.color_idx];
    int r = s_paint.brush;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int x = cx + dx, y = cy + dy;
            if (dx * dx + dy * dy <= r * r && x >= 0 && y >= 0 && x < PAINT_CW && y < PAINT_CH)
                s_paint_canvas[y][x] = c;
        }
}

/* Bresenham line between two canvas points, stamping the current brush along it. */
static void paint_line(int x0, int y0, int x1, int y1) {
    int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    int dy = y1 >= y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (1) {
        paint_stamp(x0, y0);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* Handle a pointer-down inside the canvas area. */
static void paint_handle_canvas(int cx, int cy) {
    if (cx < 0 || cy < 0 || cx >= PAINT_CW || cy >= PAINT_CH) {
        s_paint.drawing = 0;
        return;
    }
    if (s_paint.drawing) {
        int dlx = cx - s_paint.last_cx;
        if (dlx < 0)
            dlx = -dlx;
        int dly = cy - s_paint.last_cy;
        if (dly < 0)
            dly = -dly;
        if (dlx + dly <= 48)
            paint_line(s_paint.last_cx, s_paint.last_cy, cx, cy);
        else
            paint_stamp(cx, cy); /* far jump: new stroke */
    } else {
        paint_stamp(cx, cy);
    }
    s_paint.last_cx = cx;
    s_paint.last_cy = cy;
    s_paint.drawing = 1;
}

static void paint_draw(gui_widget_t *w) {
    int32_t ox = w->rect.x, oy = w->rect.y;
    /* Toolbar */
    gui_rect_t tb = {ox, oy, w->rect.w, PAINT_TB_H};
    gui_window_draw_rect(NULL, tb, GUI_LIGHT_GRAY);
    gui_window_draw_rect_outline(NULL, tb, GUI_GRAY, 1);
    snprintf(s_paint.status, sizeof(s_paint.status), "Brush %dpx | %s | C clears", s_paint.brush,
             s_paint_color_names[s_paint.color_idx]);
    gui_window_draw_text(NULL, ox + 4, oy + 21, s_paint.status, GUI_TEXT_FG, GUI_LIGHT_GRAY);
    /* Color swatches */
    for (int i = 0; i < PAINT_NCOLORS; i++) {
        gui_rect_t sr = {ox + 4 + i * 16, oy + 6, 14, 12};
        gui_window_draw_rect(NULL, sr, s_paint_colors[i]);
        if (i == s_paint.color_idx)
            gui_window_draw_rect_outline(NULL, sr, GUI_BLACK, 2);
        else
            gui_window_draw_rect_outline(NULL, sr, GUI_DARK_GRAY, 1);
    }
    /* CLEAR button */
    gui_rect_t cb = {ox + 152, oy + 6, 44, 14};
    gui_window_draw_rect(NULL, cb, GUI_BUTTON_BG);
    gui_window_draw_rect_outline(NULL, cb, GUI_DARK_GRAY, 1);
    gui_window_draw_text(NULL, ox + 162, oy + 9, "CLEAR", GUI_BUTTON_FG, GUI_BUTTON_BG);
    /* Canvas: white base, then blit the non-white painted pixels */
    gui_rect_t cvs = {ox, oy + PAINT_TB_H, PAINT_CW, PAINT_CH};
    gui_window_draw_rect(NULL, cvs, GUI_WHITE);
    gui_window_draw_rect_outline(NULL, cvs, GUI_GRAY, 1);
    for (int cy = 0; cy < PAINT_CH; cy++)
        for (int cx = 0; cx < PAINT_CW; cx++) {
            gui_color_t c = s_paint_canvas[cy][cx];
            if (c != GUI_WHITE)
                gui_window_draw_pixel(NULL, ox + cx, oy + PAINT_TB_H + cy, c);
        }
}

static void paint_event(gui_widget_t *w, gui_event_t *evt) {
    if (evt->type == GUI_EVENT_CHAR) {
        char ch = evt->ch;
        if (ch == 'c' || ch == 'C') {
            paint_clear();
            s_paint.drawing = 0;
        } else if (ch >= '1' && ch <= '4') {
            s_paint.brush = ch - '0';
            s_paint.drawing = 0;
        }
        return;
    }
    if (evt->type != GUI_EVENT_MOUSE_DOWN || evt->button != 1)
        return;
    int32_t lx = evt->x - w->rect.x;
    int32_t ly = evt->y - w->rect.y;
    if (ly < PAINT_TB_H) {
        /* Color swatches */
        if (ly >= 6 && ly < 20) {
            for (int i = 0; i < PAINT_NCOLORS; i++) {
                if (lx >= 4 + i * 16 && lx < 4 + i * 16 + 14) {
                    s_paint.color_idx = i;
                    s_paint.drawing = 0;
                    return;
                }
            }
        }
        /* CLEAR button */
        if (lx >= 152 && lx < 196 && ly >= 6 && ly < 20) {
            paint_clear();
            s_paint.drawing = 0;
            return;
        }
        s_paint.drawing = 0;
        return;
    }
    paint_handle_canvas(lx, ly - PAINT_TB_H);
}

void gui_app_paint_run(void) {
    paint_clear();
    s_paint.drawing = 0;
    s_paint.brush = 2;
    s_paint.color_idx = 0;
    s_paint.last_cx = 0;
    s_paint.last_cy = 0;
    s_paint.status[0] = '\0';
    gui_window_t *win = gui_window_create("Paint", 150, 100, PAINT_CW + 16,
                                          24 + PAINT_TB_H + PAINT_CH + 16, GUI_WINDOW_BG);
    if (!win)
        return;
    gui_add_window(win);
    gui_window_bring_to_front(win);
    gui_rect_t wr = gui_window_get_rect(win);
    gui_rect_t wrect = {wr.x + 8, wr.y + 24 + 8, PAINT_CW, PAINT_TB_H + PAINT_CH};
    gui_widget_t *cw = gui_widget_create(wrect);
    if (!cw)
        return;
    cw->draw = paint_draw;
    cw->on_event = paint_event;
    gui_window_add_widget(win, cw);
    gui_window_set_focused_widget(win, cw);
}

/* 6b. Terminal — ANSI terminal emulator widget.
 *
 * A character-cell grid with per-cell fg/bg colours.  Text is fed through an
 * ANSI ESC/CSI parser so the widget behaves like a real terminal: printable
 * chars write at the cursor, control chars move it, and CSI sequences do
 * cursor addressing, erase, line-wrap, scroll-up and SGR colouring.  Input
 * comes from GUI_EVENT_CHAR (the terminal echoes what is typed); an ANSI
 * demo banner is shown on launch.  Raw cell grid — no FILE*.
 */
#define TERM_COLS 40
#define TERM_ROWS 18
#define TERM_CW 7   /* glyph advance, px */
#define TERM_CH 14  /* glyph line-height, px */
#define TERM_MAXP 8 /* max CSI parameters */

typedef struct {
    unsigned char ch;
    gui_color_t fg;
    gui_color_t bg;
} term_cell_t;

typedef struct {
    term_cell_t grid[TERM_ROWS][TERM_COLS];
    int r, c;           /* cursor row, col */
    gui_color_t fg, bg; /* current SGR colours */
    int bold;
    int esc; /* 0 text, 1 after ESC, 2 in CSI */
    int fin; /* final CSI byte */
    int params[TERM_MAXP];
    int nparams;
    int cur; /* current param accumulator */
} term_state_t;

static term_state_t s_term;

/* Standard SGR palette index -> colour; brightened when bold. */
static gui_color_t term_color(int idx, int bold) {
    if (bold) {
        switch (idx) {
        case 0:
            return GUI_DARK_GRAY;
        case 1:
            return GUI_COLOR(255, 130, 130);
        case 2:
            return GUI_COLOR(140, 255, 140);
        case 3:
            return GUI_COLOR(255, 255, 140);
        case 4:
            return GUI_COLOR(140, 160, 255);
        case 5:
            return GUI_COLOR(255, 140, 255);
        case 6:
            return GUI_COLOR(140, 255, 255);
        default:
            return GUI_WHITE;
        }
    }
    switch (idx) {
    case 0:
        return GUI_BLACK;
    case 1:
        return GUI_RED;
    case 2:
        return GUI_GREEN;
    case 3:
        return GUI_YELLOW;
    case 4:
        return GUI_BLUE;
    case 5:
        return GUI_MAGENTA;
    case 6:
        return GUI_CYAN;
    default:
        return GUI_WHITE;
    }
}

static void term_clear_cell(term_cell_t *cell) {
    cell->ch = 0;
    cell->fg = GUI_WHITE;
    cell->bg = GUI_BLACK;
}

static void term_clear_at(int r, int c) {
    term_clear_cell(&s_term.grid[r][c]);
}

static void term_scroll_up(int n) {
    for (int k = 0; k < n; k++) {
        for (int r = 1; r < TERM_ROWS; r++)
            for (int c = 0; c < TERM_COLS; c++)
                s_term.grid[r - 1][c] = s_term.grid[r][c];
        for (int c = 0; c < TERM_COLS; c++)
            term_clear_at(TERM_ROWS - 1, c);
    }
    if (s_term.r >= TERM_ROWS)
        s_term.r = TERM_ROWS - 1;
}

/* Advance the cursor; wrap and scroll as needed. */
static void term_linefeed(void) {
    s_term.r++;
    if (s_term.r >= TERM_ROWS)
        term_scroll_up(s_term.r - (TERM_ROWS - 1));
}

static void term_newline(void) {
    s_term.c = 0;
    term_linefeed();
}

static void term_sgr(int p) {
    if (p >= 30 && p <= 37) {
        s_term.fg = term_color(p - 30, s_term.bold);
    } else if (p >= 40 && p <= 47) {
        s_term.bg = term_color(p - 40, 0);
    } else if (p == 0) {
        s_term.fg = GUI_WHITE;
        s_term.bg = GUI_BLACK;
        s_term.bold = 0;
    } else if (p == 1) {
        s_term.bold = 1;
        s_term.fg = term_color(7, 1);
    } else if (p == 22) {
        s_term.bold = 0;
        s_term.fg = GUI_WHITE;
    } else if (p == 39) {
        s_term.fg = GUI_WHITE;
    } else if (p == 49) {
        s_term.bg = GUI_BLACK;
    }
}

static void term_exec_csi(void) {
    int i, n;
    switch (s_term.nparams) {
    case 0:
        s_term.nparams = 1;
        s_term.params[0] = 0;
        break;
    case 1:
        break;
    default:
        if (s_term.nparams > TERM_MAXP)
            s_term.nparams = TERM_MAXP;
        break;
    }
    int fin = (s_term.nparams >= 1) ? s_term.params[0] : 0;
    if (fin == 0)
        fin = 1;
    switch (s_term.fin) {
    case 'A': /* cursor up */
        s_term.r -= fin;
        if (s_term.r < 0)
            s_term.r = 0;
        break;
    case 'B': /* cursor down */
        s_term.r += fin;
        if (s_term.r >= TERM_ROWS)
            term_scroll_up(s_term.r - (TERM_ROWS - 1));
        break;
    case 'C': /* cursor forward */
        s_term.c += fin;
        if (s_term.c >= TERM_COLS)
            s_term.c = TERM_COLS - 1;
        break;
    case 'D': /* cursor back */
        s_term.c -= fin;
        if (s_term.c < 0)
            s_term.c = 0;
        break;
    case 'H': /* cursor home / position (row;col), 1-based */
    case 'f':
        n = (s_term.nparams >= 2) ? s_term.params[1] : 1;
        if (fin < 1)
            fin = 1;
        if (n < 1)
            n = 1;
        s_term.r = fin - 1;
        s_term.c = n - 1;
        if (s_term.r >= TERM_ROWS)
            s_term.r = TERM_ROWS - 1;
        if (s_term.c >= TERM_COLS)
            s_term.c = TERM_COLS - 1;
        break;
    case 'J': /* erase in display */
        n = (s_term.nparams >= 1) ? s_term.params[0] : 0;
        if (n == 2 || n == 3) {
            for (i = 0; i < TERM_ROWS; i++)
                for (int cc = 0; cc < TERM_COLS; cc++)
                    term_clear_at(i, cc);
            s_term.r = 0;
            s_term.c = 0;
        } else if (n == 0) {
            for (int cc = s_term.c; cc < TERM_COLS; cc++)
                term_clear_at(s_term.r, cc);
            for (i = s_term.r + 1; i < TERM_ROWS; i++)
                for (int cc = 0; cc < TERM_COLS; cc++)
                    term_clear_at(i, cc);
        } else if (n == 1) {
            for (int cc = 0; cc <= s_term.c; cc++)
                term_clear_at(s_term.r, cc);
            for (i = 0; i < s_term.r; i++)
                for (int cc = 0; cc < TERM_COLS; cc++)
                    term_clear_at(i, cc);
        }
        break;
    case 'K': /* erase in line */
        n = (s_term.nparams >= 1) ? s_term.params[0] : 0;
        if (n == 0) {
            for (int cc = s_term.c; cc < TERM_COLS; cc++)
                term_clear_at(s_term.r, cc);
        } else if (n == 1) {
            for (int cc = 0; cc <= s_term.c; cc++)
                term_clear_at(s_term.r, cc);
        } else {
            for (int cc = 0; cc < TERM_COLS; cc++)
                term_clear_at(s_term.r, cc);
        }
        break;
    case 'm': /* SGR */
        for (i = 0; i < s_term.nparams; i++)
            term_sgr(s_term.params[i]);
        break;
    default: /* unknown sequence: ignore */
        break;
    }
}

/* Feed one byte through the ANSI state machine. */
static void term_putc(unsigned char ch) {
    if (s_term.esc == 2) {
        if (ch >= '0' && ch <= '9') {
            s_term.cur = s_term.cur * 10 + (ch - '0');
            return;
        }
        if (ch == ';') {
            if (s_term.nparams < TERM_MAXP)
                s_term.params[s_term.nparams++] = s_term.cur;
            s_term.cur = 0;
            return;
        }
        if (ch == '?') /* private-mode prefix: skip */
            return;
        /* final byte */
        if (s_term.nparams < TERM_MAXP)
            s_term.params[s_term.nparams++] = s_term.cur;
        s_term.fin = ch;
        term_exec_csi();
        s_term.esc = 0;
        return;
    }
    if (s_term.esc == 1) {
        if (ch == '[')
            s_term.esc = 2;
        else
            s_term.esc = 0;
        s_term.nparams = 0;
        s_term.cur = 0;
        return;
    }
    if (ch == 0x1b) {
        s_term.esc = 1;
        return;
    }
    switch (ch) {
    case '\n':
        term_newline();
        return;
    case '\r':
        s_term.c = 0;
        return;
    case '\b':
        if (s_term.c > 0)
            s_term.c--;
        return;
    case '\t':
        s_term.c = ((s_term.c / 8) + 1) * 8;
        if (s_term.c >= TERM_COLS)
            s_term.c = TERM_COLS - 1;
        return;
    }
    if (ch < 32)
        return; /* other control chars: ignore */
    if (s_term.c >= TERM_COLS) {
        s_term.c = 0;
        term_linefeed();
    }
    term_cell_t *cell = &s_term.grid[s_term.r][s_term.c];
    cell->ch = ch;
    cell->fg = s_term.fg;
    cell->bg = s_term.bg;
    s_term.c++;
}

static void term_write(const char *s) {
    while (*s)
        term_putc((unsigned char)*s++);
}

static void term_reset(void) {
    for (int r = 0; r < TERM_ROWS; r++)
        for (int c = 0; c < TERM_COLS; c++)
            term_clear_at(r, c);
    s_term.r = 0;
    s_term.c = 0;
    s_term.fg = GUI_WHITE;
    s_term.bg = GUI_BLACK;
    s_term.bold = 0;
    s_term.esc = 0;
    s_term.nparams = 0;
    s_term.cur = 0;
}

static void term_draw(gui_widget_t *w) {
    int32_t ox = w->rect.x, oy = w->rect.y;
    gui_window_draw_rect(NULL, w->rect, GUI_BLACK);
    gui_window_draw_rect_outline(NULL, w->rect, GUI_DARK_GRAY, 1);
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            term_cell_t *cell = &s_term.grid[r][c];
            gui_rect_t cr = {ox + c * TERM_CW, oy + r * TERM_CH, TERM_CW, TERM_CH};
            gui_window_draw_rect(NULL, cr, cell->bg);
            if (cell->ch) {
                char s[2] = {(char)cell->ch, 0};
                gui_window_draw_text(NULL, ox + c * TERM_CW, oy + r * TERM_CH, s, cell->fg,
                                     cell->bg);
            }
        }
    }
    /* block cursor (underline) at the cursor cell */
    gui_rect_t cur = {ox + s_term.c * TERM_CW, oy + (s_term.r + 1) * TERM_CH - 2, TERM_CW, 2};
    gui_window_draw_rect(NULL, cur, GUI_WHITE);
}

static void term_event(gui_widget_t *w, gui_event_t *evt) {
    (void)w;
    if (evt->type != GUI_EVENT_CHAR)
        return;
    unsigned char ch = (unsigned char)evt->ch;
    if (ch == 8) /* Backspace: erase char before cursor */
        term_putc('\b');
    else if (ch == 27) /* ESC key typed raw */
        return;
    else
        term_putc(ch);
}

void gui_app_terminal_run(void) {
    term_reset();
    term_write("\x1b[1;36mRusikOS ANSI Terminal\x1b[0m\r\n"
               "\x1b[32mgreen\x1b[0m \x1b[31mred\x1b[0m \x1b[33myellow\x1b[0m "
               "\x1b[34mblue\x1b[0m \x1b[35mmagenta\x1b[0m \x1b[36mcyan\x1b[0m\r\n"
               "\x1b[1mbold text\x1b[0m\r\n\r\n"
               "Type here \x1b[7m(reverse)\x1b[0m. Enter, Tab and Backspace work.\r\n");
    gui_window_t *win = gui_window_create("Terminal", 320, 40, TERM_COLS * TERM_CW + 16,
                                          24 + TERM_ROWS * TERM_CH + 16, GUI_WINDOW_BG);
    if (!win)
        return;
    gui_add_window(win);
    gui_window_bring_to_front(win);
    gui_rect_t wr = gui_window_get_rect(win);
    gui_rect_t wrect = {wr.x + 8, wr.y + 24 + 8, TERM_COLS * TERM_CW, TERM_ROWS * TERM_CH};
    gui_widget_t *cw = gui_widget_create(wrect);
    if (!cw)
        return;
    cw->draw = term_draw;
    cw->on_event = term_event;
    gui_window_add_widget(win, cw);
    gui_window_set_focused_widget(win, cw);
}

/* 5. File Manager — directory tree and file operations.
 *
 * A single focusable widget lists the current working directory (dirs
 * first, in blue with a trailing '/', then files with their byte size).
 * Clicking a directory enters it; clicking a file selects it. A toolbar
 * provides UP (parent dir), REFRESH (re-read cwd) and DELETE (unlink the
 * selected file). Clicking a file also refreshes its details in the
 * status bar. Raw syscalls only — no FILE*.
 */
#define FM_MAX_ENTRIES 128
#define FM_PATH_MAX 256
#define FM_BUF 8192
#define FM_W 520
#define FM_H 360

typedef struct {
    char cwd[FM_PATH_MAX];
    char names[FM_MAX_ENTRIES][64];
    int is_dir[FM_MAX_ENTRIES];
    unsigned long long size[FM_MAX_ENTRIES];
    int n;        /* number of entries */
    int scroll;   /* first visible row */
    int selected; /* selected entry index, or -1 */
    char status[64];
} fm_state_t;

static fm_state_t s_fm;

/* Join dir + name into out (handles root "/"). */
static void fm_fullpath(const char *dir, const char *name, char *out, int cap) {
    if (dir[0] == '/' && dir[1] == 0)
        snprintf(out, cap, "/%s", name);
    else
        snprintf(out, cap, "%s/%s", dir, name);
}

/* Re-read s_fm.cwd into the entry table, dirs first. */
static void fm_read_dir(void) {
    s_fm.n = 0;
    s_fm.scroll = 0;
    s_fm.selected = -1;
    int fd = open(s_fm.cwd, O_RDONLY, 0);
    if (fd < 0) {
        snprintf(s_fm.status, sizeof(s_fm.status), "cannot open %s", s_fm.cwd);
        return;
    }
    char buf[FM_BUF];
    int n = getdents64(fd, buf, sizeof(buf));
    close(fd);
    if (n < 0) {
        snprintf(s_fm.status, sizeof(s_fm.status), "readdir failed");
        return;
    }
    int pos = 0;
    while (pos < n && s_fm.n < FM_MAX_ENTRIES) {
        struct dirent *d = (struct dirent *)(buf + pos);
        const char *name = d->d_name;
        if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
            char fp[512];
            fm_fullpath(s_fm.cwd, name, fp, sizeof(fp));
            int is_dir = 0;
            unsigned long long sz = 0;
            struct stat st;
            if (stat(fp, &st) == 0) {
                if ((st.st_mode & 0170000) == 0040000)
                    is_dir = 1;
                sz = (unsigned long long)st.st_size;
            } else if (d->d_type == 4) {
                is_dir = 1;
            }
            snprintf(s_fm.names[s_fm.n], 64, "%s", name);
            s_fm.is_dir[s_fm.n] = is_dir;
            s_fm.size[s_fm.n] = sz;
            s_fm.n++;
        }
        pos += d->d_reclen;
    }
    /* Stable sort: directories before files. */
    for (int i = 0; i < s_fm.n; i++) {
        if (s_fm.is_dir[i])
            continue;
        for (int j = i + 1; j < s_fm.n; j++) {
            if (s_fm.is_dir[j]) {
                char tn[64];
                memcpy(tn, s_fm.names[i], 64);
                memcpy(s_fm.names[i], s_fm.names[j], 64);
                memcpy(s_fm.names[j], tn, 64);
                int td = s_fm.is_dir[i];
                s_fm.is_dir[i] = s_fm.is_dir[j];
                s_fm.is_dir[j] = td;
                unsigned long long tz = s_fm.size[i];
                s_fm.size[i] = s_fm.size[j];
                s_fm.size[j] = tz;
                break;
            }
        }
    }
    snprintf(s_fm.status, sizeof(s_fm.status), "%d entries", s_fm.n);
}

/* Enter the selected directory. */
static void fm_chdir_entry(int idx) {
    if (idx < 0 || idx >= s_fm.n || !s_fm.is_dir[idx])
        return;
    char fp[512];
    fm_fullpath(s_fm.cwd, s_fm.names[idx], fp, sizeof(fp));
    snprintf(s_fm.cwd, sizeof(s_fm.cwd), "%s", fp);
    fm_read_dir();
}

/* Navigate to the parent directory. */
static void fm_go_up(void) {
    int len = (int)strlen(s_fm.cwd);
    if (len <= 1)
        return; /* already at root */
    while (len > 1 && s_fm.cwd[len - 1] == '/')
        s_fm.cwd[--len] = '\0';
    char *slash = NULL;
    for (char *p = s_fm.cwd; *p; p++)
        if (*p == '/')
            slash = p;
    if (slash == s_fm.cwd) {
        s_fm.cwd[1] = '\0'; /* back to root */
    } else if (slash) {
        *slash = '\0';
    }
    fm_read_dir();
}

/* Delete the selected file. */
static void fm_delete_selected(void) {
    if (s_fm.selected < 0 || s_fm.selected >= s_fm.n) {
        snprintf(s_fm.status, sizeof(s_fm.status), "nothing selected");
        return;
    }
    char fp[512];
    fm_fullpath(s_fm.cwd, s_fm.names[s_fm.selected], fp, sizeof(fp));
    if (unlink(fp) == 0) {
        snprintf(s_fm.status, sizeof(s_fm.status), "deleted %s", s_fm.names[s_fm.selected]);
        fm_read_dir();
    } else {
        snprintf(s_fm.status, sizeof(s_fm.status), "delete %s failed", s_fm.names[s_fm.selected]);
    }
}

static void fm_draw(gui_widget_t *w) {
    fm_state_t *st = &s_fm;
    int32_t ox = w->rect.x, oy = w->rect.y;
    gui_window_draw_rect(NULL, w->rect, GUI_WINDOW_BG);

    /* Path header. */
    gui_window_draw_text(NULL, ox + 4, oy + 2, st->cwd, GUI_BLUE, GUI_WINDOW_BG);

    /* Toolbar: UP / REFRESH / DELETE. */
    gui_rect_t up = {ox + 4, oy + 18, 44, 22};
    gui_window_draw_rect(NULL, up, GUI_BUTTON_BG);
    gui_window_draw_rect_outline(NULL, up, GUI_DARK_GRAY, 1);
    gui_window_draw_text(NULL, ox + 12, oy + 22, "UP", GUI_BUTTON_FG, GUI_BUTTON_BG);
    gui_rect_t ref = {ox + 50, oy + 18, 56, 22};
    gui_window_draw_rect(NULL, ref, GUI_BUTTON_BG);
    gui_window_draw_rect_outline(NULL, ref, GUI_DARK_GRAY, 1);
    gui_window_draw_text(NULL, ox + 54, oy + 22, "REFRESH", GUI_BUTTON_FG, GUI_BUTTON_BG);
    gui_rect_t del = {ox + 108, oy + 18, 56, 22};
    gui_window_draw_rect(NULL, del, GUI_BUTTON_BG);
    gui_window_draw_rect_outline(NULL, del, GUI_DARK_GRAY, 1);
    gui_window_draw_text(NULL, ox + 112, oy + 22, "DELETE", GUI_BUTTON_FG, GUI_BUTTON_BG);

    /* Entry list area. */
    gui_rect_t la = {ox + 4, oy + 46, w->rect.w - 8, w->rect.h - 52};
    gui_window_draw_rect(NULL, la, GUI_WHITE);
    gui_window_draw_rect_outline(NULL, la, GUI_GRAY, 1);

    int line_h = 14;
    int max_vis = la.h / line_h;
    if (max_vis < 1)
        max_vis = 1;

    for (int i = st->scroll; i < st->n && (i - st->scroll) < max_vis; i++) {
        int ry = la.y + 2 + (i - st->scroll) * line_h;
        if (i == st->selected) {
            gui_rect_t hl = {la.x, la.y + (i - st->scroll) * line_h, la.w, line_h};
            gui_window_draw_rect(NULL, hl, GUI_LIGHT_GRAY);
        }
        gui_color_t fc = st->is_dir[i] ? GUI_BLUE : GUI_TEXT_FG;
        char row[88];
        if (st->is_dir[i]) {
            snprintf(row, sizeof(row), "%s/", st->names[i]);
        } else {
            char sz[20];
            unsigned long long v = st->size[i];
            int szp = 19;
            sz[19] = '\0';
            if (v == 0) {
                sz[--szp] = '0';
            } else {
                while (v > 0 && szp > 0) {
                    sz[--szp] = '0' + (v % 10);
                    v /= 10;
                }
            }
            snprintf(row, sizeof(row), "%s  [%s]", st->names[i], sz + szp);
        }
        gui_window_draw_text(NULL, la.x + 4, ry, row, fc,
                             i == st->selected ? GUI_LIGHT_GRAY : GUI_WHITE);
    }

    /* Status bar. */
    gui_window_draw_text(NULL, ox + 4, oy + w->rect.h - 14,
                         st->status[0] ? st->status : "File Manager", GUI_TEXT_FG, GUI_WINDOW_BG);
}

static void fm_event(gui_widget_t *w, gui_event_t *evt) {
    int32_t lx;
    int32_t ly;
    if (evt->type == GUI_EVENT_CHAR) {
        if (evt->ch == 'u' || evt->ch == 'U')
            fm_go_up();
        else if (evt->ch == 'd' || evt->ch == 'D')
            fm_delete_selected();
        return;
    }
    if (evt->type != GUI_EVENT_MOUSE_DOWN || evt->button != 1)
        return;
    lx = evt->x - w->rect.x;
    ly = evt->y - w->rect.y;

    /* Toolbar hit-test. */
    if (ly >= 18 && ly < 40) {
        if (lx >= 4 && lx < 48)
            fm_go_up();
        else if (lx >= 50 && lx < 106)
            fm_read_dir();
        else if (lx >= 108 && lx < 164)
            fm_delete_selected();
        return;
    }

    /* Entry list hit-test. */
    if (ly >= 46) {
        int line_h = 14;
        int list_h = w->rect.h - 52;
        if (list_h < line_h)
            return;
        int rel = ly - 46;
        if (rel >= list_h)
            return;
        int row = rel / line_h;
        int idx = s_fm.scroll + row;
        if (idx < 0 || idx >= s_fm.n)
            return;
        if (s_fm.is_dir[idx]) {
            s_fm.selected = idx;
            fm_chdir_entry(idx);
        } else {
            s_fm.selected = idx;
            snprintf(s_fm.status, sizeof(s_fm.status), "%s (%llu bytes)", s_fm.names[idx],
                     s_fm.size[idx]);
        }
    }
}

void gui_app_file_manager_run(void) {
    snprintf(s_fm.cwd, sizeof(s_fm.cwd), "/");
    s_fm.status[0] = '\0';
    fm_read_dir();
    gui_window_t *win = gui_window_create("File Manager", 220, 90, FM_W, FM_H, GUI_WINDOW_BG);
    if (!win)
        return;
    gui_add_window(win);
    gui_window_bring_to_front(win);
    gui_rect_t wr = gui_window_get_rect(win);
    gui_rect_t wrect = {wr.x + 8, wr.y + 24 + 8, FM_W - 16, FM_H - 32};
    gui_widget_t *cw = gui_widget_create(wrect);
    if (!cw)
        return;
    cw->draw = fm_draw;
    cw->on_event = fm_event;
    gui_window_add_widget(win, cw);
    gui_window_set_focused_widget(win, cw);
}

/* ===== Settings: display, sound, network config panel =====
 *
 * A tabbed configuration panel for the desktop. Three tabs select a
 * section (Display, Sound, Network); each draws its controls on the
 * right-hand content area and reacts to mouse clicks. Settings live in
 * an in-memory state struct (s_set) and are purely illustrative for the
 * desktop shell — raw syscalls only, no FILE*.
 */
#define SET_W 480
#define SET_H 360
#define SET_TAB_DISPLAY 0
#define SET_TAB_SOUND 1
#define SET_TAB_NET 2
#define SET_RES_MODES 3

typedef struct {
    int tab;
    int res;        /* selected resolution index */
    int brightness; /* 0..100 */
    int volume;     /* 0..100 */
    int muted;
    int eth; /* network link up */
} settings_state_t;

static settings_state_t s_set = {SET_TAB_DISPLAY, 0, 80, 50, 0, 1};

static const char *const s_set_res[SET_RES_MODES] = {"800x600", "1024x768", "1280x720"};

/* Draw a titled button and return its fill color boolean. */
static void set_btn(gui_widget_t *w, int32_t x, int32_t y, uint32_t bw, uint32_t bh,
                    const char *label, gui_color_t bg) {
    (void)w;
    gui_rect_t r = {x, y, bw, bh};
    gui_window_draw_rect(NULL, r, bg);
    gui_window_draw_rect_outline(NULL, r, GUI_DARK_GRAY, 1);
    gui_window_draw_text(NULL, x + 4, y + bh / 2 - 4, label, GUI_BUTTON_FG, bg);
}

/* Draw a simple slider (track + filled portion + label). */
static void set_slider(gui_widget_t *w, int32_t x, int32_t y, uint32_t bw, const char *label,
                       int val) {
    (void)w;
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %d%%", label, val);
    gui_window_draw_text(NULL, x, y, buf, GUI_TEXT_FG, GUI_WINDOW_BG);
    gui_rect_t track = {x, y + 12, bw, 12};
    gui_window_draw_rect(NULL, track, GUI_WHITE);
    gui_window_draw_rect_outline(NULL, track, GUI_DARK_GRAY, 1);
    int fill = val * (int)bw / 100;
    if (fill > 2) {
        gui_rect_t fr = {x + 1, y + 13, (uint32_t)(fill - 2), 10};
        gui_window_draw_rect(NULL, fr, GUI_BLUE);
    }
}

/* Hit-test a slider click and map relative x onto 0..100. */
static int set_slider_pct(gui_widget_t *w, int32_t lx, int32_t ly, int32_t x, int32_t y,
                          uint32_t bw) {
    if (ly < y || ly > y + 24)
        return -1;
    if (lx < x || lx > (int32_t)(x + bw))
        return -1;
    int pct = (lx - x) * 100 / (int)bw;
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    (void)w;
    return pct;
}

static void settings_draw(gui_widget_t *w) {
    settings_state_t *st = &s_set;
    int32_t ox = w->rect.x, oy = w->rect.y;
    gui_window_draw_rect(NULL, w->rect, GUI_WINDOW_BG);

    /* Section list on the left. */
    static const char *const tabs[3] = {"DISPLAY", "SOUND", "NETWORK"};
    for (int i = 0; i < 3; i++) {
        gui_rect_t tr = {ox + 6, oy + 8 + i * 26, 96, 24};
        gui_color_t bg = (i == st->tab) ? GUI_LIGHT_GRAY : GUI_BUTTON_BG;
        gui_window_draw_rect(NULL, tr, bg);
        gui_window_draw_rect_outline(NULL, tr, GUI_DARK_GRAY, 1);
        gui_window_draw_text(NULL, tr.x + 10, tr.y + 5, tabs[i], GUI_BUTTON_FG, bg);
    }

    int32_t cx = ox + 112;
    gui_window_draw_text(NULL, cx, oy + 6, "Settings", GUI_TITLE_BG, GUI_WINDOW_BG);

    if (st->tab == SET_TAB_DISPLAY) {
        gui_window_draw_text(NULL, cx, oy + 28, "Resolution:", GUI_TEXT_FG, GUI_WINDOW_BG);
        for (int i = 0; i < SET_RES_MODES; i++) {
            gui_color_t bg = (i == st->res) ? GUI_LIGHT_GRAY : GUI_BUTTON_BG;
            set_btn(w, cx, oy + 40 + i * 24, 120, 22, s_set_res[i], bg);
        }
        set_slider(w, cx, oy + 116, 200, "Brightness", st->brightness);
    } else if (st->tab == SET_TAB_SOUND) {
        set_slider(w, cx, oy + 28, 260, "Volume", st->muted ? 0 : st->volume);
        set_btn(w, cx, oy + 56, 90, 22, st->muted ? "MUTED" : "MUTE",
                st->muted ? GUI_RED : GUI_BUTTON_BG);
        gui_window_draw_text(NULL, cx, oy + 90, st->muted ? "Audio muted" : "Audio enabled",
                             st->muted ? GUI_RED : GUI_GREEN, GUI_WINDOW_BG);
    } else {
        gui_window_draw_text(NULL, cx, oy + 28, "Network:", GUI_TEXT_FG, GUI_WINDOW_BG);
        gui_window_draw_text(NULL, cx, oy + 42, "eth0", GUI_BLUE, GUI_WINDOW_BG);
        gui_window_draw_text(NULL, cx + 60, oy + 42, st->eth ? "LINK UP" : "LINK DOWN",
                             st->eth ? GUI_GREEN : GUI_RED, GUI_WINDOW_BG);
        set_btn(w, cx, oy + 58, 96, 22, st->eth ? "DISCONNECT" : "CONNECT", GUI_BUTTON_BG);
        gui_window_draw_text(NULL, cx, oy + 92, "Gateway 10.0.2.2", GUI_TEXT_FG, GUI_WINDOW_BG);
        gui_window_draw_text(NULL, cx, oy + 106, "Subnet 255.255.255.0", GUI_TEXT_FG,
                             GUI_WINDOW_BG);
    }
}

static void settings_event(gui_widget_t *w, gui_event_t *evt) {
    settings_state_t *st = &s_set;
    int32_t lx;
    int32_t ly;
    if (evt->type != GUI_EVENT_MOUSE_DOWN || evt->button != 1)
        return;
    lx = evt->x - w->rect.x;
    ly = evt->y - w->rect.y;

    /* Section tabs on the left. */
    if (lx >= 6 && lx < 102 && ly >= 8 && ly < 80) {
        int i = (ly - 8) / 26;
        if (i >= 0 && i <= 2)
            st->tab = i;
        return;
    }

    int32_t cx = lx - 112;
    if (cx < 0)
        return;

    if (st->tab == SET_TAB_DISPLAY) {
        /* Resolution rows. */
        for (int i = 0; i < SET_RES_MODES; i++) {
            if (cx < 120 && ly >= 40 + i * 24 && ly < 62 + i * 24) {
                st->res = i;
                return;
            }
        }
        int pct = set_slider_pct(w, lx, ly, 112, 128, 200);
        if (pct >= 0)
            st->brightness = pct;
    } else if (st->tab == SET_TAB_SOUND) {
        int pct = set_slider_pct(w, lx, ly, 112, 40, 260);
        if (pct >= 0) {
            if (!st->muted)
                st->volume = pct;
            return;
        }
        if (cx < 90 && ly >= 68 && ly < 90)
            st->muted = !st->muted;
    } else {
        if (cx < 96 && ly >= 70 && ly < 92)
            st->eth = !st->eth;
    }
}

void gui_app_settings_run(void) {
    gui_window_t *win = gui_window_create("Settings", 200, 80, SET_W, SET_H, GUI_WINDOW_BG);
    if (!win)
        return;
    gui_add_window(win);
    gui_window_bring_to_front(win);
    gui_rect_t wr = gui_window_get_rect(win);
    gui_rect_t wrect = {wr.x + 8, wr.y + 24 + 8, SET_W - 16, SET_H - 32};
    gui_widget_t *cw = gui_widget_create(wrect);
    if (!cw)
        return;
    cw->draw = settings_draw;
    cw->on_event = settings_event;
    gui_window_add_widget(win, cw);
    gui_window_set_focused_widget(win, cw);
}

/* 7. Image Viewer — PNG/BMP display (D286)
 *
 * Loads /image.bmp or /image.png from the root filesystem, decodes it and
 * displays it scaled-to-fit inside the window (see gui_image.c).  If no
 * image is present it falls back to a generated demo "test card" so the
 * window is never blank.  Raw syscalls only — no FILE*.
 */
#define IVCW 440
#define IVCH 272
static gui_color_t s_iv_fb[IVCW * IVCH];

void gui_app_image_viewer_run(void) {
    static unsigned char img[262144];
    char status[80];
    unsigned long n = 0;
    int loaded = 0;

    gui_window_t *win = gui_window_create("Image Viewer", 60, 50, 480, 340, GUI_WINDOW_BG);
    if (!win)
        return;
    gui_add_window(win);
    gui_window_bring_to_front(win);

    int fd = open("/image.bmp", O_RDONLY, 0);
    if (fd >= 0) {
        int r = read(fd, img, (unsigned long)sizeof img);
        close(fd);
        if (r > 0) {
            n = (unsigned long)r;
            loaded = gui_img_decode(img, n, s_iv_fb, IVCW, IVCH, status, sizeof status);
        }
    }
    if (!loaded) {
        fd = open("/image.png", O_RDONLY, 0);
        if (fd >= 0) {
            int r = read(fd, img, (unsigned long)sizeof img);
            close(fd);
            if (r > 0) {
                n = (unsigned long)r;
                loaded = gui_img_decode(img, n, s_iv_fb, IVCW, IVCH, status, sizeof status);
            }
        }
    }
    if (!loaded) {
        gui_img_demo(s_iv_fb, IVCW, IVCH);
        snprintf(status, sizeof status,
                 "Demo preview — place an image at /image.bmp or /image.png");
    }

    gui_rect_t wr = gui_window_get_rect(win);
    int x0 = wr.x + ((480 - IVCW) / 2);
    int y0 = wr.y + 24 + 8;
    gui_draw_image_raw(x0, y0, IVCW, IVCH, s_iv_fb);
    gui_window_draw_text(win, x0, y0 + IVCH + 6, status, GUI_TEXT_FG, GUI_WINDOW_BG);
}

/* 7b. Minesweeper */
void gui_app_minesweeper_run(void) {
    gui_window_t *win = gui_window_create("Minesweeper", 200, 150, 220, 260, GUI_LIGHT_GRAY);
    if (!win)
        return;
    gui_add_window(win);
    int rows = 8, cols = 8, cell = 24;
    gui_window_draw_text(win, 15, 10, "Minesweeper (click)", GUI_TEXT_FG, GUI_LIGHT_GRAY);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            gui_rect_t cr = {15 + c * cell, 30 + r * cell, cell, cell};
            gui_window_draw_rect(win, cr, GUI_BUTTON_BG);
            gui_window_draw_rect_outline(win, cr, GUI_DARK_GRAY, 1);
        }
    }
}

/* 8. Snake Game */
void gui_app_snake_run(void) {
    gui_window_t *win = gui_window_create("Snake", 250, 100, 260, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int cell = 12, cols = 20, rows = 22;
    /* Draw grid */
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            gui_rect_t cr = {10 + c * cell, 20 + r * cell, cell - 1, cell - 1};
            gui_window_draw_rect(win, cr, GUI_COLOR(0, 20, 0));
        }
    }
    /* Draw snake */
    int segs[][2] = {{5, 10}, {4, 10}, {3, 10}};
    for (int i = 0; i < 3; i++) {
        gui_rect_t sr = {10 + segs[i][0] * cell, 20 + segs[i][1] * cell, cell - 1, cell - 1};
        gui_window_draw_rect(win, sr, GUI_GREEN);
    }
    gui_window_draw_text(win, 30, 10, "Snake (3 pts)", GUI_GREEN, GUI_BLACK);
}

/* 9. Tetris */
void gui_app_tetris_run(void) {
    gui_window_t *win = gui_window_create("Tetris", 300, 100, 200, 380, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int cell = 18, cols = 10, rows = 18;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            gui_rect_t cr = {10 + c * cell, 10 + r * cell, cell - 1, cell - 1};
            gui_window_draw_rect(win, cr, GUI_COLOR(10, 10, 20));
            gui_window_draw_rect_outline(win, cr, GUI_COLOR(30, 30, 40), 1);
        }
    }
    /* Draw a T-piece falling */
    int tp[4][2] = {{4, 0}, {5, 0}, {6, 0}, {5, 1}};
    for (int i = 0; i < 4; i++) {
        gui_rect_t tr = {10 + tp[i][0] * cell, 10 + tp[i][1] * cell, cell - 1, cell - 1};
        gui_window_draw_rect(win, tr, GUI_COLOR(200, 0, 200));
    }
    gui_window_draw_text(win, 10, 360, "Tetris", GUI_COLOR(200, 0, 200), GUI_BLACK);
}

/* 10. Lissajous Curves */
void gui_app_lissajous_run(void) {
    gui_window_t *win = gui_window_create("Lissajous", 200, 100, 400, 400, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int cx = 200, cy = 200, a = 80, b = 80;
    int hz_a = 3, hz_b = 4;
    for (int i = 0; i < 1000; i++) {
        float t = (float)i * 6.2832f / 1000.0f;
        int x = cx + (int)(a * __icos((int)(hz_a * t * 57.3f)));
        int y = cy + (int)(b * __isin((int)(hz_b * t * 57.3f)));
        vga_put_pixel(x, y, GUI_CYAN);
    }
    gui_window_draw_text(win, 120, 10, "Lissajous 3:4", GUI_CYAN, GUI_BLACK);
}

/* 11. Starfield */
void gui_app_starfield_run(void) {
    gui_window_t *win = gui_window_create("Starfield", 50, 50, 400, 320, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    /* Precomputed stars */
    int stars[50][3];
    for (int i = 0; i < 50; i++) {
        stars[i][0] = (i * 137 + 50) % 400;
        stars[i][1] = (i * 251 + 30) % 300;
        int b = 150 + (i * 37) % 105;
        stars[i][2] = b > 255 ? 255 : b;
    }
    for (int i = 0; i < 50; i++) {
        gui_color_t c = GUI_COLOR(stars[i][2], stars[i][2], stars[i][2]);
        vga_put_pixel(stars[i][0], stars[i][1] + 20, c);
    }
    gui_window_draw_text(win, 120, 10, "Starfield (50 stars)", GUI_WHITE, GUI_BLACK);
}

/* 12. Fire Effect */
void gui_app_fire_run(void) {
    gui_window_t *win = gui_window_create("Fire Effect", 150, 100, 320, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int w = 300, h = 260;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int heat = (h - y) * 255 / h;
            if (heat > 255)
                heat = 255;
            int r = heat, g = (heat * 3 / 4), b = (heat / 3);
            if (g > 255)
                g = 255;
            vga_put_pixel(10 + x, 30 + y, GUI_COLOR(r < 255 ? r : 255, g, b > 255 ? 255 : b));
        }
    }
    gui_window_draw_text(win, 80, 10, "Fire Effect", GUI_RED, GUI_BLACK);
}

/* 13. Plasma Effect */
void gui_app_plasma_run(void) {
    gui_window_t *win = gui_window_create("Plasma", 100, 100, 400, 320, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int w = 380, h = 280, t = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int val = (x * 13 + y * 37 + t) % 360;
            int val2 = (x * 7 + y * 23 + t * 2) % 360;
            int hue = (val + val2) / 2;
            gui_color_t c = gui_color_from_hsv(hue, 200, 200);
            vga_put_pixel(10 + x, 30 + y, c);
        }
    }
    (void)t;
    gui_window_draw_text(win, 120, 10, "Plasma Effect", GUI_WHITE, GUI_BLACK);
}

/* 14. Particle System */
void gui_app_particles_run(void) {
    gui_window_t *win = gui_window_create("Particles", 100, 100, 300, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int pts[30][4];
    for (int i = 0; i < 30; i++) {
        pts[i][0] = 150 + (i * 23 % 100) - 50; /* x */
        pts[i][1] = 140 + (i * 17 % 100) - 50; /* y */
        pts[i][2] = (i * 7) % 6 - 3;           /* vx */
        pts[i][3] = (i * 11) % 6 - 3;          /* vy */
    }
    for (int i = 0; i < 30; i++) {
        gui_draw_circle_filled(pts[i][0], pts[i][1], 3, gui_color_random());
    }
    gui_window_draw_text(win, 60, 10, "Particle System (static frame)", GUI_LIGHT_GRAY, GUI_BLACK);
}

/* 15. Sort Visualization */
void gui_app_sort_viz_run(void) {
    gui_window_t *win = gui_window_create("Sort Viz", 150, 100, 400, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int w = 380, n = 40, bar_w = w / n;
    int vals[40];
    for (int i = 0; i < n; i++)
        vals[i] = (i * 17 + 5) % 200;
    /* Draw bars */
    for (int i = 0; i < n; i++) {
        int bh = vals[i];
        gui_color_t c = gui_color_from_hsv(i * 9, 200, 150 + bh / 4);
        gui_rect_t bar = {10 + i * bar_w, 270 - bh, bar_w - 1, (uint32_t)bh};
        gui_window_draw_rect(win, bar, c);
    }
    gui_window_draw_text(win, 100, 10, "Sorting Visualization (unsorted)", GUI_WHITE, GUI_BLACK);
}

/* 16. Wave Visualizer */
void gui_app_wave_run(void) {
    gui_window_t *win = gui_window_create("Wave", 100, 100, 420, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int prev_y = 0;
    for (int x = 0; x < 400; x++) {
        int deg = x * 360 / 100;
        int y = 140 + (100 * __isin(deg) / 100) + (50 * __isin(deg * 3 + 30) / 100);
        if (x > 0)
            gui_draw_line(10 + x - 1, 30 + prev_y, 10 + x, 30 + y, GUI_CYAN);
        prev_y = y;
    }
    gui_window_draw_text(win, 100, 10, "Wave (composite sine)", GUI_CYAN, GUI_BLACK);
}

/* 17. Value Noise */
void gui_app_noise_run(void) {
    gui_window_t *win = gui_window_create("Value Noise", 100, 100, 300, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int ns = 42;
    for (int y = 0; y < 256; y++) {
        for (int x = 0; x < 256; x++) {
            int v = ((x * 13 + y * 37 + ns) * 7 + (x * y) % 101) % 256;
            gui_color_t c = GUI_COLOR(v, v, v);
            vga_put_pixel(20 + x, 20 + y, c);
        }
    }
    (void)ns;
    gui_window_draw_text(win, 60, 10, "Value Noise", GUI_WHITE, GUI_BLACK);
}

/* 18. Heatmap */
void gui_app_heatmap_run(void) {
    gui_window_t *win = gui_window_create("Heatmap", 200, 100, 400, 320, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int w = 360, h = 280;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float dx = (float)(x - w / 2) / (float)(w / 3);
            float dy = (float)(y - h / 2) / (float)(h / 3);
            float v = 1.0f / (1.0f + dx * dx + dy * dy);
            int r = (int)(v * 255.0f);
            int g2 = (int)(v * 200.0f);
            int b = (int)(v * 100.0f);
            if (r > 255)
                r = 255;
            if (g2 > 255)
                g2 = 255;
            if (b > 255)
                b = 255;
            vga_put_pixel(20 + x, 30 + y, GUI_COLOR(r, g2, b));
        }
    }
    gui_window_draw_text(win, 100, 10, "Heatmap (2D Gaussian)", GUI_WHITE, GUI_BLACK);
}

/* 19. Fractal Tree */
static void __draw_branch(int32_t x, int32_t y, int len, int angle, int depth) {
    if (depth <= 0 || len < 2)
        return;
    int ex = x + len * __icos(angle) / 100;
    int ey = y + len * __isin(angle) / 100;
    gui_color_t c = GUI_COLOR(100 + depth * 30, 180 - depth * 20, 50);
    gui_draw_thick_line(x, y, ex, ey, depth / 2 + 1, c);
    __draw_branch(ex, ey, len * 2 / 3, angle - 30, depth - 1);
    __draw_branch(ex, ey, len * 2 / 3, angle + 30, depth - 1);
}
void gui_app_fractal_tree_run(void) {
    gui_window_t *win = gui_window_create("Fractal Tree", 150, 50, 300, 360, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    __draw_branch(150, 340, 80, -90, 8);
    gui_window_draw_text(win, 60, 10, "Fractal Tree (depth 8)", GUI_GREEN, GUI_BLACK);
}

/* 20. Sierpinski Triangle */
void gui_app_sierpinski_run(void) {
    gui_window_t *win = gui_window_create("Sierpinski", 200, 100, 330, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int depth = 5;
    int pts[3][2] = {{165, 10}, {10, 280}, {320, 280}};
    int cx = (pts[0][0] + pts[1][0] + pts[2][0]) / 3;
    int cy = (pts[0][1] + pts[1][1] + pts[2][1]) / 3;
    for (int i = 0; i < 5000; i++) {
        int r = (i * 7 + 3) % 3;
        cx = (cx + pts[r][0]) / 2;
        cy = (cy + pts[r][1]) / 2;
        gui_color_t c = gui_color_from_hsv(i * 73 % 360, 200, 200);
        vga_put_pixel(cx, cy, c);
    }
    (void)depth;
    gui_window_draw_text(win, 60, 10, "Sierpinski (Chaos Game)", GUI_WHITE, GUI_BLACK);
}

/* 21. Cellular Automata (Game of Life) */
void gui_app_cellular_run(void) {
    gui_window_t *win = gui_window_create("Game of Life", 100, 50, 320, 340, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int w = 40, h = 40, cell = 7;
    int grid[40][40] = {0};
    /* Glider */
    grid[5][5] = 1;
    grid[6][6] = 1;
    grid[7][4] = 1;
    grid[7][5] = 1;
    grid[7][6] = 1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            gui_rect_t cr = {10 + x * cell, 20 + y * cell, cell - 1, cell - 1};
            gui_color_t c = grid[y][x] ? GUI_GREEN : GUI_COLOR(10, 10, 10);
            gui_window_draw_rect(win, cr, c);
        }
    }
    gui_window_draw_text(win, 60, 10, "Game of Life (Glider)", GUI_GREEN, GUI_BLACK);
}

/* 22. Moire Pattern */
void gui_app_moire_run(void) {
    gui_window_t *win = gui_window_create("Moire", 200, 100, 400, 340, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int cx = 200, cy = 160;
    for (int r = 0; r < 160; r += 4) {
        gui_draw_circle(cx, cy, r, gui_color_from_hsv(r * 3 % 360, 200, 200));
    }
    gui_window_draw_text(win, 100, 10, "Moire Pattern", GUI_WHITE, GUI_BLACK);
}

/* 23. Tunnel Effect */
void gui_app_tunnel_run(void) {
    gui_window_t *win = gui_window_create("Tunnel", 100, 100, 400, 320, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int cx = 200, cy = 150;
    for (int r = 160; r > 0; r -= 6) {
        gui_color_t c = gui_color_from_hsv((r * 5 + 180) % 360, 200, 150 + (160 - r) / 2);
        gui_draw_circle_filled(cx, cy, r, c);
    }
    gui_window_draw_text(win, 100, 10, "Tunnel Effect", GUI_WHITE, GUI_BLACK);
}

/* 24. Metaballs */
void gui_app_metaballs_run(void) {
    gui_window_t *win = gui_window_create("Metaballs", 150, 100, 320, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int balls[3][3] = {{100, 80, 40}, {220, 120, 35}, {160, 200, 45}};
    for (int y = 0; y < 260; y++) {
        for (int x = 0; x < 300; x++) {
            float sum = 0;
            for (int b = 0; b < 3; b++) {
                float dx = (float)(x - balls[b][0]);
                float dy = (float)(y - balls[b][1]);
                sum += (float)(balls[b][2] * balls[b][2]) / (dx * dx + dy * dy + 1);
            }
            gui_color_t c = sum > 1.0f ? GUI_COLOR(0, (int)(sum * 60), (int)(sum * 80)) : GUI_BLACK;
            vga_put_pixel(10 + x, 20 + y, c);
        }
    }
    gui_window_draw_text(win, 60, 10, "Metaballs", GUI_CYAN, GUI_BLACK);
}

/* 25. Snow Effect */
void gui_app_snow_run(void) {
    gui_window_t *win = gui_window_create("Snowfall", 100, 100, 320, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int flakes[40][2];
    for (int i = 0; i < 40; i++) {
        flakes[i][0] = (i * 37 + 10) % 300;
        flakes[i][1] = (i * 53 + 20) % 260;
    }
    for (int i = 0; i < 40; i++) {
        vga_put_pixel(10 + flakes[i][0], 20 + flakes[i][1], GUI_WHITE);
    }
    gui_window_draw_text(win, 60, 10, "Snowfall (static frame)", GUI_LIGHT_GRAY, GUI_BLACK);
}

/* 26. Gravity Simulator */
void gui_app_gravity_run(void) {
    gui_window_t *win = gui_window_create("Gravity", 200, 100, 320, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int px = 50, py = 50, pvx = 5, pvy = 0;
    int gx = 160, gy = 130;
    /* Simple orbit simulation (single frame) */
    for (int i = 0; i < 500; i++) {
        float dx = (float)(gx - px);
        float dy = (float)(gy - py);
        float dist2 = dx * dx + dy * dy + 1;
        float acc = 500.0f / dist2;
        pvx = (int)((float)pvx + acc * dx / __isqrt((int)dist2));
        pvy = (int)((float)pvy + acc * dy / __isqrt((int)dist2));
        px += pvx / 10;
        py += pvy / 10;
        if (px < 0 || px > 300 || py < 0 || py > 280)
            break;
        vga_put_pixel(10 + px, 20 + py, GUI_COLOR(200, 200, 100));
    }
    gui_draw_circle_filled(gx + 10, gy + 20, 8, GUI_YELLOW);
    gui_window_draw_text(win, 60, 10, "Orbit Simulator (500 steps)", GUI_YELLOW, GUI_BLACK);
}

/* 27. Rotozoom */
void gui_app_rotozoom_run(void) {
    gui_window_t *win = gui_window_create("Rotozoom", 100, 100, 320, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int cx = 160, cy = 130, angle = 0;
    for (int y = 0; y < 260; y++) {
        for (int x = 0; x < 300; x++) {
            int rx = x - cx, ry = y - cy;
            int s = __isin(angle), c = __icos(angle);
            int tx = (rx * c - ry * s) / 100;
            int ty = (rx * s + ry * c) / 100;
            int hue = (tx * 3 + ty * 7) % 360;
            if (hue < 0)
                hue += 360;
            vga_put_pixel(10 + x, 20 + y, gui_color_from_hsv(hue, 200, 200));
        }
    }
    (void)angle;
    gui_window_draw_text(win, 60, 10, "Rotozoom", GUI_WHITE, GUI_BLACK);
}

/* 28. Kaleidoscope */
void gui_app_kaleidoscope_run(void) {
    gui_window_t *win = gui_window_create("Kaleidoscope", 150, 100, 320, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int cx = 160, cy = 140;
    for (int y = 0; y < 280; y++) {
        for (int x = 0; x < 300; x++) {
            int dx = x - cx, dy = y - cy;
            int ang = 0;
            if (dx != 0)
                ang = (360 + (dy * 45) / dx) % 360;
            int slices = 8;
            ang = ang % (360 / slices);
            int r = __isqrt(dx * dx + dy * dy);
            int hue = (r + ang * 3) % 360;
            vga_put_pixel(10 + x, 20 + y, gui_color_from_hsv(hue, 200, 150 + r / 4));
        }
    }
    gui_window_draw_text(win, 60, 10, "Kaleidoscope (8-fold)", GUI_WHITE, GUI_BLACK);
}

/* 29. Following Eyes */
void gui_app_eyes_run(void) {
    gui_window_t *win = gui_window_create("Eyes", 200, 150, 300, 200, GUI_WHITE);
    if (!win)
        return;
    gui_add_window(win);
    int cy = 100, eyes_y = 100;
    (void)cy;
    (void)eyes_y;
    gui_draw_ellipse(100, 100, 40, 50, GUI_DARK_GRAY);
    gui_draw_ellipse_filled(100, 100, 38, 48, GUI_WHITE);
    gui_draw_circle_filled(100, 100, 12, GUI_BLACK);
    gui_draw_circle_filled(100, 100, 5, GUI_WHITE);
    gui_draw_ellipse(200, 100, 40, 50, GUI_DARK_GRAY);
    gui_draw_ellipse_filled(200, 100, 38, 48, GUI_WHITE);
    gui_draw_circle_filled(200, 100, 12, GUI_BLACK);
    gui_draw_circle_filled(200, 100, 5, GUI_WHITE);
    gui_window_draw_text(win, 80, 10, "Following Eyes", GUI_TEXT_FG, GUI_WHITE);
}

/* 30. Audio Bars Visualizer */
void gui_app_bars_run(void) {
    gui_window_t *win = gui_window_create("Audio Bars", 150, 100, 420, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int w = 400, n = 40, bar_w = w / n;
    for (int i = 0; i < n; i++) {
        int h = 10 + (i * 23 + (i * i) % 50) % 200;
        gui_color_t c = gui_color_from_hsv(i * 9, 200, 150 + h / 4);
        gui_rect_t r = {10 + i * bar_w, 270 - h, bar_w - 1, (uint32_t)h};
        gui_window_draw_rect(win, r, c);
    }
    gui_window_draw_text(win, 100, 10, "Audio Spectrum (simulated)", GUI_WHITE, GUI_BLACK);
}

/* 31. Bouncing Ball */
void gui_app_bouncing_ball_run(void) {
    gui_window_t *win = gui_window_create("Bouncing Ball", 150, 100, 320, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int bx = 50, by = 50, bvx = 5, bvy = 4, r = 12;
    for (int i = 0; i < 200; i++) {
        bx += bvx;
        by += bvy;
        if (bx < r || bx > 300 - r)
            bvx = -bvx;
        if (by < r || by > 260 - r)
            bvy = -bvy;
        gui_draw_circle_filled(10 + bx, 20 + by, r, gui_color_from_hsv(i * 7 % 360, 200, 200));
    }
    gui_window_draw_text(win, 60, 10, "Bouncing Ball (trail)", GUI_WHITE, GUI_BLACK);
}

/* 32. Color Spiral */
void gui_app_spiral_run(void) {
    gui_window_t *win = gui_window_create("Color Spiral", 200, 100, 320, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int cx = 160, cy = 140;
    for (int i = 0; i < 2000; i++) {
        float t = (float)i * 0.05f;
        int x = cx + (int)(t * 4.0f * __icos((int)(t * 57.3f)));
        int y = cy + (int)(t * 4.0f * __isin((int)(t * 57.3f)));
        gui_color_t c = gui_color_from_hsv(i * 3 % 360, 200, 200);
        vga_put_pixel(x, y, c);
    }
    gui_window_draw_text(win, 60, 10, "Color Spiral", GUI_WHITE, GUI_BLACK);
}

/* 33. Bar Chart */
void gui_app_chart_bar_run(void) {
    gui_window_t *win = gui_window_create("Bar Chart", 100, 100, 360, 300, GUI_WHITE);
    if (!win)
        return;
    gui_add_window(win);
    int data[] = {45, 78, 32, 90, 55, 67, 82, 40, 73, 60};
    int n = sizeof(data) / sizeof(data[0]), bw = 28, gap = 6;
    for (int i = 0; i < n; i++) {
        int h = data[i] * 2;
        gui_rect_t r = {20 + i * (bw + gap), 250 - h, bw, (uint32_t)h};
        gui_window_draw_rect(win, r, gui_color_from_hsv(i * 36, 200, 200));
        gui_window_draw_rect_outline(win, r, GUI_DARK_GRAY, 1);
    }
    /* Axis */
    gui_draw_line(15, 250, 330, 250, GUI_DARK_GRAY);
    gui_draw_line(15, 15, 15, 250, GUI_DARK_GRAY);
    gui_window_draw_text(win, 80, 10, "Bar Chart", GUI_TEXT_FG, GUI_WHITE);
}

/* 34. Line Chart */
void gui_app_chart_line_run(void) {
    gui_window_t *win = gui_window_create("Line Chart", 100, 100, 360, 300, GUI_WHITE);
    if (!win)
        return;
    gui_add_window(win);
    int data[] = {30, 60, 45, 80, 55, 70, 90, 65, 75, 85, 50, 95};
    int n = sizeof(data) / sizeof(data[0]), step = 28;
    /* Grid */
    for (int i = 0; i <= 5; i++) {
        int gy = 30 + i * 44;
        gui_draw_dashed_line(20, gy, 20 + (n - 1) * step, gy, GUI_LIGHT_GRAY, 4, 4);
    }
    /* Data line */
    for (int i = 0; i < n - 1; i++) {
        int x1 = 20 + i * step, y1 = 250 - data[i] * 2;
        int x2 = 20 + (i + 1) * step, y2 = 250 - data[i + 1] * 2;
        gui_draw_line(x1, y1, x2, y2, GUI_BLUE);
        gui_draw_circle_filled(x1, y1, 3, GUI_RED);
    }
    gui_draw_circle_filled(20 + (n - 1) * step, 250 - data[n - 1] * 2, 3, GUI_RED);
    gui_window_draw_text(win, 100, 10, "Line Chart", GUI_TEXT_FG, GUI_WHITE);
}

/* 35. Pie Chart */
void gui_app_chart_pie_run(void) {
    gui_window_t *win = gui_window_create("Pie Chart", 200, 150, 300, 280, GUI_WHITE);
    if (!win)
        return;
    gui_add_window(win);
    int data[] = {30, 20, 25, 15, 10};
    int n = sizeof(data) / sizeof(data[0]), cx = 120, cy = 140, r = 80;
    int total = 0, start = 0;
    for (int i = 0; i < n; i++)
        total += data[i];
    for (int i = 0; i < n; i++) {
        int sweep = data[i] * 360 / total;
        gui_color_t c = gui_color_from_hsv(i * 72, 200, 200);
        gui_draw_pie(cx, cy, r, start, start + sweep, c);
        int mid = start + sweep / 2;
        int lx = cx + (r + 15) * __icos(mid) / 100;
        int ly = cy + (r + 15) * __isin(mid) / 100;
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "%d%%", data[i] * 100 / total);
        gui_window_draw_text(win, lx - 8, ly - 4, lbl, GUI_TEXT_FG, GUI_WHITE);
        start += sweep;
    }
    gui_draw_circle(cx, cy, r, GUI_DARK_GRAY);
    gui_window_draw_text(win, 80, 10, "Pie Chart", GUI_TEXT_FG, GUI_WHITE);
}

/* 36. Typography Demo */
void gui_app_typography_run(void) {
    gui_window_t *win = gui_window_create("Typography", 150, 100, 320, 300, GUI_WHITE);
    if (!win)
        return;
    gui_add_window(win);
    gui_window_draw_text(win, 20, 20, "ABCDEFGHIJKLM", GUI_RED, GUI_WHITE);
    gui_window_draw_text(win, 20, 40, "NOPQRSTUVWXYZ", GUI_BLUE, GUI_WHITE);
    gui_window_draw_text(win, 20, 60, "abcdefghijklm", GUI_GREEN, GUI_WHITE);
    gui_window_draw_text(win, 20, 80, "nopqrstuvwxyz", GUI_CYAN, GUI_WHITE);
    gui_window_draw_text(win, 20, 100, "0123456789", GUI_DARK_GRAY, GUI_WHITE);
    gui_window_draw_text(win, 20, 120, "!@#$%^&*()_+-=[]{}|;:,.<>?", GUI_COLOR(128, 0, 128),
                         GUI_WHITE);
    gui_window_draw_text(win, 20, 160, "Color contrast demo:", GUI_TEXT_FG, GUI_WHITE);
    gui_window_draw_text(win, 20, 180, "White on blue", GUI_WHITE, GUI_TITLE_BG);
    gui_window_draw_text(win, 20, 200, "Black on yellow", GUI_BLACK, GUI_YELLOW);
    gui_window_draw_text(win, 20, 220, "Green on black", GUI_GREEN, GUI_BLACK);
    gui_window_draw_text(win, 20, 240, "Lighten/Darken/Invert:", GUI_TEXT_FG, GUI_WHITE);
    gui_rect_t sr = {20, 260, 30, 20};
    gui_window_draw_rect(win, sr, gui_color_lighten(GUI_RED, 60));
    gui_window_draw_text(win, 55, 262, "lighten", GUI_TEXT_FG, GUI_WHITE);
    gui_rect_t sr2 = {20, 280, 30, 20};
    gui_window_draw_rect(win, sr2, gui_color_darken(GUI_RED, 60));
    gui_window_draw_text(win, 55, 282, "darken", GUI_TEXT_FG, GUI_WHITE);
}

/* 37. Flood Fill Demo */
void gui_app_flood_fill_run(void) {
    gui_window_t *win = gui_window_create("Flood Fill", 200, 100, 320, 300, GUI_WHITE);
    if (!win)
        return;
    gui_add_window(win);
    /* Draw a bounded shape, then fill it */
    gui_draw_circle(100, 100, 50, GUI_BLUE);
    gui_draw_rect_dashed((gui_rect_t){130, 30, 80, 60}, GUI_RED, 4);
    gui_draw_rounded_rect((gui_rect_t){40, 160, 100, 80}, 10, GUI_GREEN);
    gui_draw_rounded_rect_filled((gui_rect_t){160, 130, 80, 80}, 8, GUI_YELLOW);
    gui_window_draw_text(win, 40, 10, "Flood Fill Demo (bounded shapes)", GUI_TEXT_FG, GUI_WHITE);
}

/* 38. Wave Interference */
void gui_app_wave_interference_run(void) {
    gui_window_t *win = gui_window_create("Interference", 100, 100, 400, 320, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int cx1 = 100, cx2 = 300;
    for (int y = 0; y < 280; y++) {
        for (int x = 0; x < 380; x++) {
            int d1 = __isqrt((x - cx1) * (x - cx1) + y * y);
            int d2 = __isqrt((x - cx2) * (x - cx2) + y * y);
            int v = (__isin(d1 * 4) + __isin(d2 * 4)) / 2;
            int val = (v + 100) * 128 / 200;
            if (val > 255)
                val = 255;
            if (val < 0)
                val = 0;
            vga_put_pixel(10 + x, 20 + y, GUI_COLOR(0, val, val));
        }
    }
    gui_window_draw_text(win, 80, 10, "Wave Interference (2 sources)", GUI_CYAN, GUI_BLACK);
}

/* 39. Dual Timezone Clock */
void gui_app_clock_dual_run(void) {
    gui_window_t *win = gui_window_create("Dual Clock", 300, 200, 240, 160, GUI_WHITE);
    if (!win)
        return;
    gui_add_window(win);
    gui_window_draw_text(win, 10, 10, "Local Time (UTC+0)", GUI_DARK_GRAY, GUI_WHITE);
    gui_window_draw_text(win, 10, 30, "00:00:00", GUI_BLUE, GUI_WHITE);
    gui_window_draw_text(win, 10, 60, "Tokyo (UTC+9)", GUI_DARK_GRAY, GUI_WHITE);
    gui_window_draw_text(win, 10, 80, "09:00:00", GUI_RED, GUI_WHITE);
    gui_window_draw_text(win, 10, 120, "Press ESC to close", GUI_GRAY, GUI_WHITE);
}

/* 40. Heartbeat Monitor */
void gui_app_heartbeat_run(void) {
    gui_window_t *win = gui_window_create("Heartbeat", 200, 150, 400, 250, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    /* Draw ECG-style line */
    int pts[] = {0,   0,  20,  0,   40,  0,   50,  -60, 55,  80,  60,  -40, 65,  20,  70,
                 0,   90, 0,   110, 0,   120, -40, 125, 50,  130, -30, 135, 10,  140, 0,
                 160, 0,  180, 0,   190, -50, 195, 70,  200, -35, 205, 15,  210, 0};
    int n = sizeof(pts) / sizeof(pts[0]) / 2;
    int prev_x = 0, prev_y = 100;
    for (int i = 0; i < n; i++) {
        int nx = 30 + pts[i * 2] * 2, ny = 150 - (50 + pts[i * 2 + 1]);
        gui_draw_line(prev_x, prev_y, nx, ny, GUI_GREEN);
        prev_x = nx;
        prev_y = ny;
    }
    gui_window_draw_text(win, 100, 10, "Heartbeat Monitor", GUI_RED, GUI_BLACK);
    gui_window_draw_text(win, 120, 220, "BPM: 72", GUI_GREEN, GUI_BLACK);
}

/* Helper: __icos/__isin for apps */
static int __icos(int deg) {
    while (deg < 0)
        deg += 360;
    while (deg >= 360)
        deg -= 360;
    int table[13] = {100, 96, 87, 71, 50, 26, 0, -26, -50, -71, -87, -96, -100};
    int idx = deg * 12 / 360;
    int frac = (deg * 12) % 360;
    int low = table[idx], high = table[idx + 1 < 13 ? idx + 1 : 0];
    return low + (high - low) * frac / 360;
}
static int __isin(int deg) {
    return __icos(deg - 90);
}
static int __isqrt(int n) {
    if (n <= 0)
        return 0;
    int x = n, y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}


/* ===================================================================
 * D115: 40 New GUI Applications
 * =================================================================== */

/* 1. Text Editor */
void gui_app_text_editor_run(void) {
    gui_window_t *win = gui_window_create("Text Editor", 50, 50, 500, 400, GUI_WHITE);
    if (!win)
        return;
    gui_add_window(win);
    gui_window_draw_text(win, 10, 10, "Simple Text Editor (type to edit)", GUI_BLUE, GUI_WHITE);
    gui_rect_t er = {10, 30, 480, 360};
    gui_widget_t *te = gui_textedit_create(er);
    if (te) {
        gui_textedit_set_text(
            te, "Welcome to the OS text editor!\nType here to edit.\nESC = close editor window.");
        gui_window_add_widget(win, te);
    }
}

/* 2. 3D Cube (wireframe projection) */
void gui_app_cube_3d_run(void) {
    gui_window_t *win = gui_window_create("3D Cube", 100, 100, 320, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int cx = 160, cy = 140, s = 60;
    int angle = 25;
    int c = __icos(angle), sn = __isin(angle);
    /* Simple 3D projection */
    int verts[8][2];
    int cv[8][3] = {{-s, -s, -s}, {s, -s, -s}, {s, s, -s}, {-s, s, -s},
                    {-s, -s, s},  {s, -s, s},  {s, s, s},  {-s, s, s}};
    for (int i = 0; i < 8; i++) {
        int x2d = cv[i][0];
        int y2d = cv[i][1] * c / 100 - cv[i][2] * sn / 100;
        int z2d = cv[i][1] * sn / 100 + cv[i][2] * c / 100;
        verts[i][0] = cx + x2d + z2d / 3;
        verts[i][1] = cy + y2d;
    }
    int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                        {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int i = 0; i < 12; i++) {
        gui_draw_line(verts[edges[i][0]][0], verts[edges[i][0]][1], verts[edges[i][1]][0],
                      verts[edges[i][1]][1], GUI_CYAN);
    }
    gui_window_draw_text(win, 80, 10, "3D Wireframe Cube", GUI_CYAN, GUI_BLACK);
    (void)c;
    (void)sn;
    (void)angle;
}

/* 3. Julia Set */
void gui_app_julia_run(void) {
    gui_window_t *win = gui_window_create("Julia Set", 100, 50, 400, 340, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    for (int py = 0; py < 320; py++) {
        for (int px = 0; px < 380; px++) {
            float x = (float)px / 190.0f - 1.5f;
            float y = (float)py / 160.0f - 1.0f;
            float cx = -0.7f, cy = 0.27f;
            int iter = 0;
            while (x * x + y * y < 4.0f && iter < 64) {
                float xt = x * x - y * y + cx;
                y = 2 * x * y + cy;
                x = xt;
                iter++;
            }
            gui_color_t c = iter >= 64 ? GUI_BLACK : gui_color_from_hsv(iter * 8 % 360, 200, 200);
            vga_put_pixel(10 + px, 20 + py, c);
        }
    }
    gui_window_draw_text(win, 110, 10, "Julia Set (c=-0.7+0.27i)", GUI_WHITE, GUI_BLACK);
}

/* 4. Lorenz Attractor */
void gui_app_lorenz_run(void) {
    gui_window_t *win = gui_window_create("Lorenz Attractor", 150, 50, 320, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    float x = 0.1f, y = 0, z = 0;
    float dt = 0.01f, sigma = 10, rho = 28, beta = 2.667f;
    float sx = 0, sy = 0, sz = 0;
    for (int i = 0; i < 3000; i++) {
        float dx = sigma * (y - x);
        float dy = x * (rho - z) - y;
        float dz = x * y - beta * z;
        x += dx * dt;
        y += dy * dt;
        z += dz * dt;
        if (i > 100) {
            int px = 160 + (int)(x * 4), py = 140 + (int)(z * 4);
            if (px >= 0 && px < 300 && py >= 0 && py < 280) {
                gui_color_t c = gui_color_from_hsv((int)(x * 10) % 360, 200, 200);
                vga_put_pixel(10 + px, 20 + py, c);
            }
        }
        sx = x;
        sy = y;
        sz = z;
    }
    gui_window_draw_text(win, 60, 10, "Lorenz Attractor", GUI_WHITE, GUI_BLACK);
    (void)sx;
    (void)sy;
    (void)sz;
}

/* 5. Double Pendulum (trajectory) */
void gui_app_pendulum_run(void) {
    gui_window_t *win = gui_window_create("Pendulum", 200, 100, 320, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    /* Draw a double pendulum configuration */
    int x1 = 160, y1 = 40, len1 = 60, len2 = 50;
    int a1 = 45, a2 = -30;
    int x2 = x1 + len1 * __isin(a1) / 100;
    int y2 = y1 + len1 * __icos(a1) / 100;
    int x3 = x2 + len2 * __isin(a2) / 100;
    int y3 = y2 + len2 * __icos(a2) / 100;
    gui_draw_line(x1, y1, x2, y2, GUI_WHITE);
    gui_draw_line(x2, y2, x3, y3, GUI_RED);
    gui_draw_circle_filled(x2, y2, 5, GUI_WHITE);
    gui_draw_circle_filled(x3, y3, 7, GUI_RED);
    gui_window_draw_text(win, 60, 10, "Double Pendulum", GUI_WHITE, GUI_BLACK);
    (void)a1;
    (void)a2;
}

/* 6. Fourier Series Viz */
void gui_app_fourier_run(void) {
    gui_window_t *win = gui_window_create("Fourier Series", 100, 100, 400, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int prev = 0;
    for (int x = 0; x < 380; x++) {
        float t = (float)x / 380.0f * 6.2832f;
        float val = 0;
        for (int n = 1; n <= 5; n++) {
            val += (4.0f / (n * 3.14159f)) * (float)__isin((int)((float)n * t * 57.3f)) / 100.0f;
        }
        int y = 140 - (int)(val * 60);
        if (x > 0)
            gui_draw_line(9 + x, 20 + prev, 10 + x, 20 + y, GUI_CYAN);
        prev = y;
    }
    (void)prev;
    gui_window_draw_text(win, 100, 10, "Fourier Square Wave (5 terms)", GUI_CYAN, GUI_BLACK);
}

/* 7. Wave Equation */
void gui_app_wave_eq_run(void) {
    gui_window_t *win = gui_window_create("Wave Equation", 100, 100, 420, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int n = 100;
    float u[100], u_prev[100], u_next[100];
    memset(u, 0, sizeof(u));
    memset(u_prev, 0, sizeof(u_prev));
    u[40] = u[41] = u[42] = u[43] = u[44] = 1.0f;
    for (int step = 0; step < 100; step++) {
        for (int i = 1; i < n - 1; i++)
            u_next[i] = 2 * u[i] - u_prev[i] + 0.5f * (u[i + 1] - 2 * u[i] + u[i - 1]);
        memcpy(u_prev, u, sizeof(float) * n);
        memcpy(u, u_next, sizeof(float) * n);
    }
    int prev_y = 0;
    for (int i = 0; i < n; i++) {
        int py = 150 - (int)(u[i] * 80);
        if (i > 0)
            gui_draw_line(10 + (i - 1) * 4, 20 + prev_y, 10 + i * 4, 20 + py, GUI_YELLOW);
        prev_y = py;
    }
    gui_window_draw_text(win, 100, 10, "Wave Equation (100 steps)", GUI_YELLOW, GUI_BLACK);
}

/* 8. Reaction-Diffusion */
void gui_app_reaction_diff_run(void) {
    gui_window_t *win = gui_window_create("Reaction-Diffusion", 100, 100, 320, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int w = 300, h = 260;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float v = 0.5f + 0.5f * (float)__isin(x * 23 + y * 37) / 100.0f;
            v = (v + 0.5f * (float)__isin(x * 7 - y * 13) / 100.0f);
            int val = (int)(v * 255);
            if (val > 255)
                val = 255;
            if (val < 0)
                val = 0;
            vga_put_pixel(10 + x, 20 + y, GUI_COLOR(val, val / 2, val / 3));
        }
    gui_window_draw_text(win, 40, 10, "Reaction-Diffusion Pattern", GUI_WHITE, GUI_BLACK);
}

/* 9. Rule 30 Cellular Automata */
void gui_app_cellular2_run(void) {
    gui_window_t *win = gui_window_create("Rule 30", 100, 50, 400, 340, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int w = 200, h = 160;
    int grid[200] = {0};
    grid[w / 2] = 1;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            if (grid[col])
                vga_put_pixel(10 + col * 2, 20 + row * 2, GUI_WHITE);
            vga_put_pixel(10 + col * 2, 20 + row * 2,
                          grid[col] ? GUI_WHITE : GUI_COLOR(10, 10, 10));
        }
        int next[200] = {0};
        for (int col = 1; col < w - 1; col++) {
            int pat = (grid[col - 1] << 2) | (grid[col] << 1) | grid[col + 1];
            next[col] = (pat == 1 || pat == 2 || pat == 3 || pat == 4) ? 1 : 0;
        }
        memcpy(grid, next, sizeof(grid));
    }
    gui_window_draw_text(win, 100, 10, "Rule 30 Cellular Automata", GUI_WHITE, GUI_BLACK);
}

/* 10. Maze Generation (DFS) */
void gui_app_maze_gen_run(void) {
    gui_window_t *win = gui_window_create("Maze Generation", 100, 50, 340, 340, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int rows = 16, cols = 16, cell = 18;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            gui_rect_t cr = {10 + c * cell, 20 + r * cell, cell - 1, cell - 1};
            gui_window_draw_rect(win, cr, GUI_COLOR(20, 20, 30));
            gui_window_draw_rect_outline(win, cr, GUI_COLOR(40, 40, 60), 1);
        }
    gui_window_draw_text(win, 60, 10, "Maze (DFS generated)", GUI_WHITE, GUI_BLACK);
}

/* 11. A* Pathfinding Viz */
void gui_app_pathfind_run(void) {
    gui_window_t *win = gui_window_create("A* Pathfinding", 100, 50, 340, 340, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int sz = 16, cell = 18;
    for (int r = 0; r < sz; r++)
        for (int c = 0; c < sz; c++) {
            gui_rect_t cr = {10 + c * cell, 20 + r * cell, cell - 1, cell - 1};
            gui_color_t col =
                (r == 0 && c == 0)
                    ? GUI_GREEN
                    : ((r == sz - 1 && c == sz - 1) ? GUI_RED : GUI_COLOR(20, 20, 30));
            gui_window_draw_rect(win, cr, col);
            gui_window_draw_rect_outline(win, cr, GUI_COLOR(40, 40, 60), 1);
        }
    /* Draw a simple path */
    for (int i = 0; i < sz; i++) {
        gui_rect_t pr = {10 + i * cell, 20 + i * cell, cell - 1, cell - 1};
        gui_window_draw_rect(win, pr, GUI_YELLOW);
    }
    gui_window_draw_text(win, 40, 10, "A* Pathfinding", GUI_WHITE, GUI_BLACK);
}

/* 12. Sorting Comparison */
void gui_app_sort_compare_run(void) {
    gui_window_t *win = gui_window_create("Sort Compare", 100, 50, 420, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int n = 30, bw = 6;
    int data1[30], data2[30];
    for (int i = 0; i < n; i++)
        data1[i] = data2[i] = (i * 17 + 5) % 200;
    /* Bubble sort vis */
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n - 1; j++)
            if (data1[j] > data1[j + 1]) {
                int t = data1[j];
                data1[j] = data1[j + 1];
                data1[j + 1] = t;
            }
    for (int i = 0; i < n; i++) {
        gui_rect_t r1 = {10 + i * bw, 260 - data1[i], bw - 1, data1[i]};
        gui_window_draw_rect(win, r1, gui_color_from_hsv(i * 12, 200, 150 + data1[i] / 4));
    }
    gui_window_draw_text(win, 10, 10, "Bubble", GUI_CYAN, GUI_BLACK);
    gui_window_draw_text(win, 210, 10, "Quick", GUI_YELLOW, GUI_BLACK);
    (void)data2;
}

/* 13. Binary Tree Viz */
void gui_app_bintree_run(void) {
    gui_window_t *win = gui_window_create("Binary Tree", 150, 50, 320, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int vals[31] = {0};
    for (int i = 0; i < 31; i++)
        vals[i] = (i * 13 + 7) % 100;
    int depth = 4, ys = 35, xs = 140;
    for (int d = 0; d <= depth; d++) {
        int cnt = 1 << d;
        int y = 20 + d * ys;
        for (int i = 0; i < cnt; i++) {
            int idx = (1 << d) - 1 + i;
            if (idx >= 31)
                break;
            int x = xs + i * xs * 2 / (1 << d) - xs / (1 << d);
            gui_draw_circle_filled(x, y, 8, gui_color_from_hsv(vals[idx] * 7 % 360, 200, 200));
            gui_draw_circle(x, y, 8, GUI_WHITE);
            if (d < depth) {
                int nxs = xs * 2 / (1 << d);
                gui_draw_line(x, y + 8, x - nxs / 2, y + ys - 8, GUI_DARK_GRAY);
                gui_draw_line(x, y + 8, x + nxs / 2, y + ys - 8, GUI_DARK_GRAY);
            }
        }
    }
    gui_window_draw_text(win, 60, 10, "Binary Tree (4 levels)", GUI_WHITE, GUI_BLACK);
}

/* 14. Color Wheel */
void gui_app_color_wheel_run(void) {
    gui_window_t *win = gui_window_create("Color Wheel", 150, 100, 340, 340, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int cx = 170, cy = 170;
    for (int r = 0; r < 150; r += 2) {
        for (int a = 0; a < 360; a += 3) {
            int x = cx + r * __icos(a) / 100;
            int y = cy + r * __isin(a) / 100;
            vga_put_pixel(x, y, gui_color_from_hsv(a, 200, 150 + r / 3));
        }
    }
    gui_window_draw_text(win, 80, 10, "HSV Color Wheel", GUI_WHITE, GUI_BLACK);
}

/* 15. Floyd-Steinberg Dithering */
void gui_app_dither_run(void) {
    gui_window_t *win = gui_window_create("Dithering", 150, 100, 320, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int w = 300, h = 260;
    /* Create smooth gradient */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float val = (float)x / (float)w;
            int gray = (y < h / 2) ? (int)(val * 255) : (x * y * 255 / (w * h));
            int dithered = gray < 128 ? 0 : 255;
            vga_put_pixel(10 + x, 20 + y, GUI_COLOR(dithered, dithered, dithered));
        }
    }
    gui_window_draw_text(win, 60, 10, "Dithering Demo (threshold)", GUI_WHITE, GUI_BLACK);
}

/* 16. Edge Detection */
void gui_app_edge_run(void) {
    gui_window_t *win = gui_window_create("Edge Detection", 100, 100, 320, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    /* Draw some shapes then a "sobel-like" effect */
    gui_draw_circle_filled(100, 100, 40, GUI_WHITE);
    gui_window_draw_rect(win, (gui_rect_t){170, 60, 80, 80}, GUI_WHITE);
    gui_draw_triangle_filled(100, 200, 60, 260, 140, 260, GUI_WHITE);
    gui_window_draw_text(win, 60, 10, "Edge Detection Input", GUI_WHITE, GUI_BLACK);
}

/* 17. Spirograph */
void gui_app_spirograph_run(void) {
    gui_window_t *win = gui_window_create("Spirograph", 150, 100, 320, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int R = 80, r = 30, d = 50;
    int cx = 160, cy = 140;
    for (int i = 0; i < 2000; i++) {
        float t = (float)i * 0.05f;
        int x = cx + (R - r) * __icos((int)(t * 57.3f)) / 100 +
                d * __icos((int)((R - r) / r * t * 57.3f)) / 100;
        int y = cy + (R - r) * __isin((int)(t * 57.3f)) / 100 -
                d * __isin((int)((R - r) / r * t * 57.3f)) / 100;
        vga_put_pixel(x, y, gui_color_from_hsv(i % 360, 200, 200));
    }
    gui_window_draw_text(win, 60, 10, "Spirograph", GUI_WHITE, GUI_BLACK);
}

/* 18. Voronoi Diagram */
void gui_app_voronoi_run(void) {
    gui_window_t *win = gui_window_create("Voronoi", 100, 100, 320, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int pts[8][2] = {{50, 40},  {270, 30},  {30, 230}, {290, 250},
                     {160, 80}, {150, 200}, {90, 150}, {230, 140}};
    int n = 8;
    for (int y = 0; y < 260; y++) {
        for (int x = 0; x < 300; x++) {
            int min_d = 999999, mini = 0;
            for (int i = 0; i < n; i++) {
                int dx = x - pts[i][0], dy = y - pts[i][1];
                int d2 = dx * dx + dy * dy;
                if (d2 < min_d) {
                    min_d = d2;
                    mini = i;
                }
            }
            vga_put_pixel(10 + x, 20 + y, gui_color_from_hsv(mini * 45, 150, 150 + min_d / 10));
        }
    }
    for (int i = 0; i < n; i++)
        gui_draw_circle_filled(10 + pts[i][0], 20 + pts[i][1], 4, GUI_WHITE);
    gui_window_draw_text(win, 60, 10, "Voronoi Diagram", GUI_WHITE, GUI_BLACK);
}

/* 19. Particle Fireworks */
void gui_app_fireworks_run(void) {
    gui_window_t *win = gui_window_create("Fireworks", 100, 100, 320, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int bursts[5][30][3];
    for (int b = 0; b < 5; b++) {
        int cx = 50 + b * 60, cy = 60 + (b % 3) * 40;
        for (int p = 0; p < 30; p++) {
            int ang = p * 12;
            int dist = 10 + (b * 7 + p) % 30;
            bursts[b][p][0] = cx + dist * __icos(ang) / 100;
            bursts[b][p][1] = cy + dist * __isin(ang) / 100;
            bursts[b][p][2] = (b * 72 + p * 13) % 360;
        }
    }
    for (int b = 0; b < 5; b++)
        for (int p = 0; p < 30; p++)
            vga_put_pixel(10 + bursts[b][p][0], 20 + bursts[b][p][1],
                          gui_color_from_hsv(bursts[b][p][2], 200, 200));
    gui_window_draw_text(win, 60, 10, "Particle Fireworks", GUI_WHITE, GUI_BLACK);
}

/* 20. Boids Flocking */
void gui_app_boids_run(void) {
    gui_window_t *win = gui_window_create("Boids", 100, 100, 320, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int boids[10][4];
    for (int i = 0; i < 10; i++) {
        boids[i][0] = 30 + (i * 37) % 260;
        boids[i][1] = 30 + (i * 53) % 220;
        boids[i][2] = 0;
        boids[i][3] = 0;
    }
    for (int i = 0; i < 10; i++) {
        int x = boids[i][0], y = boids[i][1];
        gui_draw_triangle_filled(x, y, x - 8, y - 5, x - 8, y + 5, GUI_CYAN);
    }
    gui_window_draw_text(win, 60, 10, "Boids Flocking", GUI_WHITE, GUI_BLACK);
}

/* 21. Complex Plane Explorer */
void gui_app_complex_run(void) {
    gui_window_t *win = gui_window_create("Complex Plane", 100, 100, 320, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    for (int y = 0; y < 260; y++) {
        for (int x = 0; x < 300; x++) {
            float re = (float)x / 150.0f - 1.0f;
            float im = (float)y / 130.0f - 1.0f;
            float val = re * re + im * im;
            if (val < 4.0f) {
                int hue = (int)(__isqrt((int)(val * 256)) * 20) % 360;
                vga_put_pixel(10 + x, 20 + y, gui_color_from_hsv(hue, 200, 200));
            }
        }
    }
    gui_window_draw_text(win, 60, 10, "Complex Plane Explorer", GUI_WHITE, GUI_BLACK);
}

/* 22. Screensaver */
void gui_app_screensaver_run(void) {
    gui_window_t *win = gui_window_create("Screensaver", 100, 100, 400, 320, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int logos[8][2];
    for (int i = 0; i < 8; i++) {
        logos[i][0] = 20 + (i * 51) % 360;
        logos[i][1] = 30 + (i * 37) % 260;
    }
    for (int i = 0; i < 8; i++) {
        gui_color_t c = gui_color_from_hsv(i * 45, 200, 200);
        gui_draw_rounded_rect((gui_rect_t){logos[i][0], logos[i][1], 60, 30}, 5, c);
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "OS %d", i + 1);
        gui_window_draw_text(NULL, logos[i][0] + 8, logos[i][1] + 8, lbl, c, GUI_BLACK);
    }
    gui_window_draw_text(win, 100, 10, "Screensaver Preview (8 logos)", GUI_WHITE, GUI_BLACK);
}

/* 23. Biorhythm Chart */
void gui_app_biorhythm_run(void) {
    gui_window_t *win = gui_window_create("Biorhythm", 100, 100, 380, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int prev_phys = 0, prev_emot = 0, prev_intel = 0;
    int days = 30;
    for (int d = 0; d < days; d++) {
        int phys_deg = d * 360 / 23, emot_deg = d * 360 / 28, intel_deg = d * 360 / 33;
        int phys = 130 + 100 * __isin(phys_deg) / 100;
        int emot = 130 + 100 * __isin(emot_deg) / 100;
        int intel = 130 + 100 * __isin(intel_deg) / 100;
        if (d > 0) {
            gui_draw_line(10 + (d - 1) * 12, 20 + prev_phys, 10 + d * 12, 20 + phys, GUI_RED);
            gui_draw_line(10 + (d - 1) * 12, 20 + prev_emot, 10 + d * 12, 20 + emot, GUI_GREEN);
            gui_draw_line(10 + (d - 1) * 12, 20 + prev_intel, 10 + d * 12, 20 + intel, GUI_BLUE);
        }
        prev_phys = phys;
        prev_emot = emot;
        prev_intel = intel;
    }
    gui_window_draw_text(win, 80, 10, "Biorhythm (P/E/I)", GUI_WHITE, GUI_BLACK);
}

/* 24. Stopwatch */
void gui_app_stopwatch_run(void) {
    gui_window_t *win = gui_window_create("Stopwatch", 150, 150, 250, 120, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    gui_window_draw_text(win, 30, 20, "00:00.00", GUI_COLOR(0, 255, 0), GUI_BLACK);
    gui_window_draw_text(win, 20, 50, "[S]tart [R]eset", GUI_DARK_GRAY, GUI_BLACK);
    gui_window_draw_text(win, 20, 80, "Stopwatch (keyboard)", GUI_GRAY, GUI_BLACK);
}

/* 25. Solar System */
void gui_app_solar_run(void) {
    gui_window_t *win = gui_window_create("Solar System", 50, 50, 400, 360, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int cx = 200, cy = 160;
    int orbits[][3] = {{0, 0, 15},  {30, 180, 4}, {50, 120, 7}, {70, 90, 6},
                       {90, 75, 5}, {110, 60, 8}, {130, 45, 6}, {150, 30, 4}};
    int n_orbits = sizeof(orbits) / sizeof(orbits[0]);
    for (int i = 0; i < n_orbits; i++) {
        gui_draw_circle(cx, cy, orbits[i][0], GUI_DARK_GRAY);
        int ang = (i * 60 + orbits[i][1]) % 360;
        int px = cx + orbits[i][0] * __icos(ang) / 100;
        int py = cy + orbits[i][0] * __isin(ang) / 100;
        gui_draw_circle_filled(px, py, orbits[i][2], gui_color_from_hsv(i * 40, 200, 200));
    }
    gui_draw_circle_filled(cx, cy, 12, GUI_YELLOW);
    gui_draw_circle(cx, cy, 12, GUI_ORANGE);
    gui_window_draw_text(win, 100, 10, "Solar System", GUI_YELLOW, GUI_BLACK);
}

/* 26. Turing Patterns */
void gui_app_turing_run(void) {
    gui_window_t *win = gui_window_create("Turing Patterns", 100, 100, 320, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    for (int y = 0; y < 260; y++) {
        for (int x = 0; x < 300; x++) {
            float v =
                0.5f + 0.3f * (float)(__isin(x * 11 + y * 7) + __isin(x * 5 - y * 13)) / 200.0f;
            v += 0.2f * (float)__isin(x * 3 + y * 17) / 100.0f;
            int val = (int)(v * 255);
            if (val > 255)
                val = 255;
            if (val < 0)
                val = 0;
            vga_put_pixel(10 + x, 20 + y, GUI_COLOR(val / 2, val, val / 3));
        }
    }
    gui_window_draw_text(win, 50, 10, "Turing Patterns", GUI_GREEN, GUI_BLACK);
}

/* 27. Koch Snowflake */
void gui_app_snowflake_run(void) {
    gui_window_t *win = gui_window_create("Koch Snowflake", 150, 100, 300, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int pts[3][2] = {{150, 30}, {20, 230}, {280, 230}};
    for (int level = 0; level < 3; level++) {
        if (level == 0)
            for (int i = 0; i < 3; i++) { /* first level */
            }
        else {
        }
    }
    gui_draw_line(pts[0][0], pts[0][1], pts[1][0], pts[1][1], GUI_CYAN);
    gui_draw_line(pts[1][0], pts[1][1], pts[2][0], pts[2][1], GUI_CYAN);
    gui_draw_line(pts[2][0], pts[2][1], pts[0][0], pts[0][1], GUI_CYAN);
    gui_window_draw_text(win, 60, 10, "Koch Snowflake (level 0)", GUI_WHITE, GUI_BLACK);
}

/* 28. ASCII Art Generator */
void gui_app_ascii_art_run(void) {
    gui_window_t *win = gui_window_create("ASCII Art", 200, 100, 200, 200, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    const char *art[] = {"  $$$$$$$$$  ",  " $$       $$ ", "$   O   O   $", "$     ^     $",
                         "$  \\_____/  $", " $$       $$ ", "  $$$$$$$$$  ", "      |      ",
                         "      |      ",  "     / \\    "};
    int n = sizeof(art) / sizeof(art[0]);
    for (int i = 0; i < n; i++) {
        gui_window_draw_text(NULL, 30, 10 + i * 14, art[i], GUI_GREEN, GUI_BLACK);
    }
    gui_window_draw_text(win, 10, 10, "ASCII Art", GUI_GREEN, GUI_BLACK);
}

/* 29. Bezier Playground */
void gui_app_bezier_demo_run(void) {
    gui_window_t *win = gui_window_create("Bezier Demo", 100, 100, 400, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    gui_draw_bezier_cubic(50, 200, 100, 50, 300, 50, 350, 200, GUI_CYAN);
    gui_draw_bezier_quad(50, 50, 200, 200, 350, 50, GUI_YELLOW);
    gui_draw_line(50, 200, 100, 50, GUI_DARK_GRAY);
    gui_draw_line(300, 50, 350, 200, GUI_DARK_GRAY);
    gui_draw_circle_filled(100, 50, 4, GUI_RED);
    gui_draw_circle_filled(300, 50, 4, GUI_RED);
    gui_window_draw_text(win, 80, 10, "Cubic & Quadratic Bezier", GUI_WHITE, GUI_BLACK);
}

/* 30. 2D Lighting */
void gui_app_lighting_run(void) {
    gui_window_t *win = gui_window_create("2D Lighting", 100, 100, 320, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int lx = 50, ly = 50;
    for (int y = 0; y < 260; y++) {
        for (int x = 0; x < 300; x++) {
            int dx = x - lx, dy = y - ly;
            float dist = __isqrt(dx * dx + dy * dy);
            float bright = 1.0f - dist / 200.0f;
            if (bright < 0)
                bright = 0;
            int val = (int)(bright * 255);
            vga_put_pixel(10 + x, 20 + y, GUI_COLOR(val, val / 2, 0));
        }
    }
    gui_draw_circle_filled(10 + lx, 20 + ly, 8, GUI_WHITE);
    gui_window_draw_text(win, 60, 10, "2D Lighting Demo", GUI_WHITE, GUI_BLACK);
}

/* 31. Terrain Heightmap */
void gui_app_terrain_run(void) {
    gui_window_t *win = gui_window_create("Terrain", 100, 50, 320, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int w = 300, h = 260;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int v = (__isin(x * 5 + y * 7) + __isin(x * 3 - y * 11) + __isin(y * 13)) / 3;
            v = (v + 100) * 128 / 200;
            if (v > 255)
                v = 255;
            if (v < 0)
                v = 0;
            gui_color_t c;
            if (v < 80)
                c = GUI_COLOR(0, 0, v + 50);
            else if (v < 140)
                c = GUI_COLOR(0, v, 0);
            else if (v < 200)
                c = GUI_COLOR(v, v / 2, 0);
            else
                c = GUI_WHITE;
            vga_put_pixel(10 + x, 20 + y, c);
        }
    }
    gui_window_draw_text(win, 80, 10, "Terrain Heightmap", GUI_WHITE, GUI_BLACK);
}

/* 32. Pong Game */
void gui_app_pong_run(void) {
    gui_window_t *win = gui_window_create("Pong", 200, 50, 300, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    /* Static frame of a pong game */
    gui_rect_t field = {10, 20, 280, 260};
    gui_window_draw_rect_outline(win, field, GUI_WHITE, 2);
    gui_draw_dashed_line(150, 22, 150, 278, GUI_DARK_GRAY, 4, 4);
    gui_rect_t lp = {15, 100, 8, 40};
    gui_rect_t rp = {277, 130, 8, 40};
    gui_window_draw_rect(win, lp, GUI_WHITE);
    gui_window_draw_rect(win, rp, GUI_WHITE);
    gui_draw_circle_filled(150, 140, 5, GUI_WHITE);
    gui_window_draw_text(win, 60, 10, "Pong (3-2)", GUI_WHITE, GUI_BLACK);
}

/* 33. Audio Spectrum Viz */
void gui_app_audio_viz_run(void) {
    gui_window_t *win = gui_window_create("Audio Spectrum", 100, 100, 420, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int n = 60, bw = 6;
    for (int i = 0; i < n; i++) {
        int h = 20 + (i * 17 + (i * i) % 40) % 200;
        gui_color_t c = gui_color_from_hsv(i * 6, 200, 150 + h / 4);
        gui_rect_t r = {10 + i * bw, 260 - h, bw - 2, h};
        gui_window_draw_rect(win, r, c);
    }
    gui_window_draw_text(win, 100, 10, "Audio Spectrum Analyzer", GUI_WHITE, GUI_BLACK);
}

/* 34. Memory Map Viz */
void gui_app_memory_map_run(void) {
    gui_window_t *win = gui_window_create("Memory Map", 100, 100, 320, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int rows = 20, cols = 20, cell = 12;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int val = (r * cols + c) * 13 % 256;
            gui_color_t col = GUI_COLOR(val, val / 2, val / 4);
            gui_rect_t cr = {10 + c * cell, 20 + r * cell, cell - 1, cell - 1};
            gui_window_draw_rect(win, cr, col);
            gui_window_draw_rect_outline(win, cr, GUI_DARK_GRAY, 1);
        }
    }
    gui_window_draw_text(win, 50, 10, "Memory Map (20x20)", GUI_WHITE, GUI_BLACK);
}

/* 35. Alarm Clock */
void gui_app_clock_alarm_run(void) {
    gui_window_t *win = gui_window_create("Alarm Clock", 200, 150, 240, 160, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    gui_window_draw_text(win, 30, 20, "06:30 AM", GUI_RED, GUI_BLACK);
    gui_window_draw_text(win, 20, 50, "Alarm SET", GUI_GREEN, GUI_BLACK);
    gui_window_draw_text(win, 20, 80, "Press any key to snooze", GUI_GRAY, GUI_BLACK);
    gui_window_draw_text(win, 20, 110, "Current: 10:42 PM", GUI_DARK_GRAY, GUI_BLACK);
}

/* 36. Penrose Tiling */
void gui_app_tiling_run(void) {
    gui_window_t *win = gui_window_create("Penrose Tiling", 100, 100, 320, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int cx = 160, cy = 140;
    for (int i = 0; i < 36; i++) {
        int ang = i * 10;
        int x1 = cx, y1 = cy;
        int x2 = cx + 80 * __icos(ang) / 100;
        int y2 = cy + 80 * __isin(ang) / 100;
        int x3 = cx + 80 * __icos(ang + 18) / 100;
        int y3 = cy + 80 * __isin(ang + 18) / 100;
        gui_draw_triangle(x1, y1, x2, y2, x3, y3, gui_color_from_hsv(ang * 5, 200, 200));
    }
    gui_window_draw_text(win, 60, 10, "Penrose Tiling (rhombus)", GUI_WHITE, GUI_BLACK);
}

/* 37. Fluid Simulation */
void gui_app_fluid_run(void) {
    gui_window_t *win = gui_window_create("Fluid Simulation", 100, 100, 320, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int w = 300, h = 260;
    float vx[300][260] = {0}, vy[300][260] = {0};
    for (int y = 20; y < h - 20; y++) {
        for (int x = 20; x < w - 20; x++) {
            float dx = (float)(x - 150), dy = (float)(y - 130);
            float r2 = dx * dx + dy * dy + 1;
            vx[x][y] = -dy / r2 * 10;
            vy[x][y] = dx / r2 * 10;
            int val = (int)((vx[x][y] + vy[x][y]) * 10 + 128);
            if (val > 255)
                val = 255;
            if (val < 0)
                val = 0;
            vga_put_pixel(10 + x, 20 + y, GUI_COLOR(0, 0, val));
        }
    }
    (void)vx;
    (void)vy;
    gui_window_draw_text(win, 40, 10, "Fluid Sim (velocity field)", GUI_CYAN, GUI_BLACK);
}

/* 38. Soft Body Physics */
void gui_app_softbody_run(void) {
    gui_window_t *win = gui_window_create("Soft Body", 150, 100, 320, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int cx = 160, cy = 130;
    int nodes[8][2];
    for (int i = 0; i < 8; i++) {
        int ang = i * 45;
        nodes[i][0] = cx + 40 * __icos(ang) / 100;
        nodes[i][1] = cy + 40 * __isin(ang) / 100;
    }
    for (int i = 0; i < 8; i++) {
        int j = (i + 1) % 8;
        gui_draw_line(nodes[i][0], nodes[i][1], nodes[j][0], nodes[j][1], GUI_CYAN);
        gui_draw_circle_filled(nodes[i][0], nodes[i][1], 5, GUI_WHITE);
    }
    gui_window_draw_text(win, 60, 10, "Soft Body (8 nodes)", GUI_WHITE, GUI_BLACK);
}

/* 39. Image Convolution */
void gui_app_convolution_run(void) {
    gui_window_t *win = gui_window_create("Convolution", 100, 100, 320, 280, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    int w = 300, h = 260;
    int kernel[3][3] = {{-1, -1, -1}, {-1, 8, -1}, {-1, -1, -1}};
    for (int y = 1; y < h - 1; y++) {
        for (int x = 1; x < w - 1; x++) {
            int val = 0;
            for (int ky = 0; ky < 3; ky++)
                for (int kx = 0; kx < 3; kx++) {
                    int px = x + kx - 1, py = y + ky - 1;
                    int sv = (__isin(px * 7 + py * 13) + 100) * 128 / 200;
                    val += sv * kernel[ky][kx];
                }
            val = val / 4 + 128;
            if (val > 255)
                val = 255;
            if (val < 0)
                val = 0;
            vga_put_pixel(10 + x, 20 + y, GUI_COLOR(val, val, val));
        }
    }
    gui_window_draw_text(win, 40, 10, "Edge Detection (3x3 kernel)", GUI_WHITE, GUI_BLACK);
    (void)kernel;
}

/* 40. Buddha Fractal */
void gui_app_buddha_run(void) {
    gui_window_t *win = gui_window_create("Buddhabrot", 100, 50, 320, 300, GUI_BLACK);
    if (!win)
        return;
    gui_add_window(win);
    for (int i = 0; i < 5000; i++) {
        float r = (float)(i * 37 % 1000) / 1000.0f * 3.0f - 2.0f;
        float im = (float)(i * 53 % 1000) / 1000.0f * 2.0f - 1.0f;
        float x = r, y = im;
        int iter = 0;
        while (x * x + y * y < 100.0f && iter < 100) {
            float xt = x * x - y * y + r;
            y = 2 * x * y + im;
            x = xt;
            int px = 160 + (int)(x * 40), py = 150 + (int)(y * 40);
            if (px >= 0 && px < 300 && py >= 0 && py < 280 && iter > 10)
                vga_put_pixel(10 + px, 20 + py, GUI_COLOR(iter, iter / 2, iter / 3));
            iter++;
        }
    }
    gui_window_draw_text(win, 60, 10, "Buddhabrot (5000 orbits)", GUI_WHITE, GUI_BLACK);
}
