/* gui_apps.c — GUI application programs */
#include "gui_apps.h"
#include "gui.h"
#include "gui_draw.h"
#include "string.h"
#include "stdlib.h"

/* ── Drawing Demo ── */
void gui_app_draw_run(void) {
    gui_window_t *win = gui_window_create("Drawing Demo", 50, 50, 500, 400, GUI_WHITE);
    if (!win) return;
    gui_add_window(win);
    
    gui_rect_t r = {70, 30, 100, 80};
    gui_window_draw_rect(NULL, r, GUI_LIGHT_GRAY);
    gui_window_draw_rect_outline(NULL, r, GUI_RED, 2);
    
    gui_draw_line(200, 30, 350, 110, GUI_BLUE);
    gui_draw_line(200, 110, 350, 30, GUI_GREEN);
    
    gui_draw_circle(120, 200, 40, GUI_CYAN);
    gui_draw_circle_filled(250, 200, 35, GUI_YELLOW);
    
    gui_draw_triangle(400, 30, 370, 110, 430, 110, GUI_RED);
    gui_draw_triangle_filled(400, 140, 370, 220, 430, 220, GUI_COLOR(0,200,100));
    
    gui_draw_progress_bar(70, 290, 260, 20, 67, GUI_GREEN, GUI_LIGHT_GRAY);
    gui_draw_progress_bar(70, 320, 260, 20, 33, GUI_RED, GUI_LIGHT_GRAY);
    
    gui_draw_gradient_v(350, 140, 80, 200, GUI_BLUE, GUI_CYAN);
    gui_draw_gradient_h(30, 360, 440, 30, GUI_RED, GUI_BLUE);
    
    gui_window_draw_text(win, 70, 270, "67% complete", GUI_TEXT_FG, GUI_WHITE);
}

/* ── Widget Demo ── */
void gui_app_widgets_run(void) {
    gui_window_t *win = gui_window_create("Widget Demo", 150, 100, 400, 350, GUI_WINDOW_BG);
    if (!win) return;
    gui_add_window(win);
    
    gui_rect_t lr = {20, 10, 200, 16};
    gui_widget_t *lbl = gui_label_create(lr, "Widget Gallery");
    if (lbl) { lbl->fg = GUI_BLUE; lbl->bg = GUI_WINDOW_BG; gui_window_add_widget(win, lbl); }
    
    gui_rect_t c1r = {20, 35, 200, 20};
    gui_widget_t *c1 = gui_checkbox_create(c1r, "Option A", 0);
    if (c1) gui_window_add_widget(win, c1);
    
    gui_rect_t c2r = {20, 60, 200, 20};
    gui_widget_t *c2 = gui_checkbox_create(c2r, "Option B", 1);
    if (c2) gui_window_add_widget(win, c2);
    
    gui_rect_t sr = {20, 95, 300, 24};
    gui_widget_t *sl = gui_slider_create(sr, 0, 100, 50);
    if (sl) gui_window_add_widget(win, sl);
    
    gui_rect_t lbr = {20, 130, 200, 120};
    gui_widget_t *lb = gui_listbox_create(lbr);
    if (lb) {
        gui_listbox_add_item(lb, "Red"); gui_listbox_add_item(lb, "Green");
        gui_listbox_add_item(lb, "Blue"); gui_listbox_add_item(lb, "Cyan");
        gui_listbox_add_item(lb, "Yellow"); gui_listbox_add_item(lb, "Magenta");
        gui_window_add_widget(win, lb);
    }
}

/* ── Color Palette ── */
void gui_app_colors_run(void) {
    gui_window_t *win = gui_window_create("Color Palette", 250, 200, 400, 300, GUI_WHITE);
    if (!win) return;
    gui_add_window(win);
    
    int sw = 30, sh = 20, gap = 4, cols = 6;
    gui_color_t colors[] = {
        GUI_BLACK, GUI_WHITE, GUI_RED, GUI_GREEN, GUI_BLUE, GUI_CYAN,
        GUI_YELLOW, GUI_COLOR(255,128,0), GUI_COLOR(128,0,255), GUI_COLOR(0,255,128),
        GUI_GRAY, GUI_LIGHT_GRAY, GUI_DARK_GRAY,
    };
    int n = sizeof(colors)/sizeof(colors[0]);
    for (int i = 0; i < n; i++) {
        int cx = 20 + (i % cols) * (sw + gap);
        int cy = 20 + (i / cols) * (sh + gap);
        gui_rect_t r = {cx, cy, sw, sh};
        gui_window_draw_rect(win, r, colors[i]);
        gui_window_draw_rect_outline(win, r, GUI_DARK_GRAY, 1);
    }
    gui_window_draw_text(win, 20, 20 + ((n+cols-1)/cols)*(sh+gap) + 5,
                         "GUI Color Palette", GUI_TEXT_FG, GUI_WHITE);
}

