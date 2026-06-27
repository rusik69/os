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

/* ===== Ellipse (Midpoint) ===== */
void gui_draw_ellipse(int32_t cx, int32_t cy, int32_t rx, int32_t ry, gui_color_t color) {
    int x = 0, y = ry;
    int rx2 = rx * rx, ry2 = ry * ry;
    int p1 = ry2 - rx2 * ry + rx2 / 4;
    while (ry2 * x < rx2 * y) {
        __put(cx + x, cy + y, color); __put(cx - x, cy + y, color);
        __put(cx + x, cy - y, color); __put(cx - x, cy - y, color);
        x++;
        if (p1 < 0) p1 += 2 * ry2 * x + ry2;
        else { y--; p1 += 2 * ry2 * x - 2 * rx2 * y + ry2; }
    }
    int p2 = ry2 * (x + 0.5f) * (x + 0.5f) + rx2 * (y - 1) * (y - 1) - rx2 * ry2;
    while (y >= 0) {
        __put(cx + x, cy + y, color); __put(cx - x, cy + y, color);
        __put(cx + x, cy - y, color); __put(cx - x, cy - y, color);
        y--;
        if (p2 > 0) p2 -= 2 * rx2 * y + rx2;
        else { x++; p2 += 2 * ry2 * x - 2 * rx2 * y + rx2; }
    }
}

void gui_draw_ellipse_filled(int32_t cx, int32_t cy, int32_t rx, int32_t ry, gui_color_t color) {
    for (int y = -ry; y <= ry; y++) {
        int h = (rx * __isqrt(ry * ry - y * y) + ry / 2) / ry;
        for (int x = -h; x <= h; x++) __put(cx + x, cy + y, color);
    }
}

/* ===== Arc (simple angular sweep, 0=right, CCW) ===== */
static int __isin(int deg) {
    while (deg < 0) deg += 360;
    while (deg >= 360) deg -= 360;
    /* Precomputed sin for 0,30,45,60,90... degrees, interpolate */
    int table[13] = {0, 50, 71, 87, 100, 87, 71, 50, 0, -50, -71, -87, -100};
    int idx = (deg * 12 + 180) / 360;
    if (idx < 0) idx = 0;
    if (idx > 12) idx = 12;
    return table[idx];
}
static int __icos(int deg) {
    return __isin(deg + 90);
}

void gui_draw_arc(int32_t cx, int32_t cy, int32_t r, int start_deg, int end_deg, gui_color_t color) {
    if (r <= 0) return;
    while (end_deg < start_deg) end_deg += 360;
    int steps = (end_deg - start_deg) * r / 180;
    if (steps < 1) steps = 1;
    if (steps > 360) steps = 360;
    int prev_x = cx + (r * __icos(start_deg) + 50) / 100;
    int prev_y = cy + (r * __isin(start_deg) + 50) / 100;
    for (int i = 1; i <= steps; i++) {
        int deg = start_deg + (end_deg - start_deg) * i / steps;
        int cur_x = cx + (r * __icos(deg) + 50) / 100;
        int cur_y = cy + (r * __isin(deg) + 50) / 100;
        gui_draw_line(prev_x, prev_y, cur_x, cur_y, color);
        prev_x = cur_x; prev_y = cur_y;
    }
}

void gui_draw_pie(int32_t cx, int32_t cy, int32_t r, int start_deg, int end_deg, gui_color_t color) {
    if (r <= 0) return;
    while (end_deg < start_deg) end_deg += 360;
    int steps = (end_deg - start_deg) * r / 90;
    if (steps < 1) steps = 1;
    if (steps > 360) steps = 360;
    int prev_x = cx, prev_y = cy;
    for (int i = 0; i <= steps; i++) {
        int deg = start_deg + (end_deg - start_deg) * i / steps;
        int cur_x = cx + (r * __icos(deg) + 50) / 100;
        int cur_y = cy + (r * __isin(deg) + 50) / 100;
        if (i == 0) { prev_x = cur_x; prev_y = cur_y; continue; }
        gui_draw_triangle_filled(cx, cy, prev_x, prev_y, cur_x, cur_y, color);
        prev_x = cur_x; prev_y = cur_y;
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

void gui_draw_rounded_rect_filled(gui_rect_t rect, int radius, gui_color_t color) {
    int32_t x = rect.x, y = rect.y, w = (int32_t)rect.w, h = (int32_t)rect.h;
    int r = radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    /* Fill inner rectangle */
    for (int32_t yi = y + r + 1; yi < y + h - r - 1; yi++)
        for (int32_t xi = x; xi < x + w; xi++) __put(xi, yi, color);
    /* Top/bottom center strips */
    for (int32_t xi = x + r; xi <= x + w - r - 1; xi++) {
        for (int32_t yi2 = y; yi2 <= y + r; yi2++) __put(xi, yi2, color);
        for (int32_t yi2 = y + h - r - 1; yi2 < y + h; yi2++) __put(xi, yi2, color);
    }
    /* Left/right strips */
    for (int32_t yi = y + r; yi <= y + h - r - 1; yi++) {
        for (int32_t xi2 = x; xi2 < x + r; xi2++) __put(xi2, yi, color);
        for (int32_t xi2 = x + w - r - 1; xi2 < x + w; xi2++) __put(xi2, yi, color);
    }
    /* Filled corners */
    int cx1 = x + r, cy1 = y + r, cx2 = x + w - 1 - r, cy2 = y + h - 1 - r;
    for (int dy = 0; dy <= r; dy++)
        for (int dx = 0; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) {
                __put(cx1 - dx, cy1 - dy, color); __put(cx2 + dx, cy1 - dy, color);
                __put(cx1 - dx, cy2 + dy, color); __put(cx2 + dx, cy2 + dy, color);
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

void gui_draw_gradient_radial(int32_t cx, int32_t cy, int32_t r,
                               gui_color_t inner, gui_color_t outer) {
    if (r <= 0) return;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int dist = __isqrt(dx * dx + dy * dy);
            if (dist <= r) {
                gui_color_t c = __lerp_color(inner, outer, dist, r);
                __put(cx + dx, cy + dy, c);
            }
        }
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
        int xl = xa < xb ? xa : xb;
        int xr = xa < xb ? xb : xa;
        for (int sx = xl; sx <= xr; sx++) __put(sx, sy, color);
    }
}

/* ===== Bezier ===== */
void gui_draw_bezier_cubic(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                            int32_t x2, int32_t y2, int32_t x3, int32_t y3, gui_color_t color) {
    int steps = 40;
    int prev_x = x0, prev_y = y0;
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps;
        float u = 1.0f - t;
        float uu = u * u, uuu = uu * u;
        float tt = t * t, ttt = tt * t;
        int cur_x = (int)(uuu * (float)x0 + 3.0f * uu * t * (float)x1 + 3.0f * u * tt * (float)x2 + ttt * (float)x3);
        int cur_y = (int)(uuu * (float)y0 + 3.0f * uu * t * (float)y1 + 3.0f * u * tt * (float)y2 + ttt * (float)y3);
        gui_draw_line(prev_x, prev_y, cur_x, cur_y, color);
        prev_x = cur_x; prev_y = cur_y;
    }
}

void gui_draw_bezier_quad(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                           int32_t x2, int32_t y2, gui_color_t color) {
    int steps = 30;
    int prev_x = x0, prev_y = y0;
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps;
        float u = 1.0f - t;
        int cur_x = (int)(u * u * (float)x0 + 2.0f * u * t * (float)x1 + t * t * (float)x2);
        int cur_y = (int)(u * u * (float)y0 + 2.0f * u * t * (float)y1 + t * t * (float)y2);
        gui_draw_line(prev_x, prev_y, cur_x, cur_y, color);
        prev_x = cur_x; prev_y = cur_y;
    }
}