/* ── Gradient Demo ── */
void gui_app_gradient_run(void) {
    gui_window_t *win = gui_window_create("Gradients", 50, 50, 400, 350, GUI_WHITE);
    if (!win) return;
    gui_add_window(win);
    
    gui_draw_gradient_v(50, 40, 100, 260, GUI_RED, GUI_BLUE);
    gui_window_draw_text(win, 50, 20, "Vertical (Red→Blue)", GUI_TEXT_FG, GUI_WHITE);
    
    gui_draw_gradient_h(180, 40, 180, 120, GUI_GREEN, GUI_YELLOW);
    gui_window_draw_text(win, 180, 20, "Horizontal (Green→Yellow)", GUI_TEXT_FG, GUI_WHITE);
    
    gui_draw_gradient_v(180, 180, 80, 120, GUI_CYAN, GUI_COLOR(255,0,255));
    gui_draw_gradient_h(270, 180, 90, 120, GUI_COLOR(255,128,0), GUI_COLOR(0,128,255));
}

/* ── Shapes Demo ── */
void gui_app_shapes_run(void) {
    gui_window_t *win = gui_window_create("Shapes", 200, 100, 500, 380, GUI_BLACK);
    if (!win) return;
    gui_add_window(win);
    
    gui_draw_line(50, 30, 150, 80, GUI_RED);
    gui_draw_line(50, 30, 100, 150, GUI_GREEN);
    gui_draw_line(50, 30, 220, 30, GUI_BLUE);
    gui_draw_line(100, 150, 150, 80, GUI_YELLOW);
    
    gui_draw_circle(100, 240, 45, GUI_CYAN);
    gui_draw_circle_filled(250, 90, 35, GUI_COLOR(255,100,100));
    gui_draw_circle(250, 240, 55, GUI_YELLOW);
    
    gui_draw_triangle(400, 30, 350, 100, 480, 90, GUI_GREEN);
    gui_draw_triangle_filled(380, 150, 330, 260, 480, 190, GUI_COLOR(0,100,200));
    
    gui_draw_star(420, 300, 45, 18, GUI_YELLOW);
    
    gui_window_draw_text(win, 10, 10, "Lines, Circles, Triangles, Star", GUI_WHITE, GUI_BLACK);
}

/* ── Checkerboard Demo ── */
void gui_app_checker_run(void) {
    gui_window_t *win = gui_window_create("Checkerboard", 100, 100, 400, 300, GUI_WHITE);
    if (!win) return;
    gui_add_window(win);
    gui_draw_checkerboard(20, 20, 360, 260, 16);
}

/* ── Info Panel ── */
void gui_app_info_run(void) {
    gui_window_t *win = gui_window_create("System Info", 200, 150, 350, 200, GUI_WINDOW_BG);
    if (!win) return;
    gui_add_window(win);
    
    char line[64];
    int y = 30;
    
    gui_window_draw_text(win, 120, 10, "System Information", GUI_BLUE, GUI_WINDOW_BG);
    
    snprintf(line, sizeof(line), "Resolution: 1024x768");
    gui_window_draw_text(win, 20, y, line, GUI_TEXT_FG, GUI_WINDOW_BG); y += 20;
    
    snprintf(line, sizeof(line), "GUI Library: v1.0");
    gui_window_draw_text(win, 20, y, line, GUI_TEXT_FG, GUI_WINDOW_BG); y += 20;
    
    snprintf(line, sizeof(line), "Widget Types: 7");
    gui_window_draw_text(win, 20, y, line, GUI_TEXT_FG, GUI_WINDOW_BG); y += 20;
    
    snprintf(line, sizeof(line), "Drawing Primitives: 12");
    gui_window_draw_text(win, 20, y, line, GUI_TEXT_FG, GUI_WINDOW_BG); y += 20;
    
    snprintf(line, sizeof(line), "Available Colors: 16+");
    gui_window_draw_text(win, 20, y, line, GUI_TEXT_FG, GUI_WINDOW_BG); y += 20;
}