/* ===== Polyline / Polygon ===== */
void gui_draw_polyline(const gui_point_t *pts, int n, gui_color_t color) {
    if (!pts || n < 2) return;
    for (int i = 0; i < n - 1; i++)
        gui_draw_line(pts[i].x, pts[i].y, pts[i+1].x, pts[i+1].y, color);
}

void gui_draw_polygon(const gui_point_t *pts, int n, gui_color_t color) {
    if (!pts || n < 2) return;
    gui_draw_polyline(pts, n, color);
    gui_draw_line(pts[n-1].x, pts[n-1].y, pts[0].x, pts[0].y, color);
}

void gui_draw_polygon_filled(const gui_point_t *pts, int n, gui_color_t color) {
    if (!pts || n < 3) return;
    int min_y = pts[0].y, max_y = pts[0].y;
    for (int i = 1; i < n; i++) { if (pts[i].y < min_y) min_y = pts[i].y; if (pts[i].y > max_y) max_y = pts[i].y; }
    for (int sy = min_y; sy <= max_y; sy++) {
        int xs[32], xn = 0;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            int yi = pts[i].y, yj = pts[j].y;
            if ((yi <= sy && yj > sy) || (yj <= sy && yi > sy)) {
                xs[xn++] = pts[i].x + (sy - yi) * (pts[j].x - pts[i].x) / (yj - yi);
                if (xn >= 32) break;
            }
        }
        for (int i = 0; i < xn - 1; i++)
            for (int j = 0; j < xn - 1 - i; j++)
                if (xs[j] > xs[j+1]) { int t = xs[j]; xs[j] = xs[j+1]; xs[j+1] = t; }
        for (int i = 0; i + 1 < xn; i += 2)
            for (int sx = xs[i]; sx <= xs[i+1]; sx++) __put(sx, sy, color);
    }
}

/* ===== Dashed line ===== */
void gui_draw_dashed_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                           gui_color_t color, int dash_len, int gap_len) {
    int dx = x2 - x1, dy = y2 - y1;
    int steps = __max(__abs(dx), __abs(dy));
    if (steps == 0) { __put(x1, y1, color); return; }
    int drawn = 0, segment = 0;
    for (int i = 0; i <= steps; i++) {
        int xi = x1 + dx * i / steps;
        int yi = y1 + dy * i / steps;
        if (segment < dash_len) __put(xi, yi, color);
        drawn++;
        if (segment < dash_len && drawn >= dash_len) { segment++; drawn = 0; }
        else if (segment >= dash_len && drawn >= gap_len) { segment = 0; drawn = 0; }
    }
}

/* ===== Thick line ===== */
void gui_draw_thick_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                          int thickness, gui_color_t color) {
    if (thickness <= 1) { gui_draw_line(x1, y1, x2, y2, color); return; }
    int dx = __abs(x2 - x1), dy = -__abs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1;
    int err = dx + dy, e2;
    int hx = x1, hy = y1;
    while (1) {
        gui_draw_circle_filled(hx, hy, thickness / 2, color);
        if (hx == x2 && hy == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; hx += sx; }
        if (e2 <= dx) { err += dx; hy += sy; }
    }
}

/* ===== Arrow line ===== */
void gui_draw_arrow_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                          gui_color_t color, int arrow_size) {
    gui_draw_line(x1, y1, x2, y2, color);
    int dx = x2 - x1, dy = y2 - y1;
    int len = __isqrt(dx * dx + dy * dy);
    if (len == 0) return;
    /* Arrowhead: two lines from tip going back */
    int ax1 = x2 - arrow_size * (dx * 7 + dy * 4) / (len * 10);
    int ay1 = y2 - arrow_size * (dy * 7 - dx * 4) / (len * 10);
    int ax2 = x2 - arrow_size * (dx * 7 - dy * 4) / (len * 10);
    int ay2 = y2 - arrow_size * (dy * 7 + dx * 4) / (len * 10);
    gui_draw_line(x2, y2, ax1, ay1, color);
    gui_draw_line(x2, y2, ax2, ay2, color);
}

void gui_draw_arrow(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int head_w, int head_h, gui_color_t color) {
    (void)head_h;
    gui_draw_arrow_line(x1, y1, x2, y2, color, head_w);
}

/* ===== 3D Frame ===== */
void gui_draw_frame_3d(gui_rect_t rect, int raised, int thickness) {
    gui_color_t light = GUI_COLOR(255,255,255);
    gui_color_t dark = GUI_COLOR(128,128,128);
    for (int t = 0; t < thickness && t < (int)__min(rect.w, rect.h) / 2; t++) {
        gui_rect_t tr = {rect.x + t, rect.y + t, rect.w - t * 2, rect.h - t * 2};
        gui_window_draw_rect_outline(NULL, tr, raised ? light : dark, 1);
    }
    int t2 = thickness;
    gui_rect_t tr = {rect.x + t2 - 1, rect.y + t2 - 1, rect.w - (t2 - 1) * 2, rect.h - (t2 - 1) * 2};
    gui_window_draw_rect_outline(NULL, tr, raised ? dark : light, 1);
}

/* ===== Dashed rect ===== */
void gui_draw_rect_dashed(gui_rect_t rect, gui_color_t color, int dash_len) {
    int32_t x = rect.x, y = rect.y, w = (int32_t)rect.w, h = (int32_t)rect.h;
    gui_draw_dashed_line(x, y, x + w - 1, y, color, dash_len, dash_len);
    gui_draw_dashed_line(x + w - 1, y, x + w - 1, y + h - 1, color, dash_len, dash_len);
    gui_draw_dashed_line(x + w - 1, y + h - 1, x, y + h - 1, color, dash_len, dash_len);
    gui_draw_dashed_line(x, y + h - 1, x, y, color, dash_len, dash_len);
}

/* ===== Grid ===== */
void gui_draw_grid(int32_t x, int32_t y, uint32_t w, uint32_t h,
                   int cell_w, int cell_h, gui_color_t color) {
    if (cell_w <= 0 || cell_h <= 0) return;
    for (int32_t gx = x; gx <= (int32_t)(x + w); gx += cell_w)
        gui_draw_line(gx, y, gx, (int32_t)(y + h - 1), color);
    for (int32_t gy = y; gy <= (int32_t)(y + h); gy += cell_h)
        gui_draw_line(x, gy, (int32_t)(x + w - 1), gy, color);
}

/* ===== Cross ===== */
void gui_draw_cross(int32_t cx, int32_t cy, int size, gui_color_t color) {
    gui_draw_line(cx - size, cy, cx + size, cy, color);
    gui_draw_line(cx, cy - size, cx, cy + size, color);

}

/* ===== Shadow ===== */
void gui_draw_rect_shadow(gui_rect_t rect, int offset, gui_color_t shadow_color) {
    /* Draw shadow to bottom-right */
    gui_rect_t sh = {rect.x + offset, rect.y + offset, rect.w, rect.h};
    gui_window_draw_rect(NULL, sh, shadow_color);
}

/* ===== Heart ===== */
void gui_draw_heart(int32_t cx, int32_t cy, int size, gui_color_t color) {
    /* Simple heart: two circles + triangle */
    int r = size / 4;
    if (r < 2) r = 2;
    int dx = size / 6;
    int bx1 = cx - dx, bx2 = cx + dx, by = cy - r / 2;
    gui_draw_circle_filled(bx1, by, r, color);
    gui_draw_circle_filled(bx2, by, r, color);
    int tip_y = cy + size / 2;
    gui_draw_triangle_filled(bx1 - r, by + r / 2, bx2 + r, by + r / 2, cx, tip_y, color);
}

/* ===== Donut ===== */
void gui_draw_donut(int32_t cx, int32_t cy, int32_t r_outer, int32_t r_inner, gui_color_t color) {
    gui_draw_circle(cx, cy, r_outer, color);
    gui_draw_circle(cx, cy, r_inner, color);
}

/* ===== Diamond ===== */
void gui_draw_diamond(int32_t cx, int32_t cy, int32_t rx, int32_t ry, gui_color_t color) {
    gui_point_t pts[4] = {
        {cx, cy - ry}, {cx + rx, cy}, {cx, cy + ry}, {cx - rx, cy}
    };
    gui_draw_polygon(pts, 4, color);
}

/* ===== Crosshair ===== */
void gui_draw_crosshair(int32_t cx, int32_t cy, int r, gui_color_t color) {
    gui_draw_circle(cx, cy, r, color);
    gui_draw_cross(cx, cy, r + 3, color);
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
    int pts[10][2];
    for (int i = 0; i < 5; i++) {
        int si1 = 0, co1 = 1, si2 = 0, co2 = 0;
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
    for (int i = 0; i < 5; i++) {
        int p1 = i, p2 = i + 5, p3 = (i + 1) % 5;
        gui_draw_line(pts[p1][0], pts[p1][1], pts[p2][0], pts[p2][1], color);
        gui_draw_line(pts[p2][0], pts[p2][1], pts[p3][0], pts[p3][1], color);
    }
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

/* ===== Color Utilities ===== */
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
gui_color_t gui_color_invert(gui_color_t c) {
    return GUI_COLOR(255 - ((c >> 16) & 0xFF), 255 - ((c >> 8) & 0xFF), 255 - (c & 0xFF));
}
gui_color_t gui_color_blend(gui_color_t fg, gui_color_t bg, int alpha) {
    if (alpha >= 255) return fg;
    if (alpha <= 0) return bg;
    int fr = (fg >> 16) & 0xFF, fg2 = (fg >> 8) & 0xFF, fb = fg & 0xFF;
    int br = (bg >> 16) & 0xFF, bg2 = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    int a = alpha, inv = 255 - a;
    return GUI_COLOR((fr * a + br * inv) / 255, (fg2 * a + bg2 * inv) / 255, (fb * a + bb * inv) / 255);
}
gui_color_t gui_color_to_greyscale(gui_color_t c) {
    int v = ((c >> 16) & 0xFF) * 77 + ((c >> 8) & 0xFF) * 150 + (c & 0xFF) * 29;
    v /= 256;
    return GUI_COLOR(v, v, v);
}
gui_color_t gui_color_random(void) {
    static int seed = 42;
    seed = seed * 1103515245 + 12345;
    int r = (seed >> 16) & 0xFF;
    int g = (seed >> 8) & 0xFF;
    int b = seed & 0xFF;
    return GUI_COLOR(r, g, b);
}
void gui_color_get_rgba(gui_color_t c, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
    if (r) *r = (c >> 16) & 0xFF;
    if (g) *g = (c >> 8) & 0xFF;
    if (b) *b = c & 0xFF;
    if (a) *a = (c >> 24) & 0xFF;
}
gui_color_t gui_color_from_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
int gui_color_is_dark(gui_color_t c) {
    int lum = ((c >> 16) & 0xFF) * 77 + ((c >> 8) & 0xFF) * 150 + (c & 0xFF) * 29;
    return lum < 128 * 256;
}
int gui_color_is_light(gui_color_t c) { return !gui_color_is_dark(c); }
uint32_t gui_color_distance(gui_color_t a, gui_color_t b) {
    int dr = (int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF);
    int dg = (int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF);
    int db = (int)(a & 0xFF) - (int)(b & 0xFF);
    return (uint32_t)(dr * dr + dg * dg + db * db);
}
void gui_color_to_hsv(gui_color_t c, int *h, int *s, int *v) {
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    int mx = __max(__max(r, g), b), mn = __min(__min(r, g), b);
    if (v) *v = mx;
    if (s) *s = mx ? (mx - mn) * 255 / mx : 0;
    if (h) {
        if (mx == mn) *h = 0;
        else {
            int d = mx - mn;
            if (mx == r) *h = 60 * ((g - b) * 255 / d / 255);
            else if (mx == g) *h = 60 * (2 + (b - r) * 255 / d / 255);
            else *h = 60 * (4 + (r - g) * 255 / d / 255);
            if (*h < 0) *h += 360;
        }
    }
}
gui_color_t gui_color_from_hsv(int h, int s, int v) {
    while (h < 0)
        h += 360;
    while (h >= 360)
        h -= 360;
    if (s > 255)
        s = 255;
    if (v > 255)
        v = 255;
    int region = h / 60, fpart = (h % 60) * 255 / 60;
    int p = v * (255 - s) / 255;
    int q = v * (255 - s * fpart / 255) / 255;
    int t = v * (255 - s * (255 - fpart) / 255) / 255;
    switch (region) {
        case 0: return GUI_COLOR(v, t, p);
        case 1: return GUI_COLOR(q, v, p);
        case 2: return GUI_COLOR(p, v, t);
        case 3: return GUI_COLOR(p, q, v);
        case 4: return GUI_COLOR(t, p, v);
        default: return GUI_COLOR(v, p, q);
    }
}
gui_color_t gui_color_contrast(gui_color_t c) {
    return gui_color_is_dark(c) ? GUI_WHITE : GUI_BLACK;
}

/* ===== Checkbox widget (moved here from old location) ===== */
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

/* ── Spinbox ── */
static void spinbox_draw(gui_widget_t *w) {
    gui_spinbox_data_t *d = (gui_spinbox_data_t *)w->data;
    char buf[16]; snprintf(buf, sizeof(buf), "%d", d->value);
    gui_window_draw_rect(NULL, w->rect, GUI_WHITE);
    gui_window_draw_rect_outline(NULL, w->rect, GUI_GRAY, 1);
    int bw = (int)w->rect.w, bh = (int)w->rect.h;
    gui_rect_t up = {w->rect.x + bw - 20, w->rect.y, 20, bh / 2};
    gui_rect_t dn = {w->rect.x + bw - 20, w->rect.y + bh / 2, 20, bh - bh / 2};
    gui_window_draw_rect(NULL, up, GUI_BUTTON_BG); gui_window_draw_rect_outline(NULL, up, GUI_GRAY, 1);
    gui_window_draw_rect(NULL, dn, GUI_BUTTON_BG); gui_window_draw_rect_outline(NULL, dn, GUI_GRAY, 1);
    gui_window_draw_text(NULL, up.x + 6, up.y + 2, "+", GUI_TEXT_FG, GUI_BUTTON_BG);
    gui_window_draw_text(NULL, dn.x + 7, dn.y + 2, "-", GUI_TEXT_FG, GUI_BUTTON_BG);
    gui_window_draw_text(NULL, w->rect.x + 4, w->rect.y + 2, buf, GUI_TEXT_FG, GUI_WHITE);
}

static void spinbox_event(gui_widget_t *w, gui_event_t *evt) {
    gui_spinbox_data_t *d = (gui_spinbox_data_t *)w->data;
    if (evt->type == GUI_EVENT_MOUSE_DOWN && evt->button == 1) {
        int bw = (int)w->rect.w;
        if (evt->x >= w->rect.x + bw - 20) {
            if (evt->y < w->rect.y + (int)w->rect.h / 2) { d->value += d->step; if (d->value > d->max_val) d->value = d->max_val; }
            else { d->value -= d->step; if (d->value < d->min_val) d->value = d->min_val; }
            if (d->on_change) d->on_change(w, d->value);
        }
    }
}

static void spinbox_destroy(gui_widget_t *w) { if (w->data) kfree(w->data); }

gui_widget_t* gui_spinbox_create(gui_rect_t rect, int min_val, int max_val, int initial, int step) {
    gui_widget_t *w = gui_widget_create(rect);
    if (!w) return NULL;
    gui_spinbox_data_t *d = kmalloc(sizeof(gui_spinbox_data_t));
    if (!d) { kfree(w); return NULL; }
    memset(d, 0, sizeof(gui_spinbox_data_t));
    d->min_val = min_val; d->max_val = max_val; d->value = initial; d->step = step;
    w->data = d; w->draw = spinbox_draw; w->on_event = spinbox_event; w->destroy = spinbox_destroy;
    return w;
}
int gui_spinbox_get_value(gui_widget_t *sp) { gui_spinbox_data_t *d = (gui_spinbox_data_t *)sp->data; return d ? d->value : 0; }
void gui_spinbox_set_value(gui_widget_t *sp, int val) { gui_spinbox_data_t *d = (gui_spinbox_data_t *)sp->data; if (d) d->value = val; }
void gui_spinbox_set_on_change(gui_widget_t *sp, void (*fn)(gui_widget_t *, int)) { gui_spinbox_data_t *d = (gui_spinbox_data_t *)sp->data; if (d) d->on_change = fn; }

/* ── Toggle ── */
static void toggle_draw(gui_widget_t *w) {
    gui_toggle_data_t *d = (gui_toggle_data_t *)w->data;
    int ww = (int)w->rect.w, hh = (int)w->rect.h;
    int r = hh / 2;
    gui_color_t bg = d->on ? GUI_COLOR(0, 180, 80) : GUI_GRAY;
    gui_draw_rounded_rect_filled(w->rect, r, bg);
    gui_draw_rounded_rect(w->rect, r, GUI_DARK_GRAY);
    int knob_x = d->on ? w->rect.x + ww - hh : w->rect.x;
    gui_draw_circle_filled(knob_x + r, w->rect.y + r, r - 2, GUI_WHITE);
    gui_draw_circle(knob_x + r, w->rect.y + r, r - 2, GUI_DARK_GRAY);
}
static void toggle_event(gui_widget_t *w, gui_event_t *evt) {
    gui_toggle_data_t *d = (gui_toggle_data_t *)w->data;
    if (evt->type == GUI_EVENT_MOUSE_DOWN && evt->button == 1) {
        d->on = !d->on;
        if (d->on_change) d->on_change(w, d->on);
    }
}
static void toggle_destroy(gui_widget_t *w) { if (w->data) kfree(w->data); }
gui_widget_t* gui_toggle_create(gui_rect_t rect, int initial) {
    gui_widget_t *w = gui_widget_create(rect);
    if (!w)
        return NULL;
    gui_toggle_data_t *d = kmalloc(sizeof(gui_toggle_data_t));
    if (!d) {
        kfree(w);
        return NULL;
    }
    memset(d, 0, sizeof(gui_toggle_data_t));
    d->on = initial;
    w->data = d; w->draw = toggle_draw; w->on_event = toggle_event; w->destroy = toggle_destroy;
    return w;
}
int gui_toggle_is_on(gui_widget_t *tg) { gui_toggle_data_t *d = (gui_toggle_data_t *)tg->data; return d ? d->on : 0; }
void gui_toggle_set_on(gui_widget_t *tg, int on) { gui_toggle_data_t *d = (gui_toggle_data_t *)tg->data; if (d) d->on = on; }
void gui_toggle_set_on_change(gui_widget_t *tg, void (*fn)(gui_widget_t *, int)) { gui_toggle_data_t *d = (gui_toggle_data_t *)tg->data; if (d) d->on_change = fn; }

/* ── Dropdown ── */
static void dropdown_draw(gui_widget_t *w) {
    gui_dropdown_data_t *d = (gui_dropdown_data_t *)w->data;
    gui_color_t bg = d->open ? GUI_COLOR(230, 240, 255) : GUI_WHITE;
    gui_window_draw_rect(NULL, w->rect, bg);
    gui_window_draw_rect_outline(NULL, w->rect, GUI_GRAY, 1);
    gui_window_draw_text(NULL, w->rect.x + 4, w->rect.y + 2, d->open ? "v" : ">", GUI_TEXT_FG, bg);
    if (d->selected >= 0 && d->selected < d->num_items)
        gui_window_draw_text(NULL, w->rect.x + 16, w->rect.y + 2, d->items[d->selected], GUI_TEXT_FG, bg);
    if (d->open) {
        int item_h = 16, hh = item_h * d->num_items + 2;
        gui_rect_t dr = {w->rect.x, w->rect.y + (int32_t)w->rect.h, w->rect.w, (uint32_t)hh};
        gui_window_draw_rect(NULL, dr, GUI_WHITE);
        gui_window_draw_rect_outline(NULL, dr, GUI_GRAY, 1);
        for (int i = 0; i < d->num_items; i++) {
            gui_rect_t ir = {dr.x + 1, dr.y + 1 + i * item_h, dr.w - 2, item_h};
            if (i == d->selected) { gui_window_draw_rect(NULL, ir, GUI_TITLE_BG); gui_window_draw_text(NULL, ir.x + 4, ir.y + 2, d->items[i], GUI_WHITE, GUI_TITLE_BG); }
            else gui_window_draw_text(NULL, ir.x + 4, ir.y + 2, d->items[i], GUI_TEXT_FG, GUI_WHITE);
        }
    }
}
static void dropdown_event(gui_widget_t *w, gui_event_t *evt) {
    gui_dropdown_data_t *d = (gui_dropdown_data_t *)w->data;
    if (evt->type == GUI_EVENT_MOUSE_DOWN && evt->button == 1) {
        if (!d->open) { d->open = 1; return; }
        int item_h = 16;
        int rel_y = evt->y - (int)(w->rect.y + w->rect.h);
        int idx = rel_y / item_h;
        if (idx >= 0 && idx < d->num_items) { d->selected = idx; d->open = 0; if (d->on_select) d->on_select(w, idx, d->items[idx]); }
        else d->open = 0;
    }
}
static void dropdown_destroy(gui_widget_t *w) { if (w->data) kfree(w->data); }
gui_widget_t* gui_dropdown_create(gui_rect_t rect) {
    gui_widget_t *w = gui_widget_create(rect); if (!w) return NULL;
    gui_dropdown_data_t *d = kmalloc(sizeof(gui_dropdown_data_t)); if (!d) { kfree(w); return NULL; }
    memset(d, 0, sizeof(gui_dropdown_data_t)); d->selected = -1;
    w->data = d; w->draw = dropdown_draw; w->on_event = dropdown_event; w->destroy = dropdown_destroy; return w;
}
void gui_dropdown_add_item(gui_widget_t *dd, const char *item) {
    gui_dropdown_data_t *d = (gui_dropdown_data_t *)dd->data; if (!d || d->num_items >= 32) return;
    strncpy(d->items[d->num_items], item, 63); d->items[d->num_items][63] = '\0'; d->num_items++;
}
void gui_dropdown_clear(gui_widget_t *dd) { gui_dropdown_data_t *d = (gui_dropdown_data_t *)dd->data; if (d) { memset(d->items, 0, sizeof(d->items)); d->num_items = 0; d->selected = -1; d->open = 0; } }
int gui_dropdown_get_selected(gui_widget_t *dd) { gui_dropdown_data_t *d = (gui_dropdown_data_t *)dd->data; return d ? d->selected : -1; }
const char* gui_dropdown_get_selected_text(gui_widget_t *dd) { gui_dropdown_data_t *d = (gui_dropdown_data_t *)dd->data; if (!d || d->selected < 0) return ""; return d->items[d->selected]; }
void gui_dropdown_set_on_select(gui_widget_t *dd, void (*fn)(gui_widget_t *, int, const char *)) { gui_dropdown_data_t *d = (gui_dropdown_data_t *)dd->data; if (d) d->on_select = fn; }

/* ── Scrollbar ── */
static void scrollbar_draw(gui_widget_t *w) {
    gui_scrollbar_data_t *d = (gui_scrollbar_data_t *)w->data;
    gui_window_draw_rect(NULL, w->rect, GUI_LIGHT_GRAY);
    gui_window_draw_rect_outline(NULL, w->rect, GUI_GRAY, 1);
    int hh = (int)w->rect.h;
    if (d->content_size <= d->view_size || d->content_size <= 0) return;
    int thumb_h = __max(12, hh * d->view_size / d->content_size);
    int max_pos = d->content_size - d->view_size;
    int thumb_y = max_pos > 0 ? (hh - thumb_h) * d->scroll_pos / max_pos : 0;
    gui_rect_t tr = {w->rect.x + 2, w->rect.y + thumb_y, w->rect.w - 4, thumb_h};
    gui_window_draw_rect(NULL, tr, GUI_BUTTON_BG);
    gui_window_draw_rect_outline(NULL, tr, GUI_DARK_GRAY, 1);
}
static void scrollbar_event(gui_widget_t *w, gui_event_t *evt) {
    gui_scrollbar_data_t *d = (gui_scrollbar_data_t *)w->data;
    if (evt->type == GUI_EVENT_MOUSE_DOWN && evt->button == 1) {
        int hh = (int)w->rect.h;
        if (d->content_size <= d->view_size) return;
        int thumb_h = __max(12, hh * d->view_size / d->content_size);
        int max_pos = d->content_size - d->view_size;
        int rel_y = evt->y - w->rect.y - thumb_h / 2;
        if (rel_y < 0) rel_y = 0;
        if (rel_y > hh - thumb_h) rel_y = hh - thumb_h;
        d->scroll_pos = max_pos > 0 ? rel_y * max_pos / (hh - thumb_h) : 0;
        if (d->on_scroll) d->on_scroll(w, d->scroll_pos);
    }
}
static void scrollbar_destroy(gui_widget_t *w) { if (w->data) kfree(w->data); }
gui_widget_t* gui_scrollbar_create(gui_rect_t rect, int content_size, int view_size) {
    gui_widget_t *w = gui_widget_create(rect); if (!w) return NULL;
    gui_scrollbar_data_t *d = kmalloc(sizeof(gui_scrollbar_data_t)); if (!d) { kfree(w); return NULL; }
    memset(d, 0, sizeof(gui_scrollbar_data_t)); d->content_size = content_size; d->view_size = view_size;
    w->data = d; w->draw = scrollbar_draw; w->on_event = scrollbar_event; w->destroy = scrollbar_destroy; return w;
}
void gui_scrollbar_set_range(gui_widget_t *sb, int cs, int vs) { gui_scrollbar_data_t *d = (gui_scrollbar_data_t *)sb->data; if (d) { d->content_size = cs; d->view_size = vs; } }
int gui_scrollbar_get_pos(gui_widget_t *sb) { gui_scrollbar_data_t *d = (gui_scrollbar_data_t *)sb->data; return d ? d->scroll_pos : 0; }
void gui_scrollbar_set_pos(gui_widget_t *sb, int pos) { gui_scrollbar_data_t *d = (gui_scrollbar_data_t *)sb->data; if (d) d->scroll_pos = pos; }
void gui_scrollbar_set_on_scroll(gui_widget_t *sb, void (*fn)(gui_widget_t *, int)) { gui_scrollbar_data_t *d = (gui_scrollbar_data_t *)sb->data; if (d) d->on_scroll = fn; }

/* ── Progress widget ── */
static void progress_widget_draw(gui_widget_t *w) {
    gui_progress_data_t *d = (gui_progress_data_t *)w->data;
    gui_draw_progress_bar(w->rect.x, w->rect.y, w->rect.w, w->rect.h, d->percent, d->fg, d->bg);
}
gui_widget_t* gui_progress_create(gui_rect_t rect, gui_color_t fg, gui_color_t bg) {
    gui_widget_t *w = gui_widget_create(rect); if (!w) return NULL;
    gui_progress_data_t *d = kmalloc(sizeof(gui_progress_data_t)); if (!d) { kfree(w); return NULL; }
    memset(d, 0, sizeof(gui_progress_data_t)); d->fg = fg; d->bg = bg;
    w->data = d; w->draw = progress_widget_draw; w->destroy = gui_widget_default_destroy; return w;
}
void gui_progress_set_percent(gui_widget_t *pw, int pct) { gui_progress_data_t *d = (gui_progress_data_t *)pw->data; if (d) d->percent = pct; }

/* ── Radio button ── */
static void radio_draw(gui_widget_t *w) {
    gui_radio_data_t *d = (gui_radio_data_t *)w->data;
    int cx = w->rect.x + 8, cy = w->rect.y + (int)w->rect.h / 2;
    gui_draw_circle(cx, cy, 7, GUI_DARK_GRAY);
    gui_draw_circle_filled(cx, cy, 6, GUI_WHITE);
    if (d->selected) gui_draw_circle_filled(cx, cy, 3, GUI_TITLE_BG);
    gui_window_draw_text(NULL, w->rect.x + 18, w->rect.y + 2, d->label, GUI_TEXT_FG, w->bg);
}
static void radio_event(gui_widget_t *w, gui_event_t *evt) {
    gui_radio_data_t *d = (gui_radio_data_t *)w->data;
    if (evt->type == GUI_EVENT_MOUSE_DOWN && evt->button == 1) {
        if (d->group) {
            for (int i = 0; i < d->group->count; i++) {
                gui_radio_data_t *od = (gui_radio_data_t *)d->group->buttons[i]->data;
                if (od) od->selected = 0;
            }
        }
        d->selected = 1;
        if (d->on_select) d->on_select(w);
    }
}
static void radio_destroy(gui_widget_t *w) { if (w->data) kfree(w->data); }
gui_widget_t* gui_radio_create(gui_rect_t rect, const char *label, gui_radiogroup_t *grp) {
    gui_widget_t *w = gui_widget_create(rect); if (!w) return NULL;
    gui_radio_data_t *d = kmalloc(sizeof(gui_radio_data_t)); if (!d) { kfree(w); return NULL; }
    memset(d, 0, sizeof(gui_radio_data_t)); if (label) strncpy(d->label, label, sizeof(d->label) - 1);
    d->group = grp; if (grp) gui_radiogroup_add(grp, w);
    w->data = d; w->draw = radio_draw; w->on_event = radio_event; w->destroy = radio_destroy; return w;
}
gui_radiogroup_t* gui_radiogroup_create(void) {
    gui_radiogroup_t *grp = kmalloc(sizeof(gui_radiogroup_t)); if (!grp) return NULL;
    memset(grp, 0, sizeof(gui_radiogroup_t)); grp->selected = -1; return grp;
}
void gui_radiogroup_add(gui_radiogroup_t *grp, gui_widget_t *rb) { if (!grp || grp->count >= 16) return; grp->buttons[grp->count++] = rb; }
int gui_radio_is_selected(gui_widget_t *rb) { gui_radio_data_t *d = (gui_radio_data_t *)rb->data; return d ? d->selected : 0; }

/* ── Separator ── */
static void separator_draw(gui_widget_t *w) {
    int h = (int)w->rect.h, ctr_x = w->rect.x + (int)w->rect.w / 2, ctr_y = w->rect.y + h / 2;
    if (w->rect.w >= w->rect.h) gui_draw_line(w->rect.x, ctr_y, w->rect.x + (int32_t)w->rect.w - 1, ctr_y, GUI_GRAY);
    else gui_draw_line(ctr_x, w->rect.y, ctr_x, w->rect.y + h - 1, GUI_GRAY);
}
gui_widget_t* gui_separator_create(gui_rect_t rect, int horizontal) {
    (void)horizontal; gui_widget_t *w = gui_widget_create(rect); if (!w) return NULL;
    w->draw = separator_draw; w->destroy = gui_widget_default_destroy; return w;
}

/* ── Panel ── */
static void panel_draw(gui_widget_t *w) {
    gui_window_draw_rect(NULL, w->rect, w->bg);
    gui_window_draw_rect_outline(NULL, w->rect, GUI_GRAY, 1);
}
gui_widget_t* gui_panel_create(gui_rect_t rect, gui_color_t bg) {
    gui_widget_t *w = gui_widget_create(rect); if (!w) return NULL;
    w->bg = bg; w->draw = panel_draw; w->destroy = gui_widget_default_destroy; return w;
}

/* ── Groupbox ── */
static void groupbox_draw(gui_widget_t *w) {
    gui_groupbox_data_t *d = (gui_groupbox_data_t *)w->data;
    gui_window_draw_rect(NULL, w->rect, d->bg);
    gui_rect_t or = {w->rect.x, w->rect.y, w->rect.w, w->rect.h};
    gui_window_draw_rect_outline(NULL, or, GUI_GRAY, 1);
    if (d->title[0]) {
        gui_window_draw_rect(NULL, (gui_rect_t){w->rect.x + 8, w->rect.y - 6, (uint32_t)(strlen(d->title) * 8 + 6), 12}, d->bg);
        gui_window_draw_text(NULL, w->rect.x + 12, w->rect.y - 8, d->title, GUI_DARK_GRAY, d->bg);
    }
}
static void groupbox_destroy(gui_widget_t *w) { if (w->data) kfree(w->data); }
gui_widget_t* gui_groupbox_create(gui_rect_t rect, const char *title, gui_color_t bg) {
    gui_widget_t *w = gui_widget_create(rect); if (!w) return NULL;
    gui_groupbox_data_t *d = kmalloc(sizeof(gui_groupbox_data_t)); if (!d) { kfree(w); return NULL; }
    memset(d, 0, sizeof(gui_groupbox_data_t)); d->bg = bg; if (title) strncpy(d->title, title, sizeof(d->title) - 1);
    w->bg = bg; w->data = d; w->draw = groupbox_draw; w->destroy = groupbox_destroy; return w;
}

/* ===================================================================
 * D115: New Drawing Primitives
 * =================================================================== */

/* Multi-stop gradient */
void gui_draw_gradient_multi(int32_t x, int32_t y, uint32_t w, uint32_t h,
                              const gui_gradient_stop_t *stops, int n_stops, int vertical) {
    if (!stops || n_stops < 2) return;
    int max_dim = vertical ? (int)h : (int)w;
    for (int pos = 0; pos < max_dim; pos++) {
        float t = (float)pos / (float)max_dim;
        int si = 0;
        while (si < n_stops - 1 && stops[si + 1].position < t * 100) si++;
        if (si >= n_stops - 1) si = n_stops - 2;
        float lt = (t * 100.0f - (float)stops[si].position) / (float)(stops[si+1].position - stops[si].position + 1);
        if (lt < 0) lt = 0;
        if (lt > 1) lt = 1;
        gui_color_t c = gui_color_lerp(stops[si].color, stops[si+1].color, (int)(lt * 255), 255);
        if (vertical) {
            for (uint32_t xi = 0; xi < w; xi++) __put(x + (int32_t)xi, y + pos, c);
        } else {
            for (uint32_t yi = 0; yi < h; yi++) __put(x + pos, y + (int32_t)yi, c);
        }
    }
}

/* Pattern fill */
void gui_draw_pattern_fill(int32_t x, int32_t y, uint32_t w, uint32_t h,
                            const gui_color_t *pattern, int pw, int ph) {
    if (!pattern || pw <= 0 || ph <= 0) return;
    for (uint32_t yi = 0; yi < h; yi++)
        for (uint32_t xi = 0; xi < w; xi++)
            __put(x + (int32_t)xi, y + (int32_t)yi, pattern[(yi % ph) * pw + (xi % pw)]);
}

/* Dashed circle */
void gui_draw_circle_dashed(int32_t cx, int32_t cy, int32_t r, gui_color_t color, int dash_len) {
    int x = 0, y = r, d = 1 - r, draw = 1, cnt = 0;
    while (x <= y) {
        if (draw) {
            __put(cx + x, cy + y, color); __put(cx - x, cy + y, color);
            __put(cx + x, cy - y, color); __put(cx - x, cy - y, color);
            __put(cx + y, cy + x, color); __put(cx - y, cy + x, color);
            __put(cx + y, cy - x, color); __put(cx - y, cy - x, color);
        }
        cnt++; if (cnt >= dash_len) { draw = !draw; cnt = 0; }
        x++; if (d < 0) d += 2 * x + 1; else { y--; d += 2 * (x - y) + 1; }
    }
}

/* Sparkle */
void gui_draw_sparkle(int32_t cx, int32_t cy, int size, gui_color_t color) {
    for (int i = 0; i < 8; i++) {
        int ang = i * 45;
        int ex = cx + (size * __icos(ang) + 50) / 100;
        int ey = cy + (size * __isin(ang) + 50) / 100;
        gui_draw_line(cx, cy, ex, ey, color);
    }
    gui_draw_circle_filled(cx, cy, size / 4, color);
}

/* Gauge */
void gui_draw_gauge(int32_t cx, int32_t cy, int r, int value, int min_val, int max_val,
                     gui_color_t face, gui_color_t needle, gui_color_t tick) {
    gui_draw_circle(cx, cy, r, tick);
    gui_draw_circle_filled(cx, cy, r - 3, face);
    for (int i = 0; i < 12; i++) {
        int ang = i * 30 - 120;
        int x1 = cx + (r - 8) * __icos(ang) / 100;
        int y1 = cy + (r - 8) * __isin(ang) / 100;
        int x2 = cx + (r - 3) * __icos(ang) / 100;
        int y2 = cy + (r - 3) * __isin(ang) / 100;
        gui_draw_line(x1, y1, x2, y2, tick);
    }
    int range = max_val - min_val;
    int val_ang = -120 + (range > 0 ? (value - min_val) * 240 / range : 0);
    int nx = cx + (r - 15) * __icos(val_ang) / 100;
    int ny = cy + (r - 15) * __isin(val_ang) / 100;
    gui_draw_thick_line(cx, cy, nx, ny, 3, needle);
    gui_draw_circle_filled(cx, cy, 4, needle);
}

/* LED */
void gui_draw_led(int32_t cx, int32_t cy, int r, int on, gui_color_t color) {
    gui_color_t off = gui_color_darken(color, 150);
    gui_draw_circle_filled(cx, cy, r, on ? color : off);
    gui_draw_circle(cx, cy, r, GUI_DARK_GRAY);
    if (on) {
        gui_draw_circle_filled(cx - r/3, cy - r/3, r/3, gui_color_lighten(color, 80));
    }
}

/* Drop shadow text */
void gui_draw_text_shadow(int32_t x, int32_t y, const char *text,
                           gui_color_t fg, gui_color_t shadow, int offset) {
    gui_window_draw_text(NULL, x + offset, y + offset, text, shadow, shadow);
    gui_window_draw_text(NULL, x, y, text, fg, shadow);
}

/* Outlined text */
void gui_draw_text_outline(int32_t x, int32_t y, const char *text,
                            gui_color_t fg, gui_color_t outline) {
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            if (dx || dy) gui_window_draw_text(NULL, x + dx, y + dy, text, outline, outline);
        }
    }
    gui_window_draw_text(NULL, x, y, text, fg, outline);
}

/* Hex grid */
void gui_draw_hex_grid(int32_t x, int32_t y, int cols, int rows, int r, gui_color_t color) {
    int dx = (int)((float)r * 1.732f), dy = (int)((float)r * 1.5f);
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            int hx = x + col * dx + (row % 2) * dx / 2;
            int hy = y + row * dy;
            for (int i = 0; i < 6; i++) {
                int a1 = i * 60 - 30, a2 = (i + 1) * 60 - 30;
                int x1 = hx + r * __icos(a1) / 100, y1 = hy + r * __isin(a1) / 100;
                int x2 = hx + r * __icos(a2) / 100, y2 = hy + r * __isin(a2) / 100;
                gui_draw_line(x1, y1, x2, y2, color);
            }
        }
    }
}

/* Sine wave */
void gui_draw_sine_wave(int32_t x, int32_t y, uint32_t w, uint32_t h,
                         int amplitude, int frequency, gui_color_t color) {
    int cy = y + (int32_t)h / 2;
    int prev_y = cy;
    for (uint32_t xi = 0; xi < w; xi++) {
        int ang = (int)xi * frequency * 360 / (int)w;
        int sy = cy + amplitude * __isin(ang) / 100;
        if (xi > 0) gui_draw_line(x + (int32_t)xi - 1, prev_y, x + (int32_t)xi, sy, color);
        prev_y = sy;
    }
}

/* Checkerboard custom */
void gui_draw_checkerboard_custom(int32_t x, int32_t y, uint32_t w, uint32_t h,
                                   int cell_size, gui_color_t c1, gui_color_t c2) {
    if (cell_size <= 0) cell_size = 8;
    for (uint32_t yi = 0; yi < h; yi++)
        for (uint32_t xi = 0; xi < w; xi++) {
            int cx = ((int32_t)xi / cell_size) & 1;
            int cy = ((int32_t)yi / cell_size) & 1;
            __put(x + (int32_t)xi, y + (int32_t)yi, (cx ^ cy) ? c1 : c2);
        }
}

/* Image flip */
void gui_draw_image_flip(int32_t x, int32_t y, uint32_t w, uint32_t h,
                          const gui_color_t *pixels, int flip_h, int flip_v) {
    if (!pixels) return;
    for (uint32_t yi = 0; yi < h; yi++)
        for (uint32_t xi = 0; xi < w; xi++) {
            uint32_t sx = flip_h ? w - 1 - xi : xi;
            uint32_t sy = flip_v ? h - 1 - yi : yi;
            __put(x + (int32_t)xi, y + (int32_t)yi, pixels[sy * w + sx]);
        }
}

/* Rounded rect outline */
void gui_draw_rounded_rect_outline(gui_rect_t rect, int radius, gui_color_t color, int thickness) {
    for (int t = 0; t < thickness; t++) {
        gui_rect_t r = {rect.x + t, rect.y + t, rect.w - t*2, rect.h - t*2};
        gui_draw_rounded_rect(r, radius - t, color);
    }
}

/* Text bounding box */
gui_rect_t gui_text_bounding_box(const char *text) {
    gui_rect_t r = {0, 0, (uint32_t)(text ? strlen(text) * 12 : 0), 14};
    return r;
}

/* Progress arc */
void gui_draw_progress_arc(int32_t cx, int32_t cy, int r, int thickness,
                            int percent, gui_color_t fg, gui_color_t bg) {
    gui_draw_circle(cx, cy, r, bg);
    int end = percent * 360 / 100;
    for (int a = 0; a < end; a += 5) {
        int ax = cx + r * __icos(a - 90) / 100;
        int ay = cy + r * __isin(a - 90) / 100;
        gui_draw_circle_filled(ax, ay, thickness, fg);
    }
}

/* Cursor block */
static int g_cursor_blink = 0;
void gui_draw_cursor_block(int32_t x, int32_t y, int w, int h, gui_color_t color, int blink_on) {
    (void)blink_on;
    g_cursor_blink = !g_cursor_blink;
    if (g_cursor_blink)
        gui_window_draw_rect(NULL, (gui_rect_t){x, y, w, h}, color);
}

